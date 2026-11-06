"""movsx from a 16-bit register must sign-extend for every 16-bit register.

The register list in _lift_movsx once left out bp and sp, so `movsx ebx, bp`
lifted as a zero-extend: Bink's IDCT turned negative coefficients into large
positive dwords and the movie frames showed wrong blocks.
"""

import pytest

from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter


def _reg(name, size):
    op = Operand("reg")
    op.reg = name
    op.size = size
    return op


def _movsx(dst, src, src_size):
    insn = Instruction(0, 3, "movsx", f"{dst}, {src}", "0fbfdd")
    insn.operands = [_reg(dst, 4), _reg(src, src_size)]
    return Lifter().lift_instruction(insn)


@pytest.mark.parametrize("src", ["ax", "bx", "cx", "dx", "si", "di", "bp", "sp"])
def test_movsx_word_register_sign_extends(src):
    out = _movsx("ebx", src, 2)

    assert len(out) == 1
    assert f"SX16(LO16(e{src}))" in out[0]


@pytest.mark.parametrize("src", ["al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"])
def test_movsx_byte_register_sign_extends(src):
    out = _movsx("ebx", src, 1)

    assert len(out) == 1
    assert "SX8(" in out[0]
