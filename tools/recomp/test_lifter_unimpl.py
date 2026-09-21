"""An instruction the lifter cannot translate must say so at runtime.

Until 19 Sep 2026 every path out of the lifter for an instruction it had no
translation for ended in a bare `/* TODO: mnemonic ops */` comment, which the
C compiler reads as nothing: the instruction vanished and the guest carried on.
Eleven such sites, three of which recorded the mnemonic in the translator's
end-of-run tally and eight of which did not. One title's gen tree carries 122 of
these comments and nothing could say whether any of them was ever reached.

The contract now is one helper, `Lifter._unimplemented`, and this pins its
three properties:

  1. the emitted C calls RECOMP_UNIMPL with the instruction text and its
     guest address, and keeps the TODO comment beside it for grep;
  2. the site is recorded in `lifter.unimplemented`, whichever path emitted it;
  3. instructions the lifter deliberately ignores -- the cache hints -- do
     NOT carry the marker, and neither does anything it translates. The
     ignored ones are decisions, written into the generated C as such, and a
     marker on them would make the runtime report count decisions as
     omissions.

The third is the negative control: without it a change that put RECOMP_UNIMPL
on every unhandled mnemonic including the deliberate ones would pass the first
two and flood the [UNIMPL] log with `prefetch`.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str="", operands=(), address=0x00012340):
    lifter = Lifter()
    insn = Instruction(address, 3, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


class UnimplementedMarkerTest(unittest.TestCase):

    def test_unknown_mnemonic_carries_the_runtime_marker(self):
        # `daa` has no translation and no operands, so nothing about operand
        # formatting can interfere with what is being tested.
        lifter, out = _lift("daa")
        self.assertIn('RECOMP_UNIMPL("daa", 0x00012340u);', out)
        self.assertIn("/* TODO: daa */", out)
        self.assertEqual(lifter.unimplemented, {"daa": [0x00012340]})

    def test_hlt_is_not_an_ignored_instruction(self):
        # The lifter's own comment says hlt is deliberately NOT in the ignored
        # set, because a no-op there turns a wait into a spin. It must be an
        # unimplemented site, with the marker, not a silent decision.
        lifter, out = _lift("hlt", address=0x00ABCDEF)
        self.assertIn('RECOMP_UNIMPL("hlt", 0x00ABCDEFu);', out)
        self.assertIn("hlt", lifter.unimplemented)

    def test_mmx_path_records_and_marks(self):
        # Before the helper, the MMX paths returned the comment WITHOUT
        # recording the site, so the translator's tally undercounted. A
        # binary MMX op with a destination that is not an mm register takes
        # the "(dst not mm)" exit.
        lifter, out = _lift("paddw", "eax, mm1", [_reg("eax"), _reg("mm1")])
        self.assertIn('RECOMP_UNIMPL("paddw eax, mm1", 0x00012340u);', out)
        self.assertIn("(dst not mm)", out)
        self.assertEqual(lifter.unimplemented, {"paddw": [0x00012340]})

    def test_ignored_hint_has_no_marker(self):
        # Negative control: a cache hint is a decision, not an omission.
        lifter, out = _lift("sfence")
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("TODO", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_translated_instruction_has_no_marker(self):
        # Negative control: an ordinary translated instruction carries neither
        # the marker nor the comment, and records nothing.
        lifter, out = _lift("mov", "eax, ebx", [_reg("eax"), _reg("ebx")])
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("TODO", out)
        self.assertEqual(lifter.unimplemented, {})


if __name__ == "__main__":
    unittest.main()
