"""
Self-check for a function cut at inline jump tables that precede their arms.

Run: py -3 tools/recomp/test_trailing_jump_tables.py

MSVC's hand-written CRT memcpy puts each dword table before the arms it
indexes, and the arms branch back into the body. The function list ends the
function where decoding meets the first table, so the arms sit in an unowned
gap and the indexed jumps leave as indirect dispatches nothing can resolve. In
Dead or Alive 3 the first memcpy stopped on one.

The fix recovers the CFG through the gap and lifts the arms as in-function
gotos. A tail_jump_alias start inside an arm does not end the gap.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
LOOP = BASE + 0x07   # body code the arms branch back to
TABLE = BASE + 0x10  # jmp [eax*4+TABLE]; the function list ends here
ARM0 = BASE + 0x18   # ret
ARM1 = BASE + 0x19   # dec eax; jmp LOOP
ALIAS = BASE + 0x19  # an alias entry inside the gap
NEXT = BASE + 0x20


def _entry(start, end, method="prologue"):
    return {"start": f"0x{start:08X}", "end": end, "_addr": start,
            "size": end - start, "detection_method": method}


def _translator(arm1_code):
    image = bytearray(b"\xCC" * 0x40)

    def put(va, data):
        image[va - BASE:va - BASE + len(data)] = data

    put(BASE, b"\x83\xE0\x01")                                  # and eax, 1
    put(BASE + 3, b"\x90" * 4)
    put(LOOP, b"\xFF\x24\x85" + TABLE.to_bytes(4, "little"))     # jmp [eax*4+TABLE]
    put(TABLE, ARM0.to_bytes(4, "little") + ARM1.to_bytes(4, "little"))
    put(ARM0, b"\xC3")
    put(ARM1, arm1_code)
    put(NEXT, b"\xC3")
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="trailing-table-test")
    return FunctionTranslator(bytes(image), {
        BASE: _entry(BASE, TABLE),
        ALIAS: _entry(ALIAS, NEXT, "tail_jump_alias"),
        NEXT: _entry(NEXT, NEXT + 1),
    })


def test_arms_after_their_table_become_in_function_gotos():
    translator = _translator(b"\x48\xEB\xEB")    # dec eax; jmp LOOP
    assert translator.discover_jump_table_entries() == set()
    assert translator.func_db[BASE]["end"] == ARM1 + 3
    code = translator.translate_function(BASE, translator.func_db[BASE])
    assert f"loc_{ARM0:08X}" in code and f"loc_{ARM1:08X}" in code, code
    assert f"goto loc_{LOOP:08X};" in code, code
    print("ok  arms_after_their_table_become_in_function_gotos")


def test_arm_leaving_the_gap_keeps_the_cut():
    translator = _translator(b"\xEB\x20")         # jmp past NEXT, no body
    translator.discover_jump_table_entries()
    assert translator.func_db[BASE]["end"] == TABLE
    print("ok  arm_leaving_the_gap_keeps_the_cut")


def test_callback_discovery_leaves_the_arms_to_their_function():
    # Discovery runs first. An immediate naming an arm after a ret must not
    # become a start that cuts the gap before the arms are recovered.
    translator = _translator(b"\x48\xEB\xEB")    # dec eax; jmp LOOP
    del translator.func_db[ALIAS]
    registrar = BASE + 0x30                     # push ARM1; call eax; ret
    image = bytearray(translator.xbe_data)
    image[0x30:0x38] = b"\x68" + ARM1.to_bytes(4, "little") + b"\xFF\xD0\xC3"
    translator.xbe_data = bytes(image)
    translator.func_db[registrar] = _entry(registrar, registrar + 8)
    translator.discover_static_indirect_targets()
    assert ARM1 not in translator.func_db
    translator.discover_jump_table_entries()
    assert translator.func_db[BASE]["end"] == ARM1 + 3
    print("ok  callback_discovery_leaves_the_arms_to_their_function")


if __name__ == "__main__":
    test_arms_after_their_table_become_in_function_gotos()
    test_arm_leaving_the_gap_keeps_the_cut()
    test_callback_discovery_leaves_the_arms_to_their_function()
    print("trailing_jump_tables: ALL PASS")
