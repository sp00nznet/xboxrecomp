"""
Self-check for the LOOP family.

LOOP decrements ECX and branches while it is non-zero; LOOPE/LOOPNE also
test ZF. None of them read or write EFLAGS otherwise.

They were reaching the generic jcc path, which knows only about flags. That
path dropped the ECX decrement entirely and emitted the `_flags` fallback --
a variable nothing ever assigns -- so the back edge compiled as never taken.
A counted loop ran its body exactly once with its counter untouched, and the
comparison sitting above the loop was discarded as well, which sent the next
jcc down the same dead path.
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
        entry_point=BASE, kernel_thunk_addr=BASE, origin="loop-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


# +0  cmp eax, 5        ; sets the flags the jge below wants
# +3  <loop insn> +4    ; -> the ret at +9
# +5  jge +2            ; -> the ret at +9
# +7  nop; nop
# +9  ret
def _fixture(loop_opcode):
    return (b"\x83\xF8\x05" + loop_opcode + b"\x04"
            b"\x7D\x02" b"\x90\x90" b"\xC3")


def test_loop_counts_ecx_down_and_branches_on_it():
    code = _translate(_fixture(b"\xE2"))
    assert "ecx -= 1;" in code, code
    assert "if (ecx != 0) goto loc_00010009;" in code, code
    assert "_flags /* loop" not in code, code


def test_loope_and_loopne_combine_the_counter_with_the_zero_flag():
    loope = _translate(_fixture(b"\xE1"))
    assert "if ((ecx != 0) && (CMP_EQ(_fa, _fb)))" in loope, loope

    loopne = _translate(_fixture(b"\xE0"))
    assert "if ((ecx != 0) && (CMP_NE(_fa, _fb)))" in loopne, loopne


def test_loop_preserves_the_comparison_for_a_later_branch():
    # The jge at +5 inherits its flags from the cmp at +0 across the loop.
    for opcode in (b"\xE2", b"\xE1", b"\xE0"):
        code = _translate(_fixture(opcode))
        assert "if (CMP_GE(_fas, _fbs)) goto loc_00010009;" in code, code


def test_loope_without_a_tracked_setter_keeps_the_fallback():
    # nop; loope +4; nops; ret -- nothing owns ZF, so the loop must not
    # invent a termination condition, but ECX still counts down.
    code = _translate(b"\x90\xE1\x04\x90\x90\x90\x90\xC3")
    assert "ecx -= 1;" in code, code
    assert "(ecx != 0) && (_flags)" in code, code


if __name__ == "__main__":
    test_loop_counts_ecx_down_and_branches_on_it()
    test_loope_and_loopne_combine_the_counter_with_the_zero_flag()
    test_loop_preserves_the_comparison_for_a_later_branch()
    test_loope_without_a_tracked_setter_keeps_the_fallback()
    print("ok")