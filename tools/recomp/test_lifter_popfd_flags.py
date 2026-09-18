"""popfd replaces every flag, so a jcc after it must not read the old compare.

`popfd` was listed in _EFLAGS_PRESERVE, the set of instructions that do not
touch EFLAGS, alongside `pushfd`. `pushfd` belongs there: it reads the flags
and leaves them alone. `popfd` loads EFLAGS from the stack and overwrites all
of them, so flag tracking must stop at it.

The consequence was that

    cmp eax, ebx
    popfd
    je  target

resolved the `je` from the `cmp`, three instructions and one full flag
replacement earlier. That is the CRT's flag save/restore idiom -- and anything
that restores a saved flags word -- getting the branch it was restoring the
flags for decided by the comparison the restore was meant to discard.

The file already knew this in one place and not the other: the neg/sbb
peephole walks forward over _EFLAGS_PRESERVE instructions looking for a carry
consumer and explicitly wrote `and insns[j].mnemonic != "popfd"` to stop
itself. The main tracking loop had no such guard. The set now carries the
fact, so the peephole no longer has to name it.

WHAT THIS TRADE IS, STATED PLAINLY. An unresolved condition currently emits no
branch at all -- the jcc becomes a comment and the block falls through. So this
turns a confidently wrong branch into a missing one. That is the right
direction on x86 semantics, because "I do not know these flags" is true and
"they are still the ones from that cmp" is false; but the missing-branch
behaviour is a pre-existing weakness in how unresolved conditions are handled
at all (it affects mul, div, cpuid and rdtsc identically) and this change
merely reaches it in one more place. Implementing popfd's flag effect properly
is the fix that would do better, and it is a larger change than this one.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import (Lifter, lift_basic_block,
                     _EFLAGS_PRESERVE, _FLAGS_UNDEFINED)


def _lift(middle):
    cmp_ = Instruction(0, 2, "cmp", "eax, ebx", "39d8")
    cmp_.operands = [Operand(type="reg", reg="eax"),
                     Operand(type="reg", reg="ebx")]
    insns = [cmp_]
    off = 2
    for m in middle:
        i = Instruction(off, 1, m, "", "9d")
        i.operands = []
        insns.append(i)
        off += 1
    je = Instruction(off, 2, "je", "0x40", "7440")
    je.operands = []
    insns.append(je)
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=insns))
    return "\n".join(lifted)


class PopfdFlagTrackingTest(unittest.TestCase):
    def test_popfd_is_not_treated_as_flag_preserving(self):
        self.assertNotIn("popfd", _EFLAGS_PRESERVE)
        self.assertIn("popfd", _FLAGS_UNDEFINED)

    def test_pushfd_still_is(self):
        # pushfd reads the flags without changing them; it must stay.
        self.assertIn("pushfd", _EFLAGS_PRESERVE)

    def test_a_branch_after_popfd_does_not_use_the_old_compare(self):
        self.assertNotIn("CMP_EQ", _lift(["popfd"]))

    def test_the_same_branch_without_popfd_still_resolves(self):
        # The positive control. Without it, a lifter that resolved NOTHING
        # would pass the test above.
        self.assertIn("CMP_EQ", _lift([]))

    def test_a_genuinely_preserving_instruction_still_preserves(self):
        # nop is in _EFLAGS_PRESERVE and must stay there: this pins that the
        # change was to popfd and not to the mechanism.
        self.assertIn("CMP_EQ", _lift(["nop"]))

    def test_popfd_anywhere_in_the_run_breaks_the_chain(self):
        self.assertNotIn("CMP_EQ", _lift(["nop", "popfd", "nop"]))


if __name__ == "__main__":
    unittest.main()
