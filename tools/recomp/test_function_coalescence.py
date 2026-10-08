"""Explicit boundary repair, using only independently constructed x86 code."""
import copy
import json

import pytest

from . import __main__ as recomp_main
from . import config
from . import manual_scan
from .disasm import Instruction
from .lifter import Lifter
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


def test_rejects_previously_coalesced_interior_owner():
    subject = translator()
    subject.coalesce_function(SPLITS[0], END, SPLITS[1:])
    assert subject.func_db[SPLITS[0]]["detection_method"] == "external_coalescence"
    with pytest.raises(ValueError, match="independent evidence"):
        subject.coalesce_function(BASE, END, [SPLITS[0]])


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


@pytest.mark.parametrize("method, body, split", [
    ("cc_boundary", "c3ccccb807000000c3", 3),
    ("gap_prologue", "b801000000c3b807000000c3", 6),
])
def test_layout_heuristics_do_not_allow_merging_unreachable_functions(
        method, body, split):
    body = bytes.fromhex(body)
    subject = translator(body, [BASE + split])
    subject.func_db[BASE + split]["detection_method"] = method
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [BASE + split])
    assert subject.func_db == before


def test_rejects_call_to_interior_even_with_incomplete_metadata():
    # call inner; jmp inner; inner: ret
    subject = translator(bytes.fromhex("e802000000eb00c3"), [BASE + 7])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + 8, [BASE + 7])


def test_rejects_register_call_to_interior_even_with_local_branch():
    interior = BASE + 9
    body = (b"\xb8" + interior.to_bytes(4, "little")
            + bytes.fromhex("ffd0eb0040c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_rejects_lea_register_call_to_interior():
    interior = BASE + 10
    body = (bytes.fromhex("8d05") + interior.to_bytes(4, "little")
            + bytes.fromhex("ffd0eb00c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_rejects_memory_indirect_call_to_interior():
    interior = BASE + 12
    body = (bytes.fromhex("c70424") + interior.to_bytes(4, "little")
            + bytes.fromhex("ff1424eb00c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_rejects_bounded_memory_indirect_callback_to_interior():
    table = BASE + 0x300
    interior = BASE + 16
    body = (b"\xbe" + table.to_bytes(4, "little")
            + b"\xbf" + (table + 4).to_bytes(4, "little")
            + bytes.fromhex("39feff16eb00c3"))
    subject = translator(body, [interior])
    raw = bytearray(subject.xbe_data)
    raw[0x300:0x304] = interior.to_bytes(4, "little")
    subject.xbe_data = bytes(raw)

    with pytest.raises(ValueError, match="callback"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_rejects_spilled_reload_register_call_to_interior():
    interior = BASE + 16
    body = (bytes.fromhex("c7442404") + interior.to_bytes(4, "little")
            + bytes.fromhex("8b442404ffd0eb00c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_rejects_exact_memory_indirect_jump_to_interior():
    interior = BASE + 12
    body = (bytes.fromhex("740a")
            + bytes.fromhex("c70424") + interior.to_bytes(4, "little")
            + bytes.fromhex("ff2424c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_observed_dynamic_path_retains_indirect_call_evidence():
    block = BASE + 16
    interior = BASE + 20
    body = (bytes.fromhex("85d27410")
            + b"\xb9" + interior.to_bytes(4, "little")
            + b"\xb8" + block.to_bytes(4, "little")
            + bytes.fromhex("ffe0ffd1ebfac3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_segmented_jump_table_is_not_coalescence_evidence(monkeypatch):
    table = BASE + 0x1000
    first_case = BASE + 8
    second_case = BASE + 10
    end = BASE + 12
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
        config.Section(".rdata", table, 0x100, 0x400, 0x100, False),
    ])
    image = bytearray(b"\xcc" * 0x500)
    image[:12] = (bytes.fromhex("64ff2485") + table.to_bytes(4, "little")
                  + bytes.fromhex("40c34bc3"))
    image[0x400:0x408] = (
        first_case.to_bytes(4, "little")
        + second_case.to_bytes(4, "little"))
    subject = FunctionTranslator(bytes(image), {
        BASE: function(BASE, first_case),
        first_case: function(first_case, second_case),
        second_case: function(second_case, end),
    })

    with pytest.raises(ValueError, match="segmented indirect jump"):
        subject.coalesce_function(BASE, end, [first_case, second_case])


def test_flat_segment_jump_table_still_recovers(monkeypatch):
    table = BASE + 0x1000
    first_case = BASE + 8
    second_case = BASE + 10
    end = BASE + 12
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
        config.Section(".rdata", table, 0x100, 0x400, 0x100, False),
    ])
    image = bytearray(b"\xcc" * 0x500)
    image[:12] = (bytes.fromhex("2eff2485") + table.to_bytes(4, "little")
                  + bytes.fromhex("40c34bc3"))
    image[0x400:0x408] = (
        first_case.to_bytes(4, "little")
        + second_case.to_bytes(4, "little"))
    subject = FunctionTranslator(bytes(image), {
        BASE: function(BASE, first_case),
        first_case: function(first_case, second_case),
        second_case: function(second_case, end),
    })

    subject.coalesce_function(BASE, end, [first_case, second_case])

    assert subject._recovered_cfg[BASE]["jump_tables"][table] == [
        first_case, second_case]


def test_segmented_memory_store_does_not_alias_plain_indirect_call():
    interior = BASE + 13
    body = (bytes.fromhex("64c70424") + interior.to_bytes(4, "little")
            + bytes.fromhex("ff1424eb00c3"))
    subject = translator(body, [interior])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    _, calls = subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True,
        return_call_refs=True)
    assert interior not in calls


def test_reassigning_memory_base_invalidates_indirect_call_slot():
    interior = BASE + 13
    body = (bytes.fromhex("c700") + interior.to_bytes(4, "little")
            + bytes.fromhex("b800200100ff10c3"))
    subject = translator(body, [])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    _, calls = subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True,
        return_call_refs=True)
    assert interior not in calls


def test_push_register_tracks_new_stack_indirect_call_target():
    interior = BASE + 11
    body = (b"\xbb" + interior.to_bytes(4, "little")
            + bytes.fromhex("53ff1424eb00c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_pop_carried_target_is_indirect_call_evidence():
    interior = BASE + 10
    body = (b"\x68" + interior.to_bytes(4, "little")
            + bytes.fromhex("58ffd0eb00c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_push_invalidates_other_symbolic_stack_slots():
    interior = BASE + 13
    body = (bytes.fromhex("c745fc") + interior.to_bytes(4, "little")
            + bytes.fromhex("50ff55fceb00c3"))
    subject = translator(body, [])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    _, calls = subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True,
        return_call_refs=True)
    assert interior not in calls


@pytest.mark.parametrize("padding", ["6690", "8bff", "8d1b", "8da4240000000090"])
def test_only_proven_alignment_padding_closes_a_decode_gap(padding):
    pad = bytes.fromhex(padding)
    body = bytes([0xeb, len(pad)]) + pad + b"\xc3"
    interior = BASE + 2 + len(pad)
    subject = translator(body, [interior])
    subject.coalesce_function(BASE, BASE + len(body), [interior])
    assert subject._recovered_cfg[BASE]["end"] == BASE + len(body)


def test_unreachable_padding_does_not_discard_incoming_comparison_flags():
    # cmp eax,6; jmp join; nop (alignment); join: jge done; mov eax,6; done: ret
    body = bytes.fromhex("83f806eb0266907d05b806000000c3")
    subject = translator(body, [BASE + 7])
    subject.coalesce_function(BASE, BASE + len(body), [BASE + 7])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert "CMP_GE(" in code
    assert "if (_flags /* jge" not in code
    assert BASE + 5 not in {
        insn.address for insn in subject._recovered_cfg[BASE]["instructions"]}


def test_jump_table_with_surrounding_alignment_padding_closes_gap():
    prefix_size = 9
    before = bytes.fromhex("6690")
    after = bytes.fromhex("6690")
    table = BASE + prefix_size + len(before)
    first_case = table + 8 + len(after)
    body = (bytes.fromhex("31c0ff2485") + table.to_bytes(4, "little")
            + before
            + first_case.to_bytes(4, "little")
            + (first_case + 2).to_bytes(4, "little")
            + after + bytes.fromhex("40c34bc3"))
    subject = translator(body, [first_case])
    subject.coalesce_function(BASE, BASE + len(body), [first_case])
    recovered = subject._recovered_cfg[BASE]
    assert recovered["jump_tables"][table] == [first_case, first_case + 2]


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
        _, blocks = subject.decode_function(BASE, end)
        trap_block = next(bb for bb in blocks if bb.last_insn.mnemonic
                          in ("int3", "ud2", "hlt"))
        assert trap_block.successors == []
        code = subject.translate_function(BASE, subject.func_db[BASE])
        assert "return;" in code.split(f"loc_{interior:08X}:")[0]
    else:
        before = copy.deepcopy(subject.func_db)
        with pytest.raises(ValueError, match="not the requested end"):
            subject.coalesce_function(BASE, end, [interior])
        assert subject.func_db == before


@pytest.mark.parametrize("iret", ["cf", "66cf"])
def test_interrupt_return_is_terminal_for_recovery_and_emission(iret):
    iret = bytes.fromhex(iret)
    interior = BASE + len(iret)
    body = iret + bytes.fromhex("40c3")
    subject = translator(body, [interior])

    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])

    # Emission must not abort the build: a linear sweep reads iretd out of
    # data (Wreckless has one at 0x001345A6), so the site becomes a runtime
    # RECOMP_UNIMPL marker like any other untranslatable instruction.
    whole = translator(body, [])
    whole.translate_function(BASE, whole.func_db[BASE])
    insn = Instruction(BASE, len(iret), "iretd", "", iret.hex(), operands=[])
    assert "RECOMP_UNIMPL" in " ".join(Lifter().lift_instruction(insn))


def test_xbox_int2d_int3_slide_preserves_fallthrough():
    continuation = BASE + 3
    body = bytes.fromhex("cd2dccb807000000c3")
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    prefix = code.split(f"loc_{continuation:08X}:")[0]
    assert "recomp_debug_service(eax, ecx)" in prefix
    assert "trap ends recovered control flow" not in prefix


def test_targeted_int3_traps_but_int2d_path_bypasses_it():
    continuation = BASE + 5
    int3 = BASE + 4
    body = bytes.fromhex("7402cd2dcc40c3")
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    before_int3, after_int3 = code.split(f"loc_{int3:08X}:", 1)
    int3_body, _ = after_int3.split(f"loc_{continuation:08X}:", 1)
    assert f"goto loc_{continuation:08X}; /* int 0x2d skips slide int3 */" in before_int3
    assert "return; /* trap ends recovered control flow */" in int3_body


def test_computed_entry_into_int3_traps_but_int2d_path_bypasses_it():
    int2d = BASE + 11
    int3 = BASE + 13
    continuation = BASE + 14
    body = (bytes.fromhex("85c97407")
            + b"\xb8" + int3.to_bytes(4, "little")
            + bytes.fromhex("ffe0cd2dcc40c3"))
    subject = translator(body, [int2d, int3, continuation])
    subject.coalesce_function(
        BASE, BASE + len(body), [int2d, int3, continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    before_int3, after_int3 = code.split(f"loc_{int3:08X}:", 1)
    int3_body, _ = after_int3.split(f"loc_{continuation:08X}:", 1)
    assert f"goto loc_{continuation:08X}; /* int 0x2d skips slide int3 */" in before_int3
    assert "return; /* trap ends recovered control flow */" in int3_body


def test_noncoalesced_int2d_int3_does_not_tail_fallthrough():
    next_start = BASE + 3
    body = bytes.fromhex("cd2dcc40c3")
    subject = translator(body, [next_start])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert "fallthrough" not in code


def ownership_subject():
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
    return subject


def test_default_ownership_does_not_follow_base_only_tables():
    subject = ownership_subject()
    subject.discover_cfg_ownership()
    assert subject.owned_function_starts == {BASE + 0x30}


def test_ownership_keeps_protected_entries_standalone():
    subject = ownership_subject()
    subject.protected_function_starts.add(BASE + 0x30)
    subject.discover_cfg_ownership()
    assert not subject.owned_function_starts
    assert not subject._recovered_cfg


def test_ownership_stops_at_a_protected_fallthrough():
    subject = ownership_subject()
    # Both table arms now run a nop and fall through into a protected entry
    # that no branch names.
    raw = bytearray(subject.xbe_data)
    raw[0x20:0x22] = b"\x90\xc3"
    raw[0x104:0x108] = (BASE + 0x20).to_bytes(4, "little")
    subject.xbe_data = bytes(raw)
    subject.func_db[BASE + 0x21] = function(BASE + 0x21, BASE + 0x22)
    subject.protected_function_starts.add(BASE + 0x21)
    subject.discover_cfg_ownership()
    assert not subject.owned_function_starts
    assert not subject._recovered_cfg


def test_explicit_coalesced_owner_stays_strong_during_ownership():
    subject = ownership_subject()
    owner = BASE + 0x30
    subject.func_db[owner]["detection_method"] = "external_coalescence"
    subject.coalesced_function_starts.add(owner)

    subject.discover_cfg_ownership()

    assert owner not in subject.owned_function_starts


def test_explicit_coalesced_owner_is_not_reexpanded_by_ownership():
    subject = ownership_subject()
    subject.func_db[BASE]["detection_method"] = "external_coalescence"
    subject.coalesced_function_starts.add(BASE)
    preserved = {
        "end": BASE + 11,
        "instructions": ["explicit-repair"],
        "jump_tables": {},
    }
    subject._recovered_cfg[BASE] = copy.deepcopy(preserved)

    subject.discover_cfg_ownership()

    assert BASE + 0x30 not in subject.owned_function_starts
    assert subject._recovered_cfg[BASE] == preserved


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


def test_coalescence_recovers_external_jump_table(monkeypatch):
    table = BASE + 0x1000
    first_case = BASE + 7
    second_case = BASE + 9
    end = BASE + 11
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
        config.Section(".rdata", table, 0x100, 0x400, 0x100, False),
    ])
    image = bytearray(b"\xcc" * 0x500)
    image[:11] = (bytes.fromhex("ff2485") + table.to_bytes(4, "little")
                  + bytes.fromhex("40c34bc3"))
    image[0x400:0x408] = (
        first_case.to_bytes(4, "little")
        + second_case.to_bytes(4, "little"))
    image[0x408:0x40c] = (0).to_bytes(4, "little")
    subject = FunctionTranslator(bytes(image), {
        BASE: function(BASE, first_case),
        first_case: function(first_case, second_case),
        second_case: function(second_case, end),
    })

    subject.coalesce_function(BASE, end, [first_case, second_case])

    assert subject._recovered_cfg[BASE]["jump_tables"][table] == [
        first_case, second_case]
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"loc_{first_case:08X}:" in code
    assert f"loc_{second_case:08X}:" in code


def test_coalescence_rejects_non_dword_jump_table_stride(monkeypatch):
    table = BASE + 0x1000
    first_case = BASE + 7
    second_case = BASE + 9
    end = BASE + 11
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
        config.Section(".rdata", table, 0x100, 0x400, 0x100, False),
    ])
    image = bytearray(b"\xcc" * 0x500)
    image[:11] = (bytes.fromhex("ff24c5") + table.to_bytes(4, "little")
                  + bytes.fromhex("40c34bc3"))
    image[0x400:0x408] = (
        first_case.to_bytes(4, "little")
        + second_case.to_bytes(4, "little"))
    subject = FunctionTranslator(bytes(image), {
        BASE: function(BASE, first_case),
        first_case: function(first_case, second_case),
        second_case: function(second_case, end),
    })

    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, end, [first_case, second_case])


def test_static_callback_rescan_uses_recovered_cfg():
    table = BASE + 0x300
    callback = BASE + 0x80
    raw = bytearray(b"\xcc" * 0x400)
    raw[0] = 0xC3
    raw[0x80] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        callback: function(callback, callback + 1),
    })
    subject._recovered_cfg[BASE] = {
        "end": BASE + len(pattern),
        "instructions": subject.disasm.disassemble_function(
            pattern, BASE, BASE + len(pattern)),
        "jump_tables": {},
    }

    subject.discover_static_indirect_targets(coalescing=True)

    assert subject.func_db[callback]["called_by"] == [f"0x{BASE:08X}"]


def static_callback_subject(cover, callback_code, inner=None):
    # An _initterm-style caller walks [table, table+4); its one callback sits
    # after a gap alias (or a real function) whose end runs past it. `inner`
    # is an optional alias start inside the callback's own code.
    table = BASE + 0x300
    alias = BASE + 0x40
    callback = BASE + 0x80
    following = BASE + 0x100
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x40] = raw[0x100] = 0xC3
    raw[0x80:0x80 + len(callback_code)] = callback_code
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        alias: {**function(alias, following), "detection_method": cover},
        following: function(following, following + 1),
    })
    if inner is not None:
        subject.func_db[inner] = {
            **function(inner, following), "detection_method": "tail_jump_alias"}
    subject.discover_static_indirect_targets()
    return subject, callback


@pytest.mark.parametrize("cover, recovered", [
    ("tail_jump_alias", True), ("prologue", False)])
def test_static_callback_inside_alias_range_is_recovered(cover, recovered):
    # Only a real function's range may hide the callback.
    subject, callback = static_callback_subject(cover, b"\xc3")
    assert (callback in subject.func_db) is recovered
    if recovered:
        assert subject.func_db[callback]["end"] == callback + 1
        assert subject.func_db[callback]["detection_method"] == (
            "static_indirect_table")


@pytest.mark.parametrize("inner, recovered", [
    (BASE + 0x84, True), (BASE + 0x83, False)])
def test_static_callback_may_fall_into_an_alias(inner, recovered):
    # nop x4 then the alias's ret: no ret before the alias start, but the
    # decode lands exactly on it. A start mid-instruction is not a fallthrough.
    code = b"\x90" * 4 + b"\xc3" if inner == BASE + 0x84 else b"\x90\x90\x05" + b"\x00" * 4
    subject, callback = static_callback_subject("tail_jump_alias", code, inner)
    assert (callback in subject.func_db) is recovered
    if recovered:
        assert subject.func_db[callback]["end"] == inner
        body = subject.translate_function(callback, subject.func_db[callback])
        assert f"sub_{inner:08X}" in body, body


@pytest.mark.parametrize("code, recovered", [
    (b"\xc3", True), (b"\xcc", False),
    (bytes.fromhex("ebfe"), True),  # closed non-returning task loop
    (bytes.fromhex("83e001ff2485") + (BASE + 0x90).to_bytes(4, "little")
     + bytes.fromhex("40ebf348ebf0")
     + (BASE + 0x8a).to_bytes(4, "little")
     + (BASE + 0x8d).to_bytes(4, "little"), True),  # both table arms loop
    (bytes.fromhex("85c074fceb7a"), True),  # loop can tail-call a known function
    (bytes.fromhex("85c074fceb79"), False),  # exit has no known entry
    (bytes.fromhex("85c0747cebfa"), True),  # conditional tail call to a known function
    (bytes.fromhex("85c0747bebfa"), False),  # conditional exit has no known entry
    (bytes.fromhex("85c0747c"), False),  # conditional tail call, trap fallthrough
    (bytes.fromhex("85c074fc"), False),  # loop with a trap fallthrough
    (bytes.fromhex("85c074fce877000000"), True),  # loop, then no-return call
    (bytes.fromhex("85c07402ebfac3"), True),  # loop that leaves through a ret
    (bytes.fromhex("ffe0"), True),  # tail call through a register
    (bytes.fromhex("ccc3"), False),  # a ret is decodable but not reachable
])
def test_immediate_callback_in_gap_is_recovered(code, recovered):
    # `push callback; call eax; ret`: the callback sits in a gap and has no
    # table. A closed CFG that returns or loops is valid; traps and escaping
    # edges are not, even when a ret decodes after them.
    callback = BASE + 0x80
    following = BASE + 0x100
    pattern = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x80:0x80 + len(code)] = code
    raw[0x100:0x102] = bytes.fromhex("ebfe")  # never returns, like ExitThread
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 2),
    })
    subject.discover_static_indirect_targets()
    assert (callback in subject.func_db) is recovered
    if recovered:
        assert subject.func_db[callback]["called_by"] == [BASE]


@pytest.mark.parametrize("cover", ["tail_jump_alias", "prologue"])
def test_immediate_inside_another_range_is_not_a_callback(cover):
    # A constant that lands in an alias's code decodes into its ret, but only
    # a callback table may claim bytes inside an alias's range. A real owner
    # keeps them unless its own CFG and tables provably end first.
    alias, constant, following = BASE + 0x40, BASE + 0x42, BASE + 0x100
    pattern = b"\x68" + constant.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x40:0x44] = bytes.fromhex("9090c3c3")
    raw[0x100] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        alias: {**function(alias, alias + 4), "detection_method": cover},
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert constant not in subject.func_db


# Unlisted code in the gap: push ebp; mov ebp, esp; mov eax, 0x41414141;
# pop ebp; ret. From +3 or +4 the bytes still decode to a closed ret.
UNLISTED = bytes.fromhex("558bec b841414141 5dc3".replace(" ", ""))


@pytest.mark.parametrize("offset, recovered", [
    (0, True),  # after int3 padding
    (3, False),  # an instruction boundary inside the function
    (4, False),  # mid-instruction
])
def test_immediate_inside_unlisted_code_is_not_a_callback(offset, recovered):
    unlisted, following = BASE + 0x20, BASE + 0x100
    constant = unlisted + offset
    pattern = b"\x68" + constant.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x20:0x20 + len(UNLISTED)] = UNLISTED
    raw[0x100] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert (constant in subject.func_db) is recovered


def test_branch_from_a_weak_callback_stays_weak():
    # A callback found only through an immediate calls into the middle of
    # unlisted code. That edge must pass the same checks as an immediate.
    unlisted, callback, following = BASE + 0x40, BASE + 0x80, BASE + 0x100
    pattern = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x40:0x40 + len(UNLISTED)] = UNLISTED
    raw[0x80:0x86] = (b"\xe8" + (unlisted + 4 - callback - 5).to_bytes(
        4, "little", signed=True) + b"\xc3")
    raw[0x100] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert unlisted + 4 not in subject.func_db


def test_unreachable_immediate_is_not_a_callback():
    # The push decodes after the caller's ret, so it never runs.
    callback, following = BASE + 0x80, BASE + 0x100
    pattern = b"\xc3\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x80] = raw[0x100] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback not in subject.func_db


def test_immediate_inside_a_callback_found_in_the_same_pass_is_rejected():
    # A table names the callback; a constant points at its second
    # instruction. Both are candidates in one pass, so original_starts cannot
    # show that the callback already owns those bytes.
    callback, following = BASE + 0x80, BASE + 0x100
    constant = callback + 1
    table = BASE + 0x300
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0")
               + b"\x68" + constant.to_bytes(4, "little") + b"\xc3")
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80:0x83] = bytes.fromhex("9090c3")
    raw[0x100] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert constant not in subject.func_db


def test_immediate_inside_a_same_pass_callback_table_is_rejected():
    # The callback's switch table follows its last instruction. A constant
    # pointing into the table must not become a function even though the
    # table bytes decode to a closed run ending in a ret.
    callback, following = BASE + 0x80, BASE + 0x100
    table = BASE + 0x90
    constant = table
    pattern = (b"\x68" + callback.to_bytes(4, "little")
               + b"\x68" + constant.to_bytes(4, "little") + bytes.fromhex("ffd0c3"))
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    code = (bytes.fromhex("83e001ff2485") + table.to_bytes(4, "little")
            + bytes.fromhex("40ebf348ebf0")
            + (BASE + 0x8a).to_bytes(4, "little")
            + (BASE + 0x8d).to_bytes(4, "little"))
    raw[0x80:0x80 + len(code)] = code
    raw[0x98] = 0xC3
    raw[0x100] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert constant not in subject.func_db


def test_recovered_callback_exposes_a_later_helper():
    # The callback calls a helper later in the same gap. The callback's range
    # ends with the code it reaches, so the helper gets a body of its own.
    callback, helper, following = BASE + 0x80, BASE + 0xa0, BASE + 0x100
    pattern = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x80:0x86] = b"\xe8" + (helper - callback - 5).to_bytes(4, "little") + b"\xc3"
    raw[0xa0] = raw[0x100] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert helper in subject.func_db
    assert subject.func_db[callback]["end"] == callback + 6


def test_callback_table_outside_its_range_claims_nothing_after_it(monkeypatch):
    # The callback's switch table lives in .data, past its range. Only code
    # and tables inside the callback are claimed, so a later immediate
    # callback in another gap is still recovered.
    callback, following, later, last = (
        BASE + 0x80, BASE + 0x100, BASE + 0x180, BASE + 0x200)
    table = BASE + 0x300
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x280, 0, 0x280, True),
        config.Section(".data", BASE + 0x280, 0x180, 0x280, 0x180, False),
    ])
    pattern = (b"\x68" + callback.to_bytes(4, "little")
               + b"\x68" + later.to_bytes(4, "little") + bytes.fromhex("ffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    code = (bytes.fromhex("83e001ff2485") + table.to_bytes(4, "little")
            + bytes.fromhex("40ebf348ebf0"))
    raw[0x80:0x80 + len(code)] = code
    raw[0x300:0x308] = ((BASE + 0x8a).to_bytes(4, "little")
                        + (BASE + 0x8d).to_bytes(4, "little"))
    raw[0x100] = raw[0x180] = raw[0x200] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
        last: function(last, last + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert later in subject.func_db


def test_callback_in_a_sections_last_gap_stops_at_its_section(monkeypatch):
    # The next start is in a later code section whose bytes are elsewhere in
    # the file, so decoding must stop at the callback's own section end.
    callback, second = BASE + 0x80, BASE + 0x1000
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x100, 0, 0x100, True),
        config.Section("LIB", second, 0x100, 0x100, 0x100, True),
    ])
    pattern = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x80] = 0xC3
    raw[0x100] = 0xC3
    subject = FunctionTranslator(bytes(raw), {
        BASE: function(BASE, BASE + len(pattern)),
        second: {**function(second, second + 1), "section": "LIB"},
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert subject.func_db[callback]["end"] == callback + 1
    assert subject.func_db[callback]["section"] == ".text"


def test_owner_with_a_data_switch_table_still_yields_a_later_callback(monkeypatch):
    # The owner indexes a table in .data, far above the callback. Only
    # embedded table storage can overlap the callback, so the owner is
    # trimmed and the callback gets a body.
    owner, callback, following = BASE + 0x40, BASE + 0x80, BASE + 0x100
    table = BASE + 0x300
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x280, 0, 0x280, True),
        config.Section(".data", BASE + 0x280, 0x180, 0x280, 0x180, False),
    ])
    registration = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(registration)] = registration
    body = bytes.fromhex("31c0ff2485") + table.to_bytes(4, "little")
    raw[0x40:0x40 + len(body)] = body
    raw[0x50] = raw[0x51] = raw[0x80] = raw[0x100] = 0xC3
    raw[0x300:0x308] = ((BASE + 0x50).to_bytes(4, "little")
                        + (BASE + 0x51).to_bytes(4, "little"))
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(registration)),
        owner: function(owner, callback + 1),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert subject.func_db[owner]["end"] == callback


def test_dependencies_come_only_from_reachable_callback_code():
    # A table callback returns at once; a call decoded after its ret cannot
    # run, so its target is not a dependency.
    callback, stray, following = BASE + 0x80, BASE + 0xc0, BASE + 0x100
    table = BASE + 0x300
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80:0x86] = b"\xc3\xe8" + (stray - callback - 6).to_bytes(4, "little")
    raw[0xc0] = raw[0x100] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert stray not in subject.func_db


def test_callback_conditional_tail_target_is_a_dependency():
    # The lifter emits a jcc that leaves the body as a tail call, so its
    # target needs a body just like a jmp target.
    callback, callee, following = BASE + 0x80, BASE + 0x40, BASE + 0x100
    table = BASE + 0x300
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80:0x89] = (bytes.fromhex("85c00f84")
                      + (callee - callback - 8).to_bytes(4, "little", signed=True)
                      + b"\xc3")
    raw[0x40] = raw[0x100] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert subject.func_db[callee]["called_by"] == [callback]


def test_callback_dependency_before_the_first_start_is_recovered():
    callee, register, callback, following = (
        BASE, BASE + 0x20, BASE + 0x80, BASE + 0x100)
    table = BASE + 0x300
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[0x20:0x20 + len(pattern)] = pattern
    raw[0x80:0x86] = b"\xe8" + (callee - callback - 5).to_bytes(4, "little", signed=True) + b"\xc3"
    raw[0] = raw[0x100] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        register: function(register, register + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert subject.func_db[callee]["called_by"] == [callback]


def test_callback_dependency_in_a_data_section_is_rejected(monkeypatch):
    # .data1 is data even though its name is not .rdata or .data.
    callback, stray, following = BASE + 0x80, BASE + 0x200, BASE + 0x100
    table = BASE + 0x300
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x200, 0, 0x200, True),
        config.Section(".data1", stray, 0x200, 0x200, 0x200, False),
    ])
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80:0x86] = b"\xe9" + (stray - callback - 5).to_bytes(4, "little") + b"\xc3"
    raw[0x100] = raw[0x200] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert stray not in subject.func_db


@pytest.mark.parametrize("coalescing", [False, True])
def test_callback_call_to_a_known_function_is_entry_evidence(coalescing):
    # A recovered callback calls an existing function directly. The callback
    # is new to the function list, so that call is caller evidence the
    # ownership pass and coalescence would otherwise lack.
    callback, known, following = BASE + 0x80, BASE + 0x40, BASE + 0x100
    pattern = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(pattern)] = pattern
    raw[0x40] = raw[0x100] = 0xC3
    raw[0x80:0x86] = b"\xe8" + (known - callback - 5).to_bytes(4, "little", signed=True) + b"\xc3"
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        known: function(known, known + 1),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets(coalescing=coalescing)
    assert callback in subject.func_db
    assert f"0x{callback:08X}" in subject.func_db[known]["called_by"]


def test_immediate_callback_after_the_last_start_is_recovered():
    callback = BASE + 0x80
    pattern = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80] = 0xC3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db[BASE] = function(BASE, BASE + len(pattern))
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert subject.func_db[callback]["end"] == callback + 1


def test_linear_callback_claims_its_embedded_table():
    # A table callback is accepted for its linear ret, so it has no saved
    # CFG. Its embedded switch table must still be claimed: the table bytes
    # decode to a closed run ending in ret, and a constant points at them.
    callback, table, following = BASE + 0x80, BASE + 0xa0, BASE + 0x100
    walk = BASE + 0x300
    pattern = (b"\xbe" + walk.to_bytes(4, "little")
               + b"\xbf" + (walk + 4).to_bytes(4, "little")
               + bytes.fromhex("39fe")
               + b"\x68" + table.to_bytes(4, "little") + bytes.fromhex("ffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80:0x8b] = (bytes.fromhex("83e001ff2485") + table.to_bytes(4, "little")
                      + b"\xc3")
    raw[0x8b] = 0xC3
    raw[0xa0:0xa8] = ((BASE + 0x8a).to_bytes(4, "little")
                      + (BASE + 0x8b).to_bytes(4, "little"))
    raw[0xa8] = raw[0x100] = 0xC3
    raw[0x300:0x304] = callback.to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert callback not in subject._recovered_cfg
    assert table not in subject.func_db


def _table_callback_subject(callback_code, extra=()):
    # An _initterm-style table names one callback at BASE+0x80; `extra`
    # places (address, bytes) elsewhere. A function at BASE+0x200 closes the
    # second gap.
    table = BASE + 0x300
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39feffd0c3"))
    raw = bytearray(b"\xcc" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80:0x80 + len(callback_code)] = callback_code
    for address, code in extra:
        raw[address - BASE:address - BASE + len(code)] = code
    raw[0x100] = raw[0x200] = 0xC3
    raw[0x300:0x304] = (BASE + 0x80).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        BASE + 0x100: function(BASE + 0x100, BASE + 0x101),
        BASE + 0x200: function(BASE + 0x200, BASE + 0x201),
    })
    return subject


def test_constants_come_only_from_reachable_callback_code():
    # A push decoded after the callback's ret cannot run, so the code it
    # names in the next gap is not a callback.
    stray = BASE + 0x180
    subject = _table_callback_subject(
        b"\xc3\x68" + stray.to_bytes(4, "little"), [(stray, b"\xc3")])
    subject.discover_static_indirect_targets()
    assert BASE + 0x80 in subject.func_db
    assert stray not in subject.func_db


def test_callback_claims_only_the_code_it_reaches():
    # The table callback returns at once; a constant in the registration
    # names a separate function later in the same gap. Linear decoding of
    # the callback runs over it, but the callback does not reach it.
    later = BASE + 0xa0
    table = BASE + 0x300
    pattern = (b"\xbe" + table.to_bytes(4, "little")
               + b"\xbf" + (table + 4).to_bytes(4, "little")
               + bytes.fromhex("39fe")
               + b"\x68" + later.to_bytes(4, "little") + bytes.fromhex("ffd0c3"))
    raw = bytearray(b"\x90" * 0x400)
    raw[:len(pattern)] = pattern
    raw[0x80] = raw[0xa0] = raw[0x100] = 0xC3
    raw[0x300:0x304] = (BASE + 0x80).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(pattern)),
        BASE + 0x100: function(BASE + 0x100, BASE + 0x101),
    })
    subject.discover_static_indirect_targets()
    assert BASE + 0x80 in subject.func_db
    assert later in subject.func_db


@pytest.mark.parametrize("arms, base, expected", [
    ([BASE + 0x50, BASE + 0x51], BASE + 0x300, (BASE + 0x300, BASE + 0x308)),
    # slot 0 is not an arm: the base points one dword before the entries
    ([BASE + 0x50, BASE + 0x51], BASE + 0x2fc, (BASE + 0x300, BASE + 0x308)),
    # the reader scanned backward from the base
    ([BASE + 0x50, BASE + 0x51], BASE + 0x304, (BASE + 0x300, BASE + 0x308)),
])
def test_table_storage_finds_the_entries(arms, base, expected):
    raw = bytearray(b"\xcc" * 0x400)
    raw[0x2f8:0x2fc] = b"\x00" * 4
    raw[0x300:0x308] = b"".join(arm.to_bytes(4, "little") for arm in arms)
    subject = translator(bytes(raw), [])
    assert subject._table_storage(base, arms) == expected


def test_jump_table_case_can_recover_register_continuation():
    case = BASE + 7
    continuation = case + 7
    table = BASE + 0x100
    body = (bytes.fromhex("ff2485") + table.to_bytes(4, "little")
            + b"\xbb" + continuation.to_bytes(4, "little")
            + bytes.fromhex("ffe3c3"))
    subject = translator(body, [])
    subject.lifter._analyze_switch_table = lambda operands: (
        [case] if operands and operands[0].type == "mem" else [])

    _, blocks = subject.decode_function(BASE, BASE + len(body))

    switch_block = next(block for block in blocks if block.start == BASE)
    case_block = next(block for block in blocks if block.start == case)
    assert case in switch_block.successors
    assert continuation in case_block.successors
    assert continuation in subject.lifter.imm_code_refs
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code


def test_recovered_cfg_does_not_rediscover_omitted_jump_table():
    case = BASE + 7
    table = BASE + 0x100
    raw = bytearray(b"\xcc" * 0x400)
    # jmp dword ptr [eax + ecx*4 + table]
    raw[:7] = bytes.fromhex("ffa488") + table.to_bytes(4, "little")
    raw[7] = 0xC3
    raw[0x100:0x104] = case.to_bytes(4, "little")
    subject = FunctionTranslator(
        bytes(raw), {BASE: function(BASE, BASE + 8)})
    instructions = subject.disasm.disassemble_function(
        bytes(raw[:8]), BASE, BASE + 8)
    subject.coalesced_function_starts.add(BASE)
    subject._recovered_cfg[BASE] = {
        "end": BASE + 8,
        "instructions": instructions,
        "jump_tables": {},
    }

    code = subject.translate_function(BASE, subject.func_db[BASE])

    assert table in subject.lifter.jump_table_targets
    assert subject.lifter.jump_table_targets[table] == []
    assert "switch:" not in code
    assert f"goto loc_{case:08X};" not in code
    assert "indirect tail jmp" in code


def test_resync_recomputes_register_jump_edges():
    leader = BASE + 5
    continuation = BASE + 7
    raw = bytes.fromhex("ffe0909090ffe3c3")
    subject = translator(raw, [])

    original = subject.disasm.disassemble_function
    full = original(raw, BASE, BASE + len(raw))
    initial = [insn for insn in full if insn.address != leader]
    calls = []

    def recording_disasm(*args, **kwargs):
        resync = set(kwargs.get("resync", ()))
        calls.append(resync)
        return full if resync else initial

    def indirect_refs(decoded, start, end, proof_mode=False,
                      return_jump_edges=False, computed_jump_edges=None):
        if any(insn.address == leader for insn in decoded):
            refs = {continuation}
            edges = {leader: {continuation}}
        else:
            refs = set()
            edges = {BASE: {BASE + 2}}
        return (refs, edges) if return_jump_edges else refs

    subject.disasm.disassemble_function = recording_disasm
    subject._indirect_code_refs = indirect_refs
    subject._computed_jump_edges = lambda decoded, start, end, \
            jump_table_targets=None: {BASE: {leader}}

    _, blocks = subject.decode_function(BASE, BASE + len(raw))

    assert calls == [set(), {leader}]
    case_block = next(block for block in blocks if block.start == leader)
    assert continuation in case_block.successors
    assert continuation in subject.lifter.imm_code_refs


@pytest.mark.parametrize("table_index", [0, 1])
@pytest.mark.parametrize("extra_code", [b"", bytes.fromhex("31c0")])
def test_embedded_jump_table_is_data_coverage(table_index, extra_code):
    table = BASE + 9
    first_case = table + 8 + len(extra_code)
    body = (bytes.fromhex("31c0ff2485")
            + (table + 4 * table_index).to_bytes(4, "little")
            + first_case.to_bytes(4, "little")
            + (first_case + 2).to_bytes(4, "little")
            + extra_code + bytes.fromhex("40c34bc3"))
    subject = translator(body, [first_case])
    if extra_code:
        with pytest.raises(ValueError, match="CFG gap"):
            subject.coalesce_function(BASE, BASE + len(body), [first_case])
        return
    subject.coalesce_function(BASE, BASE + len(body), [first_case])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert "switch: 2 entries, 2 targets" in code
    assert f"loc_{first_case:08X}:" in code
    assert f"loc_{first_case + 2:08X}:" in code


def test_register_indirect_continuation_is_recovered():
    continuation = BASE + 7
    body = b"\xb8" + continuation.to_bytes(4, "little") + bytes.fromhex("ffe040c3")
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code
    assert f"loc_{continuation:08X}:" in code
    assert "/* [UNRESOLVED]" not in code


def test_register_indirect_continuation_requires_matching_jump_register():
    continuation = BASE + 7
    body = b"\xb8" + continuation.to_bytes(4, "little") + bytes.fromhex("ffe340c3")
    subject = translator(body, [continuation])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [continuation])


def test_register_indirect_continuation_follows_register_copy():
    continuation = BASE + 9
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("89c3ffe340c3"))
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code


def test_register_indirect_continuation_follows_absolute_lea():
    continuation = BASE + 8
    body = (bytes.fromhex("8d05") + continuation.to_bytes(4, "little")
            + bytes.fromhex("ffe040c3"))
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code


def test_default_translation_preserves_spilled_continuation_candidate():
    continuation = BASE + 15
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("89042431c08b1c24ffe340c3"))
    subject = translator(body, [])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code
    assert f"loc_{continuation:08X}:" in code

    strict = translator(body, [continuation])
    strict.coalesce_function(BASE, BASE + len(body), [continuation])
    strict_code = strict.translate_function(BASE, strict.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in strict_code


def test_call_invalidates_all_continuation_register_proof():
    interior = BASE + 12
    callee = BASE + 0x100
    rel = callee - (BASE + 10)
    body = (b"\xbb" + interior.to_bytes(4, "little")
            + b"\xe8" + rel.to_bytes(4, "little", signed=True)
            + bytes.fromhex("ffe3c3"))
    subject = translator(body, [interior])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [interior])


def test_register_indirect_continuation_does_not_cross_cfg_join():
    continuation = BASE + 9
    body = (bytes.fromhex("7405")
            + b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("ffe040c3"))
    subject = translator(body, [continuation])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [continuation])


def test_register_indirect_continuation_does_not_cross_implicit_clobber():
    continuation = BASE + 8
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("adffe040c3"))
    subject = translator(body, [continuation])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [continuation])


def test_multi_operand_imul_preserves_unrelated_continuation_register():
    continuation = BASE + 10
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("6bdb02ffe040c3"))
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code


def test_direct_cfg_edge_preserves_register_target_in_both_modes():
    continuation = BASE + 9
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("eb00ffe040c3"))
    subject = translator(body, [continuation])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    assert continuation in subject._indirect_code_refs(
        instructions, BASE, BASE + len(body))
    assert continuation in subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True)
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code


def test_default_translation_ignores_unreachable_register_clobber():
    continuation = BASE + 11
    body = (b"\xbb" + continuation.to_bytes(4, "little")
            + bytes.fromhex("eb0231dbffe340c3"))
    subject = translator(body, [])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code
    assert f"loc_{continuation:08X}:" in code


def test_register_jump_edge_preserves_flag_state():
    target = BASE + 10
    body = (bytes.fromhex("83f805")
            + b"\xbb" + target.to_bytes(4, "little")
            + bytes.fromhex("ffe37d05b805000000c3"))
    subject = translator(body, [])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{target:08X};" in code
    assert "if (_flags /* jge" not in code


def test_backward_computed_edge_preserves_flag_state():
    target = BASE + 2
    done = BASE + 9
    compare = BASE + 10
    body = (bytes.fromhex("eb08")
            + bytes.fromhex("7d05b801000000c3")
            + bytes.fromhex("83f805")
            + b"\xbb" + target.to_bytes(4, "little")
            + bytes.fromhex("ffe3"))
    subject = translator(body, [])
    code = subject.translate_function(BASE, subject.func_db[BASE])

    assert f"goto loc_{target:08X};" in code
    target_body = code.split(f"loc_{target:08X}:", 1)[1]
    target_body = target_body.split(f"loc_{done:08X}:", 1)[0]
    assert "CMP_GE(" in target_body
    assert "if (_flags /* jge" not in target_body


def test_resolved_register_edge_participates_in_join_proof():
    target = BASE + 18
    continuation = BASE + 20
    body = (bytes.fromhex("85c97407")
            + b"\xb8" + target.to_bytes(4, "little")
            + bytes.fromhex("ffe0")
            + b"\xbb" + continuation.to_bytes(4, "little")
            + bytes.fromhex("eb00ffe340c3"))
    subject = translator(body, [target, continuation])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    refs = subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True)
    assert target in refs
    assert continuation not in refs
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [target, continuation])


def test_register_jump_loop_clobber_converges_fail_closed():
    target = BASE + 7
    body = (b"\xb8" + target.to_bytes(4, "little")
            + bytes.fromhex("ffe031c0ebfa"))
    subject = translator(body, [])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    refs = subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True)
    assert target not in refs


@pytest.mark.parametrize("op", ["0fc101", "f00fc101", "0fb109", "f00fb109"])
def test_multi_output_operations_clobber_eax_continuation_proof(op):
    continuation = BASE + 10 + (1 if op.startswith("f0") else 0)
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex(op + "ffe040c3"))
    subject = translator(body, [continuation])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [continuation])


def test_xlatb_clobbers_eax_continuation_proof():
    continuation = BASE + 8
    body = (b"\xb8" + continuation.to_bytes(4, "little")
            + bytes.fromhex("d7ffe040c3"))
    subject = translator(body, [continuation])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [continuation])


def test_sse_movsd_does_not_clobber_string_index_registers():
    continuation = BASE + 11
    body = (b"\xbe" + continuation.to_bytes(4, "little")
            + bytes.fromhex("f20f10c1ffe640c3"))
    subject = translator(body, [continuation])
    subject.coalesce_function(BASE, BASE + len(body), [continuation])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{continuation:08X};" in code


@pytest.mark.parametrize("string_op", ["a5", "a7"])
def test_string_dword_ops_clobber_esi_continuation_proof(string_op):
    continuation = BASE + 8
    body = (b"\xbe" + continuation.to_bytes(4, "little")
            + bytes.fromhex(string_op + "ffe640c3"))
    subject = translator(body, [continuation])
    with pytest.raises(ValueError, match="not the requested end"):
        subject.coalesce_function(BASE, BASE + len(body), [continuation])


def test_jecxz_reads_but_does_not_clobber_ecx_continuation():
    continuation = BASE + 11
    body = (b"\xb9" + continuation.to_bytes(4, "little")
            + bytes.fromhex("e302ffe1c340c3"))
    subject = translator(body, [])
    instructions = subject.disasm.disassemble_function(
        body, BASE, BASE + len(body))
    assert continuation in subject._indirect_code_refs(
        instructions, BASE, BASE + len(body), proof_mode=True)


def test_targeted_debug_slide_at_end_reaches_next_function():
    slide = BASE + 4
    end = BASE + 5
    body = bytes.fromhex("7402cd2dccc3")
    subject = translator(body, [slide, end])
    subject.coalesce_function(BASE, end, [slide])
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert f"goto loc_{end:08X}; /* int 0x2d skips slide int3 */" in code
    assert f"loc_{end:08X}: ;" in code
    assert f"sub_{end:08X}(); return;" in code


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


def test_batch_protects_manual_function_start_before_coalescence(tmp_path):
    with pytest.raises(ValueError, match="protected by manual code"):
        batch_translator(
            tmp_path, translator(), END, SPLITS,
            protected_function_starts={SPLITS[0]},
            seh_prolog=0, seh_epilog=0)


@pytest.mark.parametrize("helper", ["seh_prolog", "seh_epilog"])
def test_batch_protects_explicit_seh_override_before_coalescence(
        tmp_path, helper):
    with pytest.raises(ValueError, match="protected by manual code"):
        batch_translator(
            tmp_path, translator(), END, SPLITS,
            **{helper: SPLITS[0]})


def test_manual_protection_inputs_do_not_depend_on_split(tmp_path, monkeypatch):
    manual = tmp_path / "manual.json"
    manual.write_text(json.dumps([hex(BASE)]), encoding="utf-8")
    scan_result = ({BASE + 1}, {BASE + 2}, {BASE + 3})
    monkeypatch.setattr(manual_scan, "scan", lambda _: scan_result)

    protected, result = recomp_main._load_manual_protection(manual, "manual-src")

    assert result == scan_result
    assert protected == {BASE, BASE + 1, BASE + 2, BASE + 3}


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


@pytest.mark.parametrize("helper, body", [
    ("SEH_PROLOG", "64a1000000008d6c2410c3"),
    ("SEH_EPILOG", "64890d00000000c951c3"),
    ("SETJMP_FN", "c7422030324356c3"),
    ("LONGJMP_FN", "3d30324356c3"),
])
def test_coalescence_helper_detection_ignores_callback_only_entries(
        tmp_path, helper, body):
    raw = bytearray(b"\xcc" * 0x400)
    raw[:15] = (b"\xbe" + (BASE + 0x300).to_bytes(4, "little")
                + b"\xbf" + (BASE + 0x304).to_bytes(4, "little")
                + bytes.fromhex("39feffd0c3"))
    marker = bytes.fromhex(body)
    raw[0x40:0x40 + len(marker)] = marker
    raw[0x80] = 0xc3
    raw[0x300:0x304] = (BASE + 0x40).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + 12),
        BASE + 12: function(BASE + 12, BASE + 15),
        BASE + 0x80: function(BASE + 0x80, BASE + 0x81),
    })
    batch = batch_translator(tmp_path, subject, BASE + 15, [BASE + 12])
    assert BASE + 0x40 in batch.translator.recovered_function_starts
    assert getattr(batch.translator.lifter, helper) is None


@pytest.mark.parametrize("callback_offset", [0x40, 12, 14])
def test_batch_discovers_callbacks_from_repaired_body(tmp_path, callback_offset):
    raw = bytearray(b"\xcc" * 0x400)
    # The range setup and indirect call initially belong to separate fragments.
    raw[:15] = (b"\xbe" + (BASE + 0x300).to_bytes(4, "little")
                + b"\xbf" + (BASE + 0x304).to_bytes(4, "little")
                + bytes.fromhex("39feffd0c3"))
    raw[0x40:0x46] = bytes.fromhex("b807000000c3")
    raw[0x80] = 0xc3
    raw[0x300:0x304] = (BASE + callback_offset).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    for start, end in ((0, 12), (12, 15), (0x80, 0x81)):
        subject.func_db[BASE + start] = function(BASE + start, BASE + end)
    if callback_offset < 15:
        before = copy.deepcopy(subject.func_db)
        with pytest.raises(ValueError, match="callback"):
            subject.coalesce_function(BASE, BASE + 15, [BASE + 12])
        assert subject.func_db == before
        return
    batch = batch_translator(tmp_path, subject, BASE + 15, [BASE + 12])
    assert BASE + 0x40 in batch.translator.recovered_function_starts
    assert "eax = 7;" in batch.translate_single(BASE + 0x40)
    callback = batch.func_db[BASE + 0x40]
    callback["called_by"] = [hex(BASE), BASE + 0xAB]
    for _ in range(2):
        batch.translator.discover_static_indirect_targets(coalescing=True)
        assert callback["called_by"] == ["0x00010000", "0x000100AB"]


@pytest.mark.parametrize("caller_first", [True, False])
@pytest.mark.parametrize("separate_files", [True, False])
def test_batch_preserves_callbacks_exposed_between_repairs(
        tmp_path, caller_first, separate_files):
    raw = bytearray(b"\xcc" * 0x400)
    raw[:15] = (b"\xbe" + (BASE + 0x300).to_bytes(4, "little")
                + b"\xbf" + (BASE + 0x304).to_bytes(4, "little")
                + bytes.fromhex("39feffd0c3"))
    # The second owner falls through into a separately callable tail.
    raw[0x40:0x48] = bytes.fromhex("31c0b807000000c3")
    raw[0x80] = 0xc3
    raw[0x300:0x304] = (BASE + 0x42).to_bytes(4, "little")
    entries = [function(BASE + start, BASE + end)
               for start, end in ((0, 12), (12, 15), (0x40, 0x42),
                                  (0x42, 0x48), (0x80, 0x81))]
    for entry in entries:
        entry["end"] = hex(entry["end"])
    repairs = [{"start": hex(BASE), "end": hex(BASE + 15),
                "coalesce_starts": [hex(BASE + 12)]},
               {"start": hex(BASE + 0x40), "end": hex(BASE + 0x48),
                "coalesce_starts": [hex(BASE + 0x42)]}]
    if not caller_first:
        repairs.reverse()
    image = tmp_path / "synthetic.xbe"
    functions = tmp_path / "functions.json"
    bounds = tmp_path / "bounds.json"
    image.write_bytes(raw)
    functions.write_text(json.dumps(entries), encoding="utf-8")
    paths = [bounds]
    if separate_files:
        paths.append(tmp_path / "more-bounds.json")
        for path, repair in zip(paths, repairs):
            path.write_text(json.dumps([repair]), encoding="utf-8")
    else:
        bounds.write_text(json.dumps(repairs), encoding="utf-8")
    with pytest.raises(ValueError, match="independent evidence|callback"):
        BatchTranslator(image, functions, coalesce_json_paths=paths)


@pytest.mark.parametrize("opcode", [0xe8, 0xe9])
def test_recovered_callback_recovers_gap_callee(opcode):
    callback, callee, following = BASE + 0x80, BASE + 0x40, BASE + 0x100
    registration = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(registration)] = registration
    # The trailing ret also permits the tail-jump thunk to be discovered by
    # the existing linear callback probe; its target must receive a body too.
    body = bytes([opcode]) + (callee - callback - 5).to_bytes(4, "little", signed=True) + b"\xc3"
    raw[0x80:0x80 + len(body)] = body
    raw[0x40] = raw[0x100] = 0xc3
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(registration)),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert callback in subject.func_db
    assert callee in subject.func_db
    assert subject.func_db[callee]["called_by"] == [callback]


@pytest.mark.parametrize("reachable", [False, True])
def test_callback_after_switch_table_is_not_owned_by_linear_extent(reachable):
    owner, callback, following = BASE + 0x40, BASE + 0x80, BASE + 0x100
    registration = b"\x68" + callback.to_bytes(4, "little") + bytes.fromhex("ffd0c3")
    raw = bytearray(b"\xcc" * 0x200)
    raw[:len(registration)] = registration
    body = bytes.fromhex("31c0ff2485") + (BASE + 0x60).to_bytes(4, "little")
    raw[0x40:0x40 + len(body)] = body
    raw[0x50] = raw[0x51] = raw[0x80] = raw[0x100] = 0xc3
    raw[0x60:0x64] = (callback if reachable else BASE + 0x50).to_bytes(4, "little")
    raw[0x64:0x68] = (BASE + 0x51).to_bytes(4, "little")
    subject = translator(bytes(raw), [])
    subject.func_db.clear()
    subject.func_db.update({
        BASE: function(BASE, BASE + len(registration)),
        owner: function(owner, callback + 1),
        following: function(following, following + 1),
    })
    subject.discover_static_indirect_targets()
    assert (callback in subject.func_db) is not reachable
    assert subject.func_db[owner]["end"] == (callback + 1 if reachable else callback)
    if not reachable:
        body = subject.translate_function(owner, subject.func_db[owner])
        assert f"loc_{callback:08X}" not in body
