""""movsd" is two instructions, and the dispatcher must not pick by name.

The string MOVSD copies a dword from [esi] to es:[edi]. The SSE2 MOVSD moves
a scalar double in or out of an xmm register. They share a mnemonic and
nothing else.

lift_instruction's string branch runs ahead of its SSE branch and matched on
the mnemonic alone, so an SSE movsd was lifted as a string copy: it walked
esi and edi and touched neither operand the instruction named. The xmm
register and the memory operand were both ignored, and two unrelated pointers
were advanced instead.

The pair is told apart by the only thing that differs between them, an xmm
operand.
"""
from .disasm import Instruction, Operand
from .lifter import Lifter, _has_xmm_operand


def _reg(name):
    return Operand(type="reg", reg=name)


def _lift(mnemonic, op_str, operands):
    insn = Instruction(0, 3, mnemonic, op_str, "", operands=operands)
    return " ".join(Lifter().lift_instruction(insn))


def test_string_movsd_still_lifts_as_a_string_copy():
    out = _lift("movsd", "dword ptr es:[edi], dword ptr [esi]", [])
    assert "edi" in out and "esi" in out
    assert "xmm" not in out


def test_sse_movsd_no_longer_lifts_as_a_string_copy():
    ops = [_reg("xmm0"), Operand(type="mem", mem_base="eax", mem_size=8)]
    out = _lift("movsd", "xmm0, qword ptr [eax]", ops)
    # The string lifter knows nothing about xmm; if this came out of it, the
    # move went somewhere the instruction never mentioned.
    assert "edi" not in out and "esi" not in out
    assert "xmm0" in out


def test_the_other_string_operations_are_unaffected():
    for mnemonic, op_str in (("movsb", "byte ptr es:[edi], byte ptr [esi]"),
                             ("movsw", "word ptr es:[edi], word ptr [esi]"),
                             ("stosd", "dword ptr es:[edi], eax"),
                             ("lodsd", "eax, dword ptr [esi]")):
        out = _lift(mnemonic, op_str, [])
        assert "edi" in out or "esi" in out, (mnemonic, out)


def test_the_operand_test_is_what_decides():
    assert not _has_xmm_operand([])
    assert not _has_xmm_operand(None)
    assert not _has_xmm_operand([_reg("eax")])
    assert _has_xmm_operand([_reg("xmm7")])
    assert _has_xmm_operand([_reg("eax"), _reg("xmm3")])
