"""SSE and x87 forms that used to lift to a comment and vanish at runtime.

Each of these was a `/* ... */` comment, which the C compiler reads as nothing.
The integer bitwise ops on xmm and movq on xmm now lift to the same helpers the
float forms use. Everything else in these two lifters that is not translated
goes through `_unimplemented`, so it is counted and marked at runtime.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str="", operands=()):
    lifter = Lifter()
    insn = Instruction(0x00012340, 3, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _xmm(name):
    return Operand(type="reg", reg=name)


def _mem(size):
    return Operand(type="mem", mem_base="eax", mem_index=None, mem_scale=1,
                   mem_disp=0, mem_size=size)


class XmmIntegerBitwiseTest(unittest.TestCase):

    def test_pxor_on_xmm_is_the_xor_helper(self):
        lifter, out = _lift("pxor", "xmm0, xmm1", [_xmm("xmm0"), _xmm("xmm1")])
        self.assertIn("XMM_XOR(", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_pand_pandn_por_map_to_their_helpers(self):
        for mnem, helper in (("pand", "XMM_AND("), ("pandn", "XMM_ANDN("),
                             ("por", "XMM_OR(")):
            with self.subTest(mnemonic=mnem):
                lifter, out = _lift(mnem, "xmm2, xmm3",
                                    [_xmm("xmm2"), _xmm("xmm3")])
                self.assertIn(helper, out)
                self.assertEqual(lifter.unimplemented, {})

    def test_pcmpgtd_on_xmm_is_the_signed_compare_helper(self):
        lifter, out = _lift("pcmpgtd", "xmm0, xmm1", [_xmm("xmm0"), _xmm("xmm1")])
        self.assertIn("XMM_PCMPGTD(", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})


class MovqXmmTest(unittest.TestCase):

    def test_load_low_qword_zeroes_the_high_qword(self):
        lifter, out = _lift("movq", "xmm0, qword ptr [eax]",
                            [_xmm("xmm0"), _mem(8)])
        self.assertIn("XMM_LOAD_LOW(xmm0,", out)
        self.assertIn("xmm0.u[2] = xmm0.u[3] = 0", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_store_low_qword(self):
        lifter, out = _lift("movq", "qword ptr [eax], xmm1",
                            [_mem(8), _xmm("xmm1")])
        self.assertIn("XMM_STORE_LOW(", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_register_to_register_copies_low_and_zeroes_high(self):
        lifter, out = _lift("movq", "xmm0, xmm1", [_xmm("xmm0"), _xmm("xmm1")])
        self.assertIn("xmm0.u[0] = xmm1.u[0]", out)
        self.assertIn("xmm0.u[2] = xmm0.u[3] = 0", out)
        self.assertEqual(lifter.unimplemented, {})


class X87FallthroughTest(unittest.TestCase):

    def test_free_wait_and_stack_rotate_are_explicit(self):
        # ffree and fwait change no value here; fincstp/fdecstp only move TOP.
        for mnem, expect in (("ffree", "tag word is not modelled"),
                             ("fwait", "not modelled")):
            with self.subTest(mnemonic=mnem):
                lifter, out = _lift(mnem, "st(3)" if mnem == "ffree" else "",
                                    [_xmm("st(3)")] if mnem == "ffree" else [])
                self.assertNotIn("RECOMP_UNIMPL", out)
                self.assertIn(expect, out)
                self.assertEqual(lifter.unimplemented, {})
        for mnem, step in (("fincstp", "+ 1"), ("fdecstp", "- 1")):
            with self.subTest(mnemonic=mnem):
                lifter, out = _lift(mnem)
                self.assertIn(f"g_fp_top = (g_fp_top {step}) & 7;", out)
                self.assertNotIn("RECOMP_UNIMPL", out)
                self.assertEqual(lifter.unimplemented, {})

    def test_exception_clear_and_nop_are_explicit_no_ops(self):
        # The model keeps no x87 exception flags, so these change nothing. They
        # must not be counted as missing, or a reached one stops the title.
        for mnem in ("fnclex", "fnop"):
            with self.subTest(mnemonic=mnem):
                lifter, out = _lift(mnem)
                self.assertNotIn("RECOMP_UNIMPL", out)
                self.assertIn("not modelled", out)
                self.assertEqual(lifter.unimplemented, {})


if __name__ == "__main__":
    unittest.main()
