"""The lifter reads a code section's zero fill past raw_size, as the loader maps it.

Spartan's D3DX section ends one byte short of the `ret 0x10` closing
D3DXMatrixLookAtLH; the missing byte is the loader's zero fill. The file
bytes after raw_size belong to whatever follows the section on disk, so the
lifter must read zeros there, for at most one instruction, exactly as the
disassembler does.
"""
import pytest

from . import config
from .translator import FunctionTranslator


BASE = 0x10000
# The coalescence test's clamp, ending `ret 4` instead of `ret`:
# test ecx,ecx; jz short; mov edi,12; cmp eax,edi; jmp join;
# short: mov edi,6; cmp eax,edi; join: jge done; mov edi,eax;
# done: mov eax,edi; ret 4
CLAMP = bytes.fromhex("85c97409bf0c00000039f8eb07bf0600000039f87d0289c789f8c20400")
SPLITS = [BASE + 13, BASE + 20, BASE + 24]
END = BASE + len(CLAMP)
# The ret's high immediate byte is the first byte past raw_size.
RAW_SIZE = len(CLAMP) - 1


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x100, 0, RAW_SIZE, True),
    ])


def function(start, end):
    return {"_addr": start, "start": hex(start), "end": end,
            "size": end - start, "section": ".text"}


def translator(splits=SPLITS):
    bounds = [BASE, *splits, END]
    db = {start: function(start, end)
          for start, end in zip(bounds, bounds[1:])}
    # The next thing on disk is not zeros, so a file slice would be wrong.
    xbe = CLAMP[:RAW_SIZE] + b"\xcc" * 0x100
    return FunctionTranslator(xbe, db)


def test_readable_end_is_raw_end_plus_one_instruction():
    section = config._SECTIONS[0]
    assert config.readable_end(section) == BASE + RAW_SIZE + 14
    data = config.Section(".data", BASE, 0x100, 0, RAW_SIZE, False)
    assert config.readable_end(data) == BASE + RAW_SIZE


def test_body_crossing_raw_end_reads_zeros():
    t = translator()
    assert t._read_func_bytes(BASE, END + 2) == CLAMP + bytes(2)


def test_body_ending_at_the_readable_end_is_read():
    t = translator()
    data = t._read_func_bytes(BASE, BASE + RAW_SIZE + 14)
    assert data == CLAMP[:RAW_SIZE] + bytes(14)


def test_data_section_reads_zeros_past_raw_end(monkeypatch):
    # *Xlate sections are FORCE_DATA but flagged executable: the
    # disassembler decodes the loader's zeros there, so the lifter must too.
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section("EnglishXlate", BASE, 0x100, 0, RAW_SIZE, False),
    ])
    t = translator()
    assert t._read_func_bytes(BASE, END + 2) == CLAMP[:RAW_SIZE] + bytes(3)


def test_body_past_the_readable_end_is_refused(capsys):
    t = translator()
    assert t._read_func_bytes(BASE, BASE + RAW_SIZE + 15) is None
    assert t._read_func_bytes(BASE, BASE + RAW_SIZE + 15) is None
    err = capsys.readouterr().err
    assert err.count("sub_00010000") == 1 and ".text" in err


def test_ret_in_the_fill_lifts_with_its_zero_byte():
    t = translator(splits=[])
    code = t.translate_function(BASE, t.func_db[BASE])
    assert "esp += 8; return; /* ret 4 */" in code


def test_coalesce_end_in_the_fill_is_accepted():
    split = translator()
    split.coalesce_function(BASE, END, SPLITS)
    assert list(split.func_db) == [BASE]
    repaired = split.translate_function(BASE, split.func_db[BASE])
    whole = translator(splits=[])
    assert repaired == whole.translate_function(BASE, whole.func_db[BASE])


def test_trailing_table_scan_stops_at_its_section(monkeypatch, capsys):
    # The last function of a section, cut at its indexed jmp; the next start
    # is in the next section, so the scan bound must not reach it.
    nxt = BASE + 0x100
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x20, 0, 0x10, True),
        config.Section("D3DX", nxt, 0x10, 0x100, 0x10, True),
    ])
    body = bytes.fromhex("ff2485") + (BASE + 8).to_bytes(4, "little")
    xbe = (body + b"\x00" * 9).ljust(0x100, b"\xcc") + b"\xc3" * 0x10
    db = {BASE: function(BASE, BASE + len(body)), nxt: function(nxt, nxt + 1)}
    db[nxt]["section"] = "D3DX"
    t = FunctionTranslator(xbe, db)
    capsys.readouterr()
    t._extend_over_trailing_tables()
    assert "readable end" not in capsys.readouterr().err
