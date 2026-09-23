"""Explicit boundary repair, using only independently constructed x86 code."""
import copy
import json

import pytest

from . import config
from .translator import BatchTranslator, FunctionTranslator, load_coalescences


BASE = 0x10000
# Select a cap, then return min(eax, cap). Both CMPs feed the shared JGE.
# test ecx,ecx; jz short; mov edi,12; cmp eax,edi; jmp join;
# short: mov edi,6; cmp eax,edi; join: jge done; mov edi,eax;
# done: mov eax,edi; ret
CLAMP = bytes.fromhex("85c97409bf0c00000039f8eb07bf0600000039f87d0289c789f8c3")
SPLITS = [BASE + 13, BASE + 20, BASE + 24]
END = BASE + len(CLAMP)


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
    ])


def function(start, end):
    return {"_addr": start, "start": hex(start), "end": end,
            "size": end - start, "section": ".text"}


def translator(body=CLAMP, splits=SPLITS):
    bounds = [BASE, *splits, BASE + len(body)]
    db = {start: function(start, end)
          for start, end in zip(bounds, bounds[1:])}
    return FunctionTranslator(body.ljust(0x400, b"\xcc"), db)


def test_repaired_clamp_matches_unsplit_translation():
    split = translator()
    old = split.translate_function(SPLITS[1], split.func_db[SPLITS[1]])
    assert "if (_flags /* jge" in old
    # Translation does not discover ownership, so it is safe to recover here.
    split.coalesce_function(BASE, END, SPLITS)
    repaired = split.translate_function(BASE, split.func_db[BASE])
    whole = translator(splits=[])
    reference = whole.translate_function(BASE, whole.func_db[BASE])
    assert repaired == reference
    assert "CMP_GE(" in repaired
    assert "if (_flags /* jge" not in repaired
    assert list(split.func_db) == [BASE]
    assert split.coalesced_function_starts == {BASE}
    assert split.func_db[BASE]["detection_method"] == "external_coalescence"


def test_removes_stale_fragment_ownership():
    subject = translator()
    subject._recovered_cfg[SPLITS[0]] = {"end": END}
    subject.owned_function_starts.add(SPLITS[0])
    subject.recovered_function_starts.add(SPLITS[0])
    subject.coalesce_function(BASE, END, SPLITS)
    assert SPLITS[0] not in subject._recovered_cfg
    assert SPLITS[0] not in subject.owned_function_starts
    assert SPLITS[0] not in subject.recovered_function_starts


@pytest.mark.parametrize("starts", [[], SPLITS[:-1], SPLITS[::-1],
                                   [SPLITS[0], *SPLITS], [BASE, *SPLITS]])
def test_requires_exact_sorted_interior_census(starts):
    subject = translator()
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError):
        subject.coalesce_function(BASE, END, starts)
    assert subject.func_db == before
    assert not subject._recovered_cfg


@pytest.mark.parametrize("evidence", [
    {"has_prologue": True}, {"called_by": [hex(BASE + 0x100)]},
    {"external_entry": True},
    {"has_prologue": True, "seed_derived": True},
    {"detection_method": "tail_jump_target"},
    {"detection_method": "tail_jump_alias"},
    {"detection_method": "imm_ref_target"},
    {"detection_method": "data_ptr_target"},
    {"detection_method": "indirect_call_slot"},
    {"detection_method": "seed_vtable_thunk"},
    {"detection_method": "entry_point"},
    {"detection_method": "call_target"},
    {"detection_method": "prologue"},
    {"detection_method": "prologue_alt"},
    {"detection_method": "static_indirect_table"},
])
def test_independent_entry_evidence_is_never_discarded(evidence):
    subject = translator()
    subject.func_db[SPLITS[0]].update(evidence)
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError, match="independent evidence"):
        subject.coalesce_function(BASE, END, SPLITS)
    assert subject.func_db == before


def test_rejects_call_to_interior_even_with_incomplete_metadata():
    # call inner; jmp inner; inner: ret
    subject = translator(bytes.fromhex("e802000000eb00c3"), [BASE + 7])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + 8, [BASE + 7])


@pytest.mark.parametrize("padding", ["6690", "8bff", "8d1b", "8da4240000000090"])
def test_only_proven_alignment_padding_closes_a_decode_gap(padding):
    pad = bytes.fromhex(padding)
    body = bytes([0xeb, len(pad)]) + pad + b"\xc3"
    interior = BASE + 2 + len(pad)
    subject = translator(body, [interior])
    subject.coalesce_function(BASE, BASE + len(body), [interior])
    assert subject._recovered_cfg[BASE]["end"] == BASE + len(body)


@pytest.mark.parametrize("live", ["31c0", "8bfe"])
def test_unreached_live_instructions_are_not_alignment_padding(live):
    subject = translator(bytes.fromhex("eb02" + live + "c3"), [BASE + 4])
    with pytest.raises(ValueError, match="CFG gap"):
        subject.coalesce_function(BASE, BASE + 5, [BASE + 4])


@pytest.mark.parametrize("trap", ["cc", "0f0b", "f4"])
@pytest.mark.parametrize("has_other_edge", [False, True])
def test_traps_do_not_prove_reachability_of_a_following_entry(trap, has_other_edge):
    trap = bytes.fromhex(trap)
    prefix = b"\x85\xc9\x74" + bytes([len(trap)]) if has_other_edge else b""
    interior = BASE + len(prefix) + len(trap)
    body = prefix + trap + bytes.fromhex("b807000000c3")
    subject = translator(body, [interior])
    end = BASE + len(body)
    # Ordinary recovery retains its pre-coalescence behavior.
    assert subject._recover_cfg(BASE, end, set(), set())[0][-1].end_address == end
    if has_other_edge:
        subject.coalesce_function(BASE, end, [interior])
        assert list(subject.func_db) == [BASE]
    else:
        before = copy.deepcopy(subject.func_db)
        with pytest.raises(ValueError, match="not the requested end"):
            subject.coalesce_function(BASE, end, [interior])
        assert subject.func_db == before


def test_default_ownership_does_not_follow_base_only_tables():
    raw = bytearray(b"\xcc" * 0x400)
    # test ecx,ecx; jz bridge; jmp [eax*4 + indexed_table]
    raw[:11] = bytes.fromhex("85c9742cff2485") + (BASE + 0x100).to_bytes(4, "little")
    raw[0x20:0x22] = b"\xc3\xc3"
    raw[0x30:0x36] = b"\xff\xa0" + (BASE + 0x120).to_bytes(4, "little")
    raw[0x40:0x42] = b"\xc3\xc3"
    raw[0x200] = 0xc3
    for offset, target in zip((0x100, 0x104, 0x120, 0x124),
                              (0x20, 0x21, 0x40, 0x41)):
        raw[offset:offset + 4] = (BASE + target).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    for start, end in ((0, 11), (0x30, 0x36), (0x40, 0x42), (0x200, 0x201)):
        subject.func_db[BASE + start] = function(BASE + start, BASE + end)
    subject.func_db[BASE]["called_by"] = [hex(BASE + 0x200)]
    subject.func_db[BASE + 0x200]["has_prologue"] = True
    subject.discover_cfg_ownership()
    assert subject.owned_function_starts == {BASE + 0x30}


@pytest.mark.parametrize("jump", ["ff2485", "ffa0"])
@pytest.mark.parametrize("has_next_function", [True, False])
def test_jump_table_can_follow_owned_code(jump, has_next_function):
    # jmp [eax*4 + table] or jmp [eax + table] (pre-scaled index).
    prefix = bytes.fromhex("31c0" + jump)
    first_case = BASE + len(prefix) + 4
    end = first_case + 4
    body = (prefix + end.to_bytes(4, "little")
            + bytes.fromhex("40c34bc3")
            + first_case.to_bytes(4, "little")
            + (first_case + 2).to_bytes(4, "little"))
    subject = translator(body, [first_case])
    subject.func_db[first_case]["end"] = end
    if has_next_function:
        subject.func_db[BASE + 0x100] = function(BASE + 0x100, BASE + 0x101)
    subject.coalesce_function(BASE, end, [first_case])
    recovered = subject._recovered_cfg[BASE]
    assert recovered["end"] == end
    assert recovered["jump_tables"][end] == [first_case, first_case + 2]
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert "switch: 2 entries, 2 targets" in code
    assert f"loc_{first_case:08X}:" in code
    assert f"loc_{first_case + 2:08X}:" in code


@pytest.mark.parametrize("mutation, message", [
    (lambda s: s.func_db[BASE].update(end=END + 1), "shrinks"),
    (lambda s: s.func_db[SPLITS[0]].update(end=END + 1), "crosses end"),
    (lambda s: s.func_db.update({BASE - 1: function(BASE - 1, BASE + 1)}),
     "preceding function overlaps"),
    (lambda s: setattr(s, "_ownership_ready", True), "before discovering"),
])
def test_rejects_conflicting_ownership(mutation, message):
    subject = translator()
    mutation(subject)
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError, match=message):
        subject.coalesce_function(BASE, END, SPLITS)
    assert subject.func_db == before


def test_rejects_extent_outside_backed_code():
    with pytest.raises(ValueError, match="one code section"):
        translator().coalesce_function(BASE, BASE + 0x401, SPLITS)


def test_rejects_unproven_end():
    with pytest.raises(ValueError, match="not the requested end"):
        translator().coalesce_function(BASE, END + 1, SPLITS)


@pytest.mark.parametrize("entry", [
    {}, [None], [{"start": "0x10000", "end": "0x1001b"}],
    [{"start": 0x10000, "end": "0x1001b", "coalesce_starts": []}],
    [{"start": "0x100000000", "end": "0x1001b", "coalesce_starts": []}],
    [{"start": "0x10000", "end": "invalid", "coalesce_starts": []}],
    [{"start": "0x10000", "end": "0x1001b", "coalesce_starts": "0x1000d"}],
])
def test_invalid_recovery_json_fails_closed(tmp_path, entry):
    path = tmp_path / "bounds.json"
    path.write_text(json.dumps(entry), encoding="utf-8")
    with pytest.raises(ValueError):
        load_coalescences(path)


def batch_translator(tmp_path, subject, end, splits, coalesce=True, **kwargs):
    entries = list(copy.deepcopy(subject.func_db).values())
    for entry in entries:
        entry["end"] = hex(entry["end"])
    image = tmp_path / "synthetic.xbe"
    functions = tmp_path / "functions.json"
    bounds = tmp_path / "bounds.json"
    image.write_bytes(subject.xbe_data)
    functions.write_text(json.dumps(entries), encoding="utf-8")
    bounds.write_text(json.dumps([{
        "start": hex(BASE), "end": hex(end),
        "coalesce_starts": [hex(start) for start in splits],
    }]), encoding="utf-8")
    return BatchTranslator(image, functions,
                           coalesce_json_paths=[bounds] if coalesce else None,
                           **kwargs)


def test_batch_wires_repair_before_ownership_and_emission(tmp_path):
    batch = batch_translator(tmp_path, translator(), END, SPLITS,
                             seh_prolog=0, seh_epilog=0)
    assert list(batch.func_db) == [BASE]
    assert "CMP_GE(" in batch.translate_single(BASE)
    assert batch.translate_single(SPLITS[0]) is None


@pytest.mark.parametrize("helper, body, split, call_code", [
    # Marker instructions straddle the split for SEH; the non-local jump
    # marker is entirely in the deleted fragment for setjmp/longjmp.
    ("SEH_PROLOG", "64a1000000008d6c2410c3", 6,
     "read back frame from SEH helper"),
    ("SEH_EPILOG", "64890d00000000c951c3", 7,
     "read back frame from SEH helper"),
    ("SETJMP_FN", "90c7422030324356c3", 1, "setjmp(*recomp_setjmp_slot"),
    ("LONGJMP_FN", "903d30324356c3", 1, "recomp_guest_longjmp("),
])
def test_batch_detects_helpers_from_repaired_owner(
        tmp_path, helper, body, split, call_code):
    body = bytes.fromhex(body)
    subject = translator(body, [BASE + split])
    caller = BASE + 0x100
    call = (b"\xe8" + (BASE - caller - 5).to_bytes(4, "little", signed=True)
            + b"\xc3")
    subject.xbe_data = (subject.xbe_data[:0x100] + call
                        + subject.xbe_data[0x100 + len(call):])
    subject.func_db[caller] = function(caller, caller + len(call))
    batch = batch_translator(tmp_path, subject, BASE + len(body), [BASE + split])
    assert getattr(batch.translator.lifter, helper) == BASE
    assert call_code in batch.translate_single(caller)
    if helper.startswith("SEH_"):
        assert getattr(batch, helper.lower()) == BASE


@pytest.mark.parametrize("override", [0, BASE + 0x200])
@pytest.mark.parametrize("helper", ["seh_prolog", "seh_epilog"])
def test_batch_preserves_seh_overrides_after_repair(tmp_path, helper, override):
    body = bytes.fromhex("64a1000000008d6c2410c3")
    batch = batch_translator(tmp_path, translator(body, [BASE + 6]),
                             BASE + len(body), [BASE + 6], **{helper: override})
    assert getattr(batch, helper) == override
    assert getattr(batch.translator.lifter, helper.upper()) == override


@pytest.mark.parametrize("helper, body", [
    ("SEH_PROLOG", "64a1000000008d6c2410c3"),
    ("SEH_EPILOG", "64890d00000000c951c3"),
    ("SETJMP_FN", "c7422030324356c3"),
    ("LONGJMP_FN", "3d30324356c3"),
])
def test_default_helper_detection_precedes_static_callback_discovery(
        tmp_path, helper, body):
    raw = bytearray(b"\xcc" * 0x400)
    # mov esi, table; mov edi, table+4; cmp esi,edi; call eax; ret
    raw[:15] = (b"\xbe" + (BASE + 0x300).to_bytes(4, "little")
                + b"\xbf" + (BASE + 0x304).to_bytes(4, "little")
                + bytes.fromhex("39feffd0c3"))
    body = bytes.fromhex(body)
    raw[0x40:0x40 + len(body)] = body
    raw[0x80] = 0xc3
    raw[0x300:0x304] = (BASE + 0x40).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({BASE: function(BASE, BASE + 15),
                           BASE + 0x80: function(BASE + 0x80, BASE + 0x81)})
    batch = batch_translator(tmp_path, subject, BASE + 15, [], coalesce=False)
    assert BASE + 0x40 in batch.translator.recovered_function_starts
    assert getattr(batch.translator.lifter, helper) is None
