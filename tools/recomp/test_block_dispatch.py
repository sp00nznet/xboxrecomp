import pytest

from tools.recomp import config
from tools.recomp.block_dispatch import (BlockRecord, _dedupe_records,
                                         _normalize_functions, build_manifest,
                                         collect_blocks, dispatch_stats)
from tools.recomp.translator import FunctionTranslator


def test_normalize_functions_accepts_list_and_hex_strings():
    raw = [
        {"start": "0x1000", "end": "0x1010"},
        {"start": 0x2000, "end": 0x2020},
        {"start": "0x3000", "end": "0x3000"},
    ]
    functions = _normalize_functions(raw)
    assert [(start, info["end"]) for start, info in functions.items()] == [
        (0x1000, 0x1010), (0x2000, 0x2020)]


def test_normalize_functions_derives_end_from_address_and_size():
    raw = [
        {"address": "0x4000", "size": "0x20"},
        {"_addr": 0x5000, "size": 0x10},
    ]
    functions = _normalize_functions(raw)
    assert [(start, info["end"]) for start, info in functions.items()] == [
        (0x4000, 0x4020), (0x5000, 0x5010)]


def test_normalize_functions_preserves_keyed_addresses_and_entry_evidence():
    raw = {
        "0x1000": {"end": "0x1010", "section": ".text", "has_prologue": True},
        "0x2000": {"size": "0x20", "called_by": ["0x1000"]},
        "named": {"address": "0x3000", "size": 0x10},
    }
    functions = _normalize_functions(raw)
    assert functions[0x1000]["end"] == 0x1010
    assert functions[0x1000]["has_prologue"]
    assert functions[0x1000]["section"] == ".text"
    assert functions[0x2000]["end"] == 0x2020
    assert functions[0x2000]["called_by"] == ["0x1000"]
    assert functions[0x3000]["end"] == 0x3010


def test_normalize_functions_skips_non_address_keys():
    assert _normalize_functions({"metadata": {"size": 4}}) == {}


def test_inventory_recovers_missing_static_callback(monkeypatch):
    raw = bytearray(b"\xcc" * 0x31)
    # mov esi, 0x1040; mov edi, 0x1044; cmp esi, edi; call eax; ret
    raw[:15] = bytes.fromhex("be40100000bf4410000039feffd0c3")
    raw[0x20:0x26] = bytes.fromhex("b801000000c3")
    raw[0x30] = 0xc3
    raw.extend(b"\0" * (0x44 - len(raw)))
    raw[0x40:0x44] = bytes.fromhex("20100000")
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", 0x1000, len(raw), 0, len(raw), True),
    ])
    functions = _normalize_functions([
        {"start": 0x1000, "end": 0x100f, "section": ".text"},
        {"start": 0x1030, "end": 0x1031, "section": ".text"},
    ])
    records = collect_blocks(bytes(raw), functions)
    assert any(record.start == record.owner == 0x1020 for record in records)


def test_inventory_recovers_split_switch_ownership(monkeypatch):
    raw = bytearray(b"\xcc" * 0x31)
    raw[:12] = bytes.fromhex("83f8017723ff248510100000")
    raw[0x10:0x18] = bytes.fromhex("2010000028100000")
    raw[0x20:0x26] = bytes.fromhex("b801000000c3")
    raw[0x28:0x2e] = bytes.fromhex("b802000000c3")
    raw[0x30] = 0xc3
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", 0x1000, len(raw), 0, len(raw), True),
    ])
    functions = _normalize_functions([
        {"start": 0x1000, "end": 0x100c, "has_prologue": True},
        {"start": 0x1020, "end": 0x1026},
        {"start": 0x1028, "end": 0x102e},
        {"start": 0x1030, "end": 0x1031, "called_by": ["0x1000"]},
    ])
    records = collect_blocks(bytes(raw), functions)
    owners = {record.start: record.owner for record in records}
    assert owners[0x1020] == owners[0x1028] == 0x1000
    assert owners[0x1030] == 0x1030
    assert {record.owner for record in records} == {0x1000, 0x1030}
    assert not any(0x100c <= record.start < 0x1020 for record in records)


def test_dedupe_prefers_later_more_specific_overlapping_owner():
    outer = BlockRecord(0x1100, 0x1110, 0x1000, (), 2)
    inner = BlockRecord(0x1100, 0x1108, 0x1080, (), 1)
    records = _dedupe_records([outer, inner])
    assert records == [inner]


def test_dispatch_stats_uses_byte_indexed_guest_va_slots():
    records = [
        BlockRecord(0x1000, 0x1005, 0x1000, (0x1005,), 2),
        BlockRecord(0x1005, 0x1010, 0x1000, (), 3),
    ]
    stats = dispatch_stats(records)
    assert stats["blocks"] == 2
    assert stats["code_base"] == 0x1000
    assert stats["code_end"] == 0x1010
    assert stats["span_bytes"] == 0x10
    assert stats["dense_slots"] == 0x10
    assert stats["dense_table_bytes"] == 0x80
    assert stats["instructions"] == 5
    assert stats["avg_instructions_per_block"] == 2.5


def test_manifest_records_dense_slots_and_json_safe_successors():
    records = [
        BlockRecord(0x4010, 0x4014, 0x4000, (0x4020, 0x4030), 1),
        BlockRecord(0x4020, 0x4028, 0x4000, (), 2),
    ]
    manifest = build_manifest(records)
    assert manifest["format"] == 1
    assert manifest["blocks"][0]["dense_slot"] == 0
    assert manifest["blocks"][0]["successors"] == [0x4020, 0x4030]
    assert manifest["blocks"][1]["dense_slot"] == 0x10


def test_empty_stats_are_well_defined():
    stats = dispatch_stats([])
    assert stats["blocks"] == 0
    assert stats["dense_table_bytes"] == 0
    assert stats["avg_instructions_per_block"] == 0.0


@pytest.mark.parametrize("raw, targets", [
    # mov edi, 0x100a; jmp edi; ret; nop; nop; mov eax, 1; ret
    (bytes.fromhex("bf0a100000ffe7c39090b801000000c3"), {0x100a}),
    # jmp [eax*4+0x1010]; ret; a misaligned decode before two switch arms.
    (bytes.fromhex("ff248510100000c3b8b801000000c3c3") +
     bytes.fromhex("091000000f100000"), {0x1009, 0x100f}),
])
def test_inventory_includes_production_indirect_targets(monkeypatch, raw, targets):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", 0x1000, len(raw), 0, len(raw), True),
    ])
    records = collect_blocks(raw, {0x1000: {"end": 0x1010}})
    assert targets <= {record.start for record in records}
    stats = dispatch_stats(records)
    assert stats["code_end"] == 0x1010
    assert stats["dense_table_bytes"] == 128
    translator = FunctionTranslator(raw, {0x1000: {"end": 0x1010}})
    code = translator.translate_function(0x1000, {"end": 0x1010})
    for target in targets:
        assert f"loc_{target:08X}: ;" in code
        assert f"goto loc_{target:08X};" in code
