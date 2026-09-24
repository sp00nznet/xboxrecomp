"""`test X, X` and `cmp X, 0` are the same comparison, and the lifter must say so.

Both compute X: ZF, SF and PF come out identical and both clear CF and OF.
What differed was only the snapshot's reconstruction -- a cmp answers `je`
with `_fa == _fb`, a test with `(_fa & _fb) == 0` -- and `_merge_flag_states`
insists on one operation across every predecessor of a join. So a block
reached by `cmp [x], 0` on one edge and `test eax, eax` on the other
inherited no flag state, and its jcc compiled as the `_flags` fallback, which
nothing ever assigns: the branch was never taken.

Shin Megami Tensei: Nine's video decoder is exactly that shape. In
sub_002C4D63 -- which returns the mask saying which coefficient groups carry
AC terms -- 0x002C4EDC is a `je` reached by `cmp [ebp-0x14], 0` falling
through and by `test eax, eax` jumping in from 0x002C4E3C. Dead, the mask
comes back wrong; the IDCT's DC-only shortcut then fires for every group and
writes a row of one repeated value, which is a picture smeared horizontally
with its vertical structure intact.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter, normalise_zero_test
from .translator import _merge_flag_states


def _reg(name, size=4):
    return Operand(type="reg", reg=name, mem_size=size)


def _emit(mnemonic, ops):
    insn = Instruction(0, 2, mnemonic, "", "", operands=ops)
    lifter = Lifter()
    lifter.needs_cf = True
    return " ".join(lifter.lift_instruction(insn))


class ZeroTestNormalisationTest(unittest.TestCase):
    def test_self_test_becomes_a_compare_against_zero(self):
        kind, ops = normalise_zero_test("test", [_reg("eax"), _reg("eax")])
        self.assertEqual(kind, "cmp")
        self.assertEqual(ops[1].type, "imm")
        self.assertEqual(ops[1].imm, 0)

    def test_a_test_of_two_different_registers_is_left_alone(self):
        """`test eax, ebx` is a bitwise and, not a comparison with zero."""
        kind, ops = normalise_zero_test("test", [_reg("eax"), _reg("ebx")])
        self.assertEqual(kind, "test")
        self.assertIs(ops[1].reg, "ebx")

    def test_the_emitted_snapshot_compares_against_zero(self):
        out = _emit("test", [_reg("eax"), _reg("eax")])
        self.assertIn("_fb = (uint32_t)(0)", out)
        self.assertIn("cmp eax, 0", out)

    def test_carry_is_still_cleared(self):
        """test clears CF, and nothing borrows from zero -- same answer."""
        self.assertIn("_cf = 0;", _emit("test", [_reg("eax"), _reg("eax")]))

    def test_the_two_forms_now_merge_at_a_join(self):
        """The regression: this pair returned None, which is the dead branch."""
        mem = Operand(type="mem", mem_base="ebp", mem_disp=-0x14, mem_size=4)
        from_cmp = ("cmp", [mem, Operand(type="imm", imm=0, mem_size=4)])
        from_test = normalise_zero_test("test", [_reg("eax"), _reg("eax")])
        merged = _merge_flag_states([from_cmp, from_test])
        self.assertIsNotNone(merged)
        self.assertEqual(merged[0], "cmp")

    def test_a_genuine_test_still_refuses_to_merge_with_a_compare(self):
        """`test eax, ebx` is a different operation and must not be folded in."""
        mem = Operand(type="mem", mem_base="ebp", mem_disp=-0x14, mem_size=4)
        from_cmp = ("cmp", [mem, Operand(type="imm", imm=0, mem_size=4)])
        from_test = ("test", [_reg("eax"), _reg("ebx")])
        self.assertIsNone(_merge_flag_states([from_cmp, from_test]))


if __name__ == "__main__":
    unittest.main()
