"""movsx r32, r16 sign-extends every 16-bit register source.

`bp` and `sp` were missing from the register list, so `movsx eax, bp` was
lifted as a plain zero-extending read of LO16(ebp). A title that normalises
a signed 16-bit value (an analog stick axis, say) through that instruction
then reads a negative value as a large positive one.
"""

import pytest

from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter


def _insn(mnemonic, reg):
    ops = [Operand(type="reg", reg="eax"), Operand(type="reg", reg=reg)]
    return Instruction(0x1000, 3, mnemonic, f"eax, {reg}", "", operands=ops)


@pytest.mark.parametrize("reg, full", [
    ("bp", "ebp"),
    ("sp", "esp"),
    ("si", "esi"),
    ("di", "edi"),
    ("ax", "eax"),
])
def test_movsx_r32_r16_sign_extends(reg, full):
    insn = _insn("movsx", reg)

    assert Lifter().lift_instruction(insn) == [f"eax = SX16(LO16({full}));"]


@pytest.mark.parametrize("reg, full", [
    ("bp", "ebp"),
    ("sp", "esp"),
])
def test_movzx_r32_r16_still_zero_extends(reg, full):
    insn = _insn("movzx", reg)

    assert Lifter().lift_instruction(insn) == [f"eax = ZX16(LO16({full}));"]
