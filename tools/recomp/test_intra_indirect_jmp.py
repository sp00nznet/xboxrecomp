"""
Self-check for `jmp <reg>` whose targets are labels in the same function.

Run: py -3 tools/recomp/test_intra_indirect_jmp.py

Hand-written SIMD code uses the register as a return label: each block does
`mov ebx, <next block>; jmp <shared tail>`, and the shared tail ends `jmp ebx`.
Every target is inside the one function, so this is a computed goto, not a call.

Lifted as an indirect tail call it resolves to nothing, and the shared tail
never comes back -- so the function's epilogue never runs. Half-Life 2's Xbox
loader has this in the XMV YUV-to-RGB converter (sub_0003A089, four such
blocks): each call leaked 0x2C bytes of guest stack, and the converter wrote
past its destination surface until it walked out of the tiled aperture 144 MB
later. The observable symptom was an access violation in a completely different
place, which is why this is worth a test rather than a comment.

The two properties: the goto must be emitted when the targets are inside the
function, and it must NOT be when the register holds an address outside it --
that really is an indirect tail call and guessing a label for it would be worse
than leaving it unresolved.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _setup(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="intra-indirect-test")
    return image


def _translate(image, va, size):
    db = {va: {"start": f"0x{va:08X}", "end": va + size,
               "_addr": va, "size": size}}
    return FunctionTranslator(image, db).translate_function(va, db[va])


def _mov_ebx_imm(value):
    return b"\xBB" + value.to_bytes(4, "little")     # mov ebx, imm32


JMP_EBX = b"\xFF\xE3"                                 # jmp ebx
NOP = b"\x90"
RET = b"\xC3"


def test_intra_function_targets_become_gotos():
    #   BASE+0: mov ebx, BASE+12    (a label inside this function)
    #   BASE+5: jmp ebx
    #   BASE+7: nop nop nop nop nop
    #   BASE+12: ret
    target = BASE + 12
    image = _setup(_mov_ebx_imm(target) + JMP_EBX + NOP * 5 + RET)
    c = _translate(image, BASE, len(image))
    assert f"goto loc_{target:08X};" in c, c
    assert "intra-function indirect jmp" in c, c
    print("ok  intra_function_targets_become_gotos")


def test_outside_target_stays_an_indirect_tail_call():
    # The immediate points past the end of the function: a real tail call.
    # Guessing a label here would send control somewhere it never went.
    image = _setup(_mov_ebx_imm(BASE + 0x400) + JMP_EBX + RET)
    c = _translate(image, BASE, len(image))
    assert "goto loc_" not in c, c
    assert "RECOMP_ITAIL" in c, c
    print("ok  outside_target_stays_an_indirect_tail_call")


def test_no_indirect_jmp_means_no_label_splitting():
    # A function that loads its own address but never jumps through a register
    # (a callback registration, say) must not gain labels or a dispatch -- the
    # immediate is data, not a branch target.
    image = _setup(_mov_ebx_imm(BASE + 7) + NOP * 2 + RET)
    c = _translate(image, BASE, len(image))
    assert "intra-function indirect jmp" not in c, c
    print("ok  no_indirect_jmp_means_no_label_splitting")


if __name__ == "__main__":
    test_intra_function_targets_become_gotos()
    test_outside_target_stays_an_indirect_tail_call()
    test_no_indirect_jmp_means_no_label_splitting()
    print("intra_indirect_jmp: ALL PASS")
