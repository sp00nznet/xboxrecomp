"""--trace-stores brackets each guest store that can leave the stack with
RECOMP_STORE (templates/runtime/recomp_store_trace.h): the address is taken
before the instruction, the row logged after it. Reads, stack slots and x87
loads get nothing, and without the option the output is what it always was.
The reader (store_trace.py) parses what src/kernel/recomp_store_trace.c
writes."""

import struct

from tools.recomp import config, store_trace
from tools.recomp.translator import BatchTranslator, FunctionTranslator

BASE = 0x10000

CODE = bytes.fromhex(
    "894310"                    # +0  mov [ebx+0x10], eax
    "c7054477ae0005000000"      # +3  mov dword [0xAE7744], 5
    "50"                        # +13 push eax
    "89442404"                  # +14 mov [esp+4], eax
    "3903"                      # +18 cmp [ebx], eax
    "d903"                      # +20 fld dword [ebx]
    "d91b"                      # +22 fstp dword [ebx]
    "f3ab"                      # +24 rep stosd
    "83f801"                    # +26 cmp eax, 1
    "0f9403"                    # +29 sete byte [ebx]
    "8703"                      # +32 xchg [ebx], eax
    "f723"                      # +34 mul dword [ebx]
    "58"                        # +36 pop eax
    "c3"                        # +37 ret
)


def _translate(trace=True, funcs=None):
    config._install(
        [config.Section(".text", BASE, len(CODE), 0, len(CODE), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="trace-stores-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(CODE),
                 "_addr": BASE, "size": len(CODE)}}
    t = FunctionTranslator(CODE, db)
    if trace:
        t.trace_stores = "0xAE7744+12"
        t.trace_stores_functions = funcs
    return t.translate_function(BASE, db[BASE])


def _hooks(code):
    return [line.strip() for line in code.splitlines()
            if "RECOMP_STORE" in line or "_st_a =" in line or "_st_n =" in line]


def test_each_store_is_bracketed():
    code = _translate()
    assert "uint32_t _st_a = 0, _st_n = 0;" in code, code
    f = f"0x{BASE:08X}u"
    want_posts = [
        f"RECOMP_STORE(0x{BASE:08X}u, {f}, 4u);",
        f"RECOMP_STORE(0x{BASE + 3:08X}u, {f}, 4u);",
        f"RECOMP_STORE(0x{BASE + 22:08X}u, {f}, 4u);",     # fstp
        f"RECOMP_STORE_N(0x{BASE + 24:08X}u, {f});",        # rep stosd
        f"RECOMP_STORE(0x{BASE + 29:08X}u, {f}, 1u);",     # sete
        f"RECOMP_STORE(0x{BASE + 32:08X}u, {f}, 4u);",     # xchg
    ]
    posts = [h for h in _hooks(code) if h.startswith("RECOMP_STORE")]
    assert posts == want_posts, "\n".join(_hooks(code))


def test_the_address_is_taken_before_the_store():
    lines = [line.strip() for line in _translate().splitlines()]
    pre = lines.index("_st_a = (uint32_t)(ebx + 0x10);")
    store = next(i for i, line in enumerate(lines) if line.startswith("MEM32(ebx + 0x10) ="))
    post = next(i for i, line in enumerate(lines)
                if line.startswith(f"RECOMP_STORE(0x{BASE:08X}u"))
    assert pre < store < post
    # rep stos: the range, walked down from edi when DF is set
    assert "_st_n = ecx * 4u; _st_a = g_df ? edi + 4u - _st_n : edi;" in lines


def test_off_is_the_stock_output():
    plain = _translate(trace=False)
    assert "_st_a" not in plain and "RECOMP_STORE" not in plain
    # The function filter leaves an unlisted function stock as well.
    assert _translate(funcs={BASE + 0x100}) == plain


def test_header_written_only_when_on(tmp_path, monkeypatch):
    bt = BatchTranslator.__new__(BatchTranslator)
    bt.trace_stores = "0xAE7744+12,0x4B271C+4"
    bt._write_store_trace_header(str(tmp_path))
    text = (tmp_path / "recomp_store_trace.h").read_text()
    assert '#define RECOMP_STORE_TRACE_DEFAULTS "0xAE7744+12,0x4B271C+4"' in text
    assert "#define RECOMP_STORE(va, func, size)" in text
    bt.trace_stores = None
    bt._write_store_trace_header(str(tmp_path))
    assert not (tmp_path / "recomp_store_trace.h").exists()


def _write_trace(path, rows):
    with open(path, "wb") as f:
        f.write(b"XBRSTRC1" + struct.pack("<II", 32, 1))
        for r in rows:
            f.write(store_trace.ROW.pack(*r))


def test_reader_round_trip(tmp_path, capsys):
    p = tmp_path / "t.bin"
    # va addr func frame seq size kind thread value
    _write_trace(p, [
        (0x176183, 0xAE7748, 0x176150, 10, 2, 4, 0, 0, 0x3F800000),
        (0x176180, 0xAE7744, 0x176150, 10, 1, 4, 0, 0, 0x40000000),
        (0x062FC0, 0xEC4C14, 0x062FB0, 11, 3, 4, 0, 1, 7),
    ])
    rows = store_trace.read(p)
    assert [r.seq for r in rows] == [1, 2, 3]
    assert store_trace.ROW.size == 32
    sel = [r for r in rows if store_trace.overlaps(
        r, store_trace.parse_ranges("0xAE7744+8"))]
    assert len(sel) == 2
    ws = store_trace.writers(sel)
    assert set(ws) == {(0x176150, 0x176183), (0x176150, 0x176180)}
    assert store_trace.main([str(p), "--addr", "0xAE7744-0xAE774C"]) == 0
    out = capsys.readouterr().out
    assert "2 rows, 2 writers in 1 functions" in out
    assert "sub_00176150" in out and "sub_00062FB0" not in out


def test_function_filter_keeps_an_empty_list_empty():
    # An empty list instruments nothing; only None means every function.
    assert _translate(funcs=set()) == _translate(trace=False)
    assert "RECOMP_STORE" in _translate(funcs=None)


def _batch(tmp_path, trace):
    from types import SimpleNamespace
    bt = BatchTranslator.__new__(BatchTranslator)
    bt.output_dir = str(tmp_path)
    bt.trace_stores = trace
    bt.translator = SimpleNamespace(owned_function_starts=set())
    return bt


def test_batch_without_output_file_writes_the_header(tmp_path, monkeypatch):
    # No output_file (the unsplit lift): the C lands in output_dir and the
    # header beside it, not a crash on os.path.abspath(None).
    monkeypatch.chdir(tmp_path)
    stats = _batch(tmp_path, "0xAE7744+12").translate_batch([])
    assert stats["output_file"] == str(tmp_path / "recompiled.c")
    assert (tmp_path / "recomp_store_trace.h").exists()
    assert '#include "recomp_store_trace.h"' in (tmp_path / "recompiled.c").read_text()
    # A stock lift over that gen/ removes the header again.
    _batch(tmp_path, None).translate_batch([])
    assert not (tmp_path / "recomp_store_trace.h").exists()
    assert "recomp_store_trace" not in (tmp_path / "recompiled.c").read_text()
