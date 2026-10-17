"""`in` and `out` reach the runtime instead of vanishing.

Lifted as no-ops, `in al, dx` left AL holding whatever it held before.
D3D's vblank ISR reads the video encoder's field pin that way (port 0x80C0,
bit 5), so the field it recorded never changed, D3DDevice_GetDisplayFieldStatus
said "odd" for ever, and the XMV player -- which starts its clock on an even
field -- never showed Burnout 3's first intro frame.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str, operands):
    lifter = Lifter()
    insn = Instruction(0x00356B4C, 1, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


class PortIoTest(unittest.TestCase):

    def test_in_al_dx_writes_only_al(self):
        lifter, out = _lift("in", "al, dx", [_reg("al"), _reg("dx")])
        self.assertIn("SET_LO8(eax, xbox_PortIn((edx & 0xFFFFu), 1));", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_in_eax_imm_port(self):
        _, out = _lift("in", "eax, 0x61", [_reg("eax"), _imm(0x61)])
        self.assertIn("xbox_PortIn(0x61u, 4)", out)

    def test_out_dx_al(self):
        lifter, out = _lift("out", "dx, al", [_reg("dx"), _reg("al")])
        self.assertIn("xbox_PortOut((edx & 0xFFFFu), (uint32_t)(LO8(eax)), 1);",
                      out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_in_ax_dx_writes_only_ax(self):
        _, out = _lift("in", "ax, dx", [_reg("ax"), _reg("dx")])
        self.assertIn("SET_LO16(eax, xbox_PortIn((edx & 0xFFFFu), 2));", out)

    def test_out_imm_port_eax(self):
        _, out = _lift("out", "0x80, eax", [_imm(0x80), _reg("eax")])
        self.assertIn("xbox_PortOut(0x80u, (uint32_t)(eax), 4);", out)

    def test_rep_insb_is_not_a_silent_comment(self):
        # The rep-string lifter's fallback used to return a bare comment, so
        # a port block transfer vanished with nothing at run time to say so.
        lifter, out = _lift("rep insb", "byte ptr es:[edi], dx", [])
        self.assertIn('RECOMP_UNIMPL("rep insb', out)
        self.assertIn("rep insb", lifter.unimplemented)

    def test_helpers_are_declared_for_generated_code(self):
        import pathlib
        hdr = (pathlib.Path(__file__).resolve().parents[2] / "templates" /
               "runtime" / "recomp_types.h").read_text(encoding="utf-8")
        self.assertIn("uint32_t xbox_PortIn(uint32_t port, int width);", hdr)
        self.assertIn("void     xbox_PortOut(", hdr)


if __name__ == "__main__":
    unittest.main()
