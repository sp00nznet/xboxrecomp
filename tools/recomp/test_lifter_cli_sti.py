"""`cli`, `sti`, `sgdt` and a CS-reload `ljmp` reach the runtime instead of logging as unimplemented.

Burnout 3's XPP reads the USB frame number and a timer register between a
cli and an sti (0x00367C86..0x00367C97), so the USB ISR cannot move one of
them in between. Lifted as RECOMP_UNIMPL no-ops they logged on every pass
and masked nothing.
"""
import pathlib
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic):
    lifter = Lifter()
    insn = Instruction(0x00367C86, 1, mnemonic, "", "", operands=[])
    return lifter, " ".join(lifter.lift_instruction(insn))


class CliStiTest(unittest.TestCase):
    def test_cli_masks_through_the_runtime(self):
        lifter, out = _lift("cli")
        self.assertIn("xbox_Cli();", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("cli", lifter.unimplemented)

    def test_sti_unmasks_through_the_runtime(self):
        lifter, out = _lift("sti")
        self.assertIn("xbox_Sti();", out)
        self.assertNotIn("RECOMP_UNIMPL", out)

    def test_sgdt_writes_the_runtime_gdtr(self):
        lifter = Lifter()
        op = Operand(type="mem", mem_base="esp", mem_disp=6)
        insn = Instruction(0x001D6AB4, 5, "sgdt", "[esp + 6]", "",
                           operands=[op])
        out = " ".join(lifter.lift_instruction(insn))
        self.assertIn("xbox_Sgdt((uint32_t)(", out)
        self.assertIn("esp", out)
        self.assertNotIn("RECOMP_UNIMPL", out)

    def test_far_jump_to_the_next_instruction_falls_through(self):
        lifter = Lifter()
        insn = Instruction(0x001D6AC5, 7, "ljmp", "8:0x1d6acc", "",
                           operands=[])
        out = " ".join(lifter.lift_instruction(insn))
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("goto", out)

    def test_other_far_jumps_stay_unimplemented(self):
        lifter = Lifter()
        insn = Instruction(0x001D6AC5, 7, "ljmp", "8:0x1d7000", "",
                           operands=[])
        out = " ".join(lifter.lift_instruction(insn))
        self.assertIn("RECOMP_UNIMPL", out)

    def test_helpers_are_declared_for_generated_code(self):
        hdr = (pathlib.Path(__file__).resolve().parents[2] / "templates" /
               "runtime" / "recomp_types.h").read_text(encoding="utf-8")
        self.assertIn("void xbox_Cli(void);", hdr)
        self.assertIn("void xbox_Sti(void);", hdr)
        self.assertIn("void xbox_Sgdt(uint32_t va);", hdr)


if __name__ == "__main__":
    unittest.main()
