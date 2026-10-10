"""lahf after ucomiss: MSVC's `ucomiss x, 0; lahf; test ah, 0x44; jnp` is its
"is this float zero" test. lahf used to lift to a comment, so AH kept whatever
EAX held and the guard answered at random -- Burnout 3 divided 0 by 0 behind
one and put a NaN in the player's position."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _xmm(n):
    return Operand(type="reg", reg="xmm%d" % n)


def _block():
    cmp = Instruction(0, 3, "ucomiss", "xmm0, xmm4", "0f2ec4")
    cmp.operands = [_xmm(0), _xmm(4)]
    lahf = Instruction(3, 1, "lahf", "", "9f")
    lahf.operands = []
    return BasicBlock(start=0, instructions=[cmp, lahf])


class LahfTest(unittest.TestCase):
    def test_lahf_loads_ah_from_the_compare(self):
        out = "\n".join(lift_basic_block(Lifter(), _block())[0])
        self.assertIn("eax = (eax & 0xFFFF00FFu)", out)
        self.assertNotIn("/* lahf - load AH", out)
        # ZF (0x40) must be set on equal AND on unordered, as the hardware does.
        self.assertIn("(_fca == _fcb || (_fca != _fca || _fcb != _fcb))) ? 0x40u", out)
        # PF (0x04) is the unordered bit; CF (0x01) is below-or-unordered.
        self.assertIn("((_fca != _fca || _fcb != _fcb)) ? 0x04u", out)
        self.assertIn("0x02u", out)


if __name__ == "__main__":
    unittest.main()


class FpuUnorderedTest(unittest.TestCase):
    """fcomi/fucomi/sahf set ZF, PF and CF on an unordered compare. The
    conditions read those bits from the _fa snapshot that the setter takes,
    so a later x87 compare or AH edit cannot change them; sahf takes a new
    snapshot from AH."""

    def test_ja_is_false_and_jb_true_when_unordered(self):
        from .lifter import _make_condition
        ja = _make_condition("ja", "fucomip", [])[0]
        jb = _make_condition("jb", "fucomip", [])[0]
        jp = _make_condition("jp", "sahf", [])[0]
        self.assertIn("(!(_fa & 1u) && !(_fa & 0x40u))", ja)
        self.assertIn("(_fa & 1u)", jb)
        self.assertIn("(_fa & 4u)", jp)
        self.assertNotIn("g_fp_cmp", ja + jb + jp)
