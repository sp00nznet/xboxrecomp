"""movsx r32, r16 sign-extends every 16-bit register source.

`bp` and `sp` were missing from the register list, so `movsx eax, bp` was
lifted as a plain zero-extending read of LO16(ebp). A pad poll that
normalises the right stick's Y through exactly that instruction would then
misread it, so a stick pushed down read as a large positive value.
"""

import pytest

from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter


def _insn(mnemonic, reg):
    ops = [Operand(type="reg", reg="eax"), Operand(type="reg", reg=reg)]
    return Instruction(0x1000, 3, mnemonic, f"eax, {reg}", "", operands=ops)


@pytest.mark.parametrize("reg, modrm, full", [
    ("bp", "c5", "ebp"),
    ("sp", "c4", "esp"),
    ("si", "c6", "esi"),
    ("di", "c7", "edi"),
    ("ax", "c0", "eax"),
])
def test_movsx_r32_r16_sign_extends(reg, modrm, full):
    insn = _insn("movsx", reg)

    assert Lifter().lift_instruction(insn) == [f"eax = SX16(LO16({full}));"]


@pytest.mark.parametrize("reg, modrm, full", [
    ("bp", "c5", "ebp"),
    ("sp", "c4", "esp"),
])
def test_movzx_r32_r16_still_zero_extends(reg, modrm, full):
    insn = _insn("movzx", reg)

    assert Lifter().lift_instruction(insn) == [f"eax = ZX16(LO16({full}));"]
