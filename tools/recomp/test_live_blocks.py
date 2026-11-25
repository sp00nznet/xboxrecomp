"""
Self-check: bytes after an unconditional jump that nothing branches to are
not lifted.

Every branch is a block leader at its end, so a jump table or padding after a
`jmp` becomes a block. Lifted, it is dead C, but its own jumps still name
targets: in Burnout 3 a jns decoded from an XONLINE switch table and a jmp
decoded from the bytes after a tail jump each became an unresolved stub.

Run: py -3 -m pytest tools/recomp/test_live_blocks.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="live-blocks-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_garbage_after_a_jmp_is_not_lifted():
    #   +0  xor eax, eax
    #   +2  jmp +5            -> ret at +9
    #   +4  (data) 79 7F      decodes as jns far outside the function
    #   +6  (data) 90 90 90
    #   +9  ret
    image = (b"\x31\xC0"          # xor eax, eax
             b"\xEB\x05"          # jmp +5 -> +9
             b"\x79\x7F"          # garbage: jns +0x7F
             b"\x90\x90\x90"      # garbage
             b"\xC3")             # ret
    code = _translate(image)
    assert "jns" not in code, code
    assert "sub_0001008" not in code, code
    assert "loc_00010009" in code, code


def test_a_branch_target_after_a_jmp_is_still_lifted():
    #   +0  test eax, eax
    #   +2  jz +2             -> +6
    #   +4  jmp +1            -> +7
    #   +6  inc eax           (reached by the jz)
    #   +7  ret
    image = (b"\x85\xC0"          # test eax, eax
             b"\x74\x02"          # jz +2 -> +6
             b"\xEB\x01"          # jmp +1 -> +7
             b"\x40"              # inc eax
             b"\xC3")             # ret
    code = _translate(image)
    assert "loc_00010006" in code, code
    assert "eax + 1" in code or "eax++" in code or "inc" in code, code


if __name__ == "__main__":
    test_garbage_after_a_jmp_is_not_lifted()
    test_a_branch_target_after_a_jmp_is_still_lifted()
    print("ok")
