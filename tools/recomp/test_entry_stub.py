"""
Self-check for unresolved call targets that land inside a known function.

Run: py -3 tools/recomp/test_entry_stub.py

The function detector sometimes cuts one function in two, so a jump between
the halves is lifted as a call to an address that starts no function. Such a
target used to get a stub that only pops the return address: the code at the
target never ran. SSX 3's Options screen jumps into the shared `pop esi;
ret 8` epilogue of the next function that way, and the skipped pop left esi
holding the screen object -- the menu code then called through a NULL
pointer. The Equip Gear screen lost its setup to another such stub.

entry_stub() emits the containing function's translation again, renamed and
entered with a goto at the target. _stub_epilogue() is the fallback for an
address that is a bare epilogue: it names the registers to restore.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
FUNC = BASE                 # mov eax, 1 / inc eax / pop esi / pop edi / ret 8
MID = BASE + 5              # inc eax: a target inside FUNC
EPI = BASE + 6              # pop esi: FUNC's epilogue
LONE = BASE + 0x20          # pop ebx / ret: code in no function
JUNK = BASE + 0x30          # mov eax, 2 / ret: not an epilogue


def _translator():
    image = bytearray(b"\xCC" * 0x100)

    def put(va, data):
        image[va - BASE:va - BASE + len(data)] = data

    put(FUNC, bytes.fromhex("b801000000" "40" "5e" "5f" "c20800"))
    put(LONE, bytes.fromhex("5b" "c3"))
    put(JUNK, bytes.fromhex("b802000000" "c3"))
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="entry-stub-test")
    end = FUNC + 12
    func_db = {FUNC: {"start": f"0x{FUNC:08X}", "end": end, "_addr": FUNC,
                      "size": end - FUNC}}
    return FunctionTranslator(bytes(image), func_db)


def test_target_inside_a_function_enters_it():
    tr = _translator()
    code = tr.entry_stub(MID, f"sub_{MID:08X}")
    assert code, "no entry stub for a target inside a known function"
    assert f"void sub_{MID:08X}(void)" in code, code
    assert f"goto loc_{MID:08X};" in code, code
    assert f"loc_{MID:08X}:" in code, code
    # The containing function keeps its own name elsewhere: the stub is a copy.
    assert f"void sub_{FUNC:08X}(void)" not in code, code
    print("ok  target_inside_a_function_enters_it")


def test_target_outside_every_function_has_no_entry_stub():
    tr = _translator()
    assert tr.entry_stub(LONE, f"sub_{LONE:08X}") is None
    assert tr.entry_stub(FUNC, f"sub_{FUNC:08X}") is None   # a start, not inside
    print("ok  target_outside_every_function_has_no_entry_stub")


def test_epilogue_names_the_pops_and_the_ret():
    tr = _translator()
    assert tr._stub_epilogue(EPI) == (["esi", "edi"], 8)
    assert tr._stub_epilogue(LONE) == (["ebx"], 0)
    assert tr._stub_epilogue(JUNK) is None
    print("ok  epilogue_names_the_pops_and_the_ret")


if __name__ == "__main__":
    test_target_inside_a_function_enters_it()
    test_target_outside_every_function_has_no_entry_stub()
    test_epilogue_names_the_pops_and_the_ret()
