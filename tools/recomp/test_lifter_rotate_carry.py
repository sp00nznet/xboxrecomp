"""rcl/rcr were a comment, and a comment is a silent no-op.

Where they appear is the compiler's own 64-bit divide, emitted by MSVC for
every `long long` division in the image:

    shr ecx, 1
    rcr ebx, 1        <- carries ecx's bit 0 into ebx's bit 31
    shr edx, 1
    rcr eax, 1
    or  ecx, ecx
    jnz ...

That loop normalises a 128-bit pair down to something a 32-bit divide can
take. With the rcr dropped the low halves never move, so both the divisor
and the dividend go into the divide wrong -- and nothing says so.

The emission test is the negative control: against the unfixed lifter every
case comes back as `/* TODO: rcr ... */`.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter
from .translator import FunctionTranslator


def _emit(mnemonic, reg, count):
    ops = [Operand(type="reg", reg=reg), Operand(type="imm", imm=count)]
    insn = Instruction(0, 3, mnemonic, f"{reg}, {count}", "", operands=ops)
    return " ".join(Lifter().lift_instruction(insn))


class RotateCarryEmissionTest(unittest.TestCase):
    def test_both_directions_are_lifted(self):
        for m in ("rcl", "rcr"):
            for reg in ("al", "ax", "eax"):
                with self.subTest(mnemonic=m, reg=reg):
                    out = _emit(m, reg, 1)
                    self.assertIn("RC_ROT", out)
                    self.assertNotIn("TODO", out)

    def test_the_operand_width_reaches_the_helper(self):
        self.assertIn("8,", _emit("rcr", "al", 1))
        self.assertIn("16,", _emit("rcr", "ax", 1))
        self.assertIn("32,", _emit("rcr", "eax", 1))

    def test_the_carry_is_read_and_written(self):
        """It is the whole point of the instruction: &_cf, not a copy."""
        self.assertIn("&_cf", _emit("rcr", "eax", 1))

    def test_a_function_containing_one_declares_the_carry(self):
        """Without this the generated C does not compile: _cf is only
        declared for functions that were detected as using it."""
        insns = [
            Instruction(0x1000, 2, "rcr", "eax, 1", "",
                        operands=[Operand(type="reg", reg="eax"),
                                  Operand(type="imm", imm=1)]),
            Instruction(0x1002, 1, "ret", "", "", operands=[]),
        ]
        self.assertTrue(FunctionTranslator._function_needs_cf(insns))


class RotateCarrySemanticsTest(unittest.TestCase):
    """The 33-bit rotation itself, computed rather than tabulated.

    A byte rotates through nine positions, not eight, because the carry is
    one of them -- which is why the count is reduced modulo width+1 and not
    modulo the width.
    """

    @staticmethod
    def _reference(val, n, cf, bits, left):
        mod = bits + 1
        n &= 31
        if bits < 32:
            n %= mod
        x = ((cf & 1) << bits) | (val & ((1 << bits) - 1))
        if n:
            x = ((x << n) | (x >> (mod - n))) if left else \
                ((x >> n) | (x << (mod - n)))
            x &= (1 << mod) - 1
        return x & ((1 << bits) - 1), (x >> bits) & 1

    def test_the_divide_loop_shifts_a_64_bit_pair(self):
        """shr high,1 / rcr low,1 is a 64-bit shift right by one."""
        for value in (1, 0x8000_0001, 0xFFFF_FFFF_FFFF_FFFF, 0x1234_5678_9ABC_DEF0):
            with self.subTest(value=value):
                high, low = value >> 32, value & 0xFFFF_FFFF
                carry_out = high & 1            # what `shr high, 1` leaves
                high >>= 1
                low, _ = self._reference(low, 1, carry_out, 32, left=False)
                self.assertEqual((high << 32) | low, value >> 1)

    def test_one_bit_rotates_round_the_carry(self):
        for bits in (8, 16, 32):
            top = 1 << (bits - 1)
            with self.subTest(bits=bits):
                # rcr with carry set drops the carry into the top bit.
                val, cf = self._reference(0, 1, 1, bits, left=False)
                self.assertEqual((val, cf), (top, 0))
                # rcl with carry set drops it into the bottom bit.
                val, cf = self._reference(0, 1, 1, bits, left=True)
                self.assertEqual((val, cf), (1, 0))
                # A whole turn of width+1 places is the identity.
                if bits < 32:
                    val, cf = self._reference(0x5A % (1 << bits), bits + 1, 1,
                                              bits, left=True)
                    self.assertEqual((val, cf), (0x5A % (1 << bits), 1))


if __name__ == "__main__":
    unittest.main()
