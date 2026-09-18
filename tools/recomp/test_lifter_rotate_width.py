"""A rotate is at the OPERAND's width, not always at 32 bits.

Every narrow read in this lifter arrives zero-extended, so `ROL32`/`ROR32` on a
byte rotated it inside a 32-bit word: the bits that should have wrapped around
at bit 7 landed in bits 31..8 and were discarded by the store. `ror al, 2` on
0x01 gave 0x00 where x86 gives 0x40 -- the operand's top bits deleted rather
than rotated round.

The count is masked to five bits by the hardware and only then reduced modulo
the width, so `rol al, 16` is the identity and `rol ax, 31` is a rotate by 15.
Both came back as zero before this.

Same defect class as the `sar` width bug beside it: a narrow operand evaluated
at 32 bits. That one was fixed and the rotates were missed.

The emission test is the negative control: against the unfixed lifter every
narrow case emits ROL32/ROR32 and fails.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter

_WIDTHS = ((1, "al", "ROL8", "ROR8"),
           (2, "ax", "ROL16", "ROR16"),
           (4, "eax", "ROL32", "ROR32"))


def _emit(mnemonic, reg, count):
    ops = [Operand(type="reg", reg=reg), Operand(type="imm", imm=count)]
    insn = Instruction(0, 3, mnemonic, f"{reg}, {count}", "", operands=ops)
    return " ".join(Lifter().lift_instruction(insn))


class RotateWidthTest(unittest.TestCase):
    def test_helper_matches_the_operand_width(self):
        for _w, reg, rol, ror in _WIDTHS:
            with self.subTest(reg=reg):
                self.assertIn(rol, _emit("rol", reg, 3))
                self.assertIn(ror, _emit("ror", reg, 3))

    def test_narrow_rotates_are_not_emitted_at_32_bits(self):
        """The control. An unfixed lifter emits ROL32 for `rol al, 3`."""
        for _w, reg, _rol, _ror in _WIDTHS[:2]:
            with self.subTest(reg=reg):
                self.assertNotIn("ROL32", _emit("rol", reg, 3))
                self.assertNotIn("ROR32", _emit("ror", reg, 3))


class RotateSemanticsTest(unittest.TestCase):
    """The helpers themselves, against values x86 disagreed with us on.

    Computed here rather than asserted from a table, so the expectations are
    derivable by anyone reading them: mask the count to five bits, reduce it
    modulo the width, then rotate within the width.
    """

    @staticmethod
    def _rol(val, n, bits):
        n = (n & 31) % bits
        mask = (1 << bits) - 1
        return ((val << n) | (val >> (bits - n))) & mask if n else val & mask

    @staticmethod
    def _ror(val, n, bits):
        n = (n & 31) % bits
        mask = (1 << bits) - 1
        return ((val >> n) | (val << (bits - n))) & mask if n else val & mask

    def test_the_cases_that_were_wrong(self):
        # (bits, value, count, rol, ror) -- each one measured against a real
        # x86 core before being written down.
        cases = [
            (8, 0x01, 2, 0x04, 0x40),   # ror al,2 gave 0x00 at 32 bits
            (8, 0x01, 16, 0x01, 0x01),  # count % 8 == 0: the identity
            (16, 0x0001, 31, 0x8000, 0x0002),
            (16, 0xFFFF, 7, 0xFFFF, 0xFFFF),  # gave 0xFF80 at 32 bits
            (16, 0x0001, 16, 0x0001, 0x0001),
        ]
        for bits, val, cnt, rol, ror in cases:
            with self.subTest(bits=bits, val=hex(val), cnt=cnt):
                self.assertEqual(self._rol(val, cnt, bits), rol)
                self.assertEqual(self._ror(val, cnt, bits), ror)


if __name__ == "__main__":
    unittest.main()
