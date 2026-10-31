import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class StringCompareLifterTest(unittest.TestCase):
    def test_repne_scasb_scans_until_equal(self):
        scan = Instruction(
            0, 2, "repne scasb", "al, byte ptr es:[edi]", "f2ae")

        lifted = Lifter().lift_instruction(scan)
        generated = "\n".join(lifted)

        self.assertIn("while (ecx != 0)", generated)
        self.assertIn("_flags = (LO8(eax) == MEM8(edi));", generated)
        self.assertIn("edi += _st; ecx--;", generated)
        self.assertIn("if (_flags) break;", generated)
        self.assertLess(
            generated.index("edi += _st; ecx--;"),
            generated.index("if (_flags) break;"),
        )

    def test_repe_cmpsb_compares_bytes_and_feeds_equal_jump(self):
        compare = Instruction(
            0, 2, "repe cmpsb", "byte ptr [esi], byte ptr es:[edi]",
            "f3a6")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=1),
            Operand(type="mem", mem_base="edi", mem_size=1),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        self.assertIn("_flags = (MEM8(esi) == MEM8(edi));", generated)
        self.assertIn("esi += _st; edi += _st; ecx--;", generated)
        self.assertIn("if (!_flags) break;", generated)
        self.assertIn("if ((_flags != 0)) goto loc_00000010;", generated)

    def test_dword_compare_sets_the_flags_its_jcc_reads(self):
        compare = Instruction(
            0, 2, "repe cmpsd", "dword ptr [esi], dword ptr es:[edi]",
            "f3a7")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=4),
            Operand(type="mem", mem_base="edi", mem_size=4),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        # Was a bare comment, so the jcc after it read whatever the previous
        # instruction had left in the flags. It compares four bytes at a time
        # and steps esi/edi by four, and the following je reads _flags.
        self.assertIn("_flags = (MEM32(esi) == MEM32(edi));", generated)
        self.assertIn("esi += _st; edi += _st; ecx--;", generated)
        # The step is four bytes, in whichever direction EFLAGS.DF says.
        self.assertIn("RECOMP_DF_STEP(4)", generated)
        self.assertIn("if (!_flags) break;", generated)
        # The je reads the flag the loop set, in whichever equivalent form
        # the emitter picks -- what matters is that it reads _flags and not
        # a stale _fa/_fb snapshot left by some earlier compare.
        jcc = generated.splitlines()[-1]
        self.assertIn("_flags", jcc)
        self.assertNotIn("_fa", jcc)


if __name__ == "__main__":
    unittest.main()


class WideCompareAfterXorTest(unittest.TestCase):
    def test_word_compare_overrides_an_earlier_xor(self):
        # XNet's XNetXnAddrToInAddr: `xor edx, edx; repe cmpsw; jne`. The
        # jne has to read the compare, not the zero the xor left in edx --
        # reading edx made every address compare equal to this machine's.
        clear = Instruction(0, 2, "xor", "edx, edx", "33d2")
        clear.operands = [Operand(type="reg", reg="edx"),
                          Operand(type="reg", reg="edx")]
        compare = Instruction(
            2, 3, "repe cmpsw", "word ptr [esi], word ptr es:[edi]",
            "f366a7")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=2),
            Operand(type="mem", mem_base="edi", mem_size=2),
        ]
        jump = Instruction(5, 2, "jne", "0x10", "7509")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[clear, compare, jump]))
        jcc = "\n".join(lifted).splitlines()[-1]
        self.assertIn("_flags", jcc)
        self.assertNotIn("edx", jcc)
