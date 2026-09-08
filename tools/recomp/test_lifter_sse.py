import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


def _xmm(name):
    return Operand(type="reg", reg=name)


def _mem(base=None, disp=0, size=16):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


class SsePackedLifterTest(unittest.TestCase):
    """An XMM register is 128 bits wide. Lifting packed moves as scalar
    floats transferred 4 of every 16 bytes, which left every matrix built by
    packed SSE sparse, and dropped packed arithmetic to a bare comment."""

    def test_aligned_packed_load_transfers_sixteen_bytes(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movaps", "xmm0, xmmword ptr [eax]",
                      [_xmm("xmm0"), _mem("eax")])),
            ["xmm0 = XMM_MEM(eax); /* movaps */"],
        )

    def test_aligned_packed_store_transfers_sixteen_bytes(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movaps", "xmmword ptr [ecx], xmm0",
                      [_mem("ecx"), _xmm("xmm0")])),
            ["XMM_STORE(ecx, xmm0); /* movaps */"],
        )

    def test_unaligned_packed_move_is_also_sixteen_bytes(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movups", "xmmword ptr [ecx + 0x10], xmm1",
                      [_mem("ecx", 0x10), _xmm("xmm1")])),
            ["XMM_STORE(ecx + 0x10, xmm1); /* movups */"],
        )

    def test_scalar_move_stays_on_lane_zero(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movss", "xmm0, dword ptr [eax + 4]",
                      [_xmm("xmm0"), _mem("eax", 4, size=4)])),
            ["xmm0 = XMM_SCALAR(MEMF(eax + 4)); /* movss */"],
        )
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movss", "dword ptr [ecx], xmm2",
                      [_mem("ecx", 0, size=4), _xmm("xmm2")])),
            ["MEMF(ecx) = xmm2.f[0]; /* movss */"],
        )

    def test_packed_arithmetic_emits_a_statement(self):
        for mnemonic, helper in (("mulps", "XMM_MUL"), ("addps", "XMM_ADD"),
                                 ("subps", "XMM_SUB"), ("divps", "XMM_DIV")):
            self.assertEqual(
                Lifter().lift_instruction(
                    _insn(mnemonic, "xmm0, xmm1",
                          [_xmm("xmm0"), _xmm("xmm1")])),
                ["xmm0 = %s(xmm0, xmm1); /* %s */" % (helper, mnemonic)],
            )

    def test_packed_arithmetic_against_memory(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("mulps", "xmm0, xmmword ptr [0xa24190]",
                      [_xmm("xmm0"), _mem(disp=0xA24190)])),
            ["xmm0 = XMM_MUL(xmm0, XMM_MEM(0xA24190)); /* mulps */"],
        )

    def test_shuffle_uses_the_immediate(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("shufps", "xmm7, xmm7, 0",
                      [_xmm("xmm7"), _xmm("xmm7"),
                       Operand(type="imm", imm=0)])),
            ["xmm7 = XMM_SHUFFLE(xmm7, xmm7, 0); /* shufps */"],
        )

    def test_bitwise_ops_use_the_integer_lanes(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("andps", "xmm4, xmmword ptr [0x240430]",
                      [_xmm("xmm4"), _mem(disp=0x240430)])),
            ["xmm4 = XMM_AND(xmm4, XMM_MEM(0x240430)); /* andps */"],
        )

    def test_self_xor_zeroes_the_whole_register(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("xorps", "xmm0, xmm0",
                      [_xmm("xmm0"), _xmm("xmm0")])),
            ["xmm0 = XMM_ZERO(); /* xorps self = zero */"],
        )

    def test_move_mask_gathers_real_sign_bits(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movmskps", "eax, xmm3",
                      [Operand(type="reg", reg="eax"), _xmm("xmm3")])),
            ["eax = XMM_MOVEMASK(xmm3); /* movmskps */"],
        )

    def test_half_width_moves_target_one_half(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movlps", "xmm0, qword ptr [ecx]",
                      [_xmm("xmm0"), _mem("ecx", 0, size=8)])),
            ["XMM_LOAD_LOW(xmm0, ecx); /* movlps */"],
        )
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("movhps", "qword ptr [eax + 8], xmm1",
                      [_mem("eax", 8, size=8), _xmm("xmm1")])),
            ["XMM_STORE_HIGH(eax + 8, xmm1); /* movhps */"],
        )

    def test_scalar_arithmetic_reads_and_writes_lane_zero(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("mulss", "xmm0, xmm1",
                      [_xmm("xmm0"), _xmm("xmm1")])),
            ["xmm0.f[0] = xmm0.f[0] * xmm1.f[0]; /* mulss */"],
        )

    def test_no_packed_mnemonic_falls_through_to_a_bare_comment(self):
        packed = [
            ("movaps", [_xmm("xmm0"), _mem("eax")]),
            ("movups", [_xmm("xmm0"), _mem("eax")]),
            ("movlps", [_xmm("xmm0"), _mem("eax", 0, size=8)]),
            ("movhps", [_xmm("xmm0"), _mem("eax", 0, size=8)]),
            ("movlhps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("movhlps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("addps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("subps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("mulps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("divps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("minps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("maxps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("andps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("andnps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("orps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("xorps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("unpcklps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("unpckhps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("cmpeqps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("cmpneqps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("cmpltps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("cmpleps", [_xmm("xmm0"), _xmm("xmm1")]),
            ("shufps", [_xmm("xmm0"), _xmm("xmm1"),
                        Operand(type="imm", imm=0x55)]),
            ("movmskps", [Operand(type="reg", reg="eax"), _xmm("xmm0")]),
        ]
        for mnemonic, operands in packed:
            lifted = Lifter().lift_instruction(_insn(mnemonic, "", operands))
            self.assertEqual(len(lifted), 1, mnemonic)
            statement = lifted[0]
            self.assertIn(";", statement, (mnemonic, statement))
            self.assertFalse(
                statement.lstrip().startswith("/*"), (mnemonic, statement))


class CvtPi2PsTest(unittest.TestCase):
    """cvtpi2ps: two signed dwords out of an MMX register (or m64) become two
    singles in the LOW half of an xmm, upper lanes untouched.

    It has an xmm destination and an mm source, so it matched neither the mm
    paths nor the xmm ones and fell through to a TODO comment -- which deletes
    the instruction silently. In Half-Life 2's Xbox loader that removed every
    integer-to-float step of the XMV YUV-to-RGB converter; the float pipeline
    then ran on whatever the xmm registers held from before and painted the
    whole intro solid red. A dropped instruction is the worst kind of bug the
    recompiler can have, because everything downstream still runs.
    """

    def test_mmx_source_is_lifted(self):
        lifted = Lifter().lift_instruction(
            _insn("cvtpi2ps", "xmm0, mm0", [_xmm("xmm0"), _xmm("mm0")]))
        self.assertEqual(len(lifted), 1)
        self.assertIn("XMM_FROM_PI(xmm0, mm0)", lifted[0])

    def test_memory_source_is_lifted(self):
        lifted = Lifter().lift_instruction(
            _insn("cvtpi2ps", "xmm1, qword ptr [eax]",
                  [_xmm("xmm1"), _mem("eax", size=8)]))
        self.assertEqual(len(lifted), 1)
        self.assertIn("MMX_MEM(", lifted[0])

    def test_it_does_not_become_a_comment(self):
        # The regression itself: a TODO comment compiles and does nothing.
        statement = Lifter().lift_instruction(
            _insn("cvtpi2ps", "xmm0, mm0", [_xmm("xmm0"), _xmm("mm0")]))[0]
        self.assertNotIn("TODO", statement)
        self.assertFalse(statement.lstrip().startswith("/*"), statement)


if __name__ == "__main__":
    unittest.main()
