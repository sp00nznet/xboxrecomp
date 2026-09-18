"""bts/btr/btc report the bit they found, not the bit they left.

All four bit-test instructions copy the tested bit into CF. Only `bt` leaves
the bit alone; `bts` sets it, `btr` clears it and `btc` flips it. The lifter
answered a following carry condition by reading the bit a second time, at the
consumer -- which for the three modifying forms reads back whatever they just
wrote:

    bts -> the bit is now 1, so "jb" was ALWAYS taken
    btr -> the bit is now 0, so "jb" was NEVER taken
    btc -> the bit is inverted, so the branch went exactly the wrong way

That is the test-and-set idiom -- "did I claim this, or was it already taken?"
-- reading its own answer. MSVC emits it for lock acquisition and for the
character-map loops behind strpbrk/strspn/strcspn, where `bts [esp], eax`
populates a 256-bit set and the carry says whether the character was already
present. Against the old code every character in such a set reported "already
present" on the first insertion.

The lift already captured the pre-write value into _cf. Two things stopped it
being used: the consumer preferred its own re-read, and _function_needs_cf did
not count the bt family as CF producers -- CF_TRACKED is only add/sub/adc/sbb/
shl/shr/sar -- so for a function whose sole carry consumer followed a `bts`,
the capture was never even emitted. Both halves are fixed; this pins both.

Plain `bt` keeps the re-read deliberately: it is exact, and it avoids forcing
_cf emission across a whole function for the common read-only case.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FunctionTranslator


def _reg_block(mnemonic, jcc, needs_cf=True):
    bit = Instruction(0, 3, mnemonic, "edx, eax", "0fabc2")
    bit.operands = [Operand(type="reg", reg="edx"),
                    Operand(type="reg", reg="eax")]
    branch = Instruction(3, 2, jcc, "0x20", "7219")
    branch.operands = []
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    lifted, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=[bit, branch]))
    return "\n".join(lifted)


def _insns(mnemonic, jcc):
    bit = Instruction(0, 3, mnemonic, "edx, eax", "0fabc2")
    bit.operands = [Operand(type="reg", reg="edx"),
                    Operand(type="reg", reg="eax")]
    branch = Instruction(3, 2, jcc, "0x20", "7219")
    branch.operands = []
    return [bit, branch]


class BitTestCarryTest(unittest.TestCase):
    def test_modifying_forms_answer_from_the_snapshot(self):
        # The condition must be _cf, and the capture must precede the write.
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                generated = _reg_block(mnemonic, "jb")
                self.assertIn("if (_cf)", generated)
                capture = generated.index("_cf = ")
                write = generated.index("edx = (edx")
                self.assertLess(capture, write,
                                "CF must be captured before the bit is written")

    def test_modifying_forms_do_not_reread_the_bit(self):
        # The exact defect: the condition re-reading the operand it just wrote.
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                generated = _reg_block(mnemonic, "jb")
                condition = generated.splitlines()[-1]
                self.assertNotIn(">>", condition)

    def test_inverted_condition_negates_the_snapshot(self):
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn("if (!_cf)", _reg_block(mnemonic, "jae"))

    def test_plain_bt_still_reads_the_bit(self):
        # bt does not modify, so the re-read is exact and is kept -- otherwise
        # every function containing a bt would be forced to publish _cf.
        generated = _reg_block("bt", "jb")
        self.assertIn("(edx >> (eax & 31)) & 1", generated.splitlines()[-1])

    def test_bit_string_form_also_answers_from_the_snapshot(self):
        # bts with a memory base and a register offset is the strpbrk shape.
        dst = Operand(type="mem", mem_base="esp", mem_index=None,
                      mem_scale=1, mem_disp=0, mem_size=4)
        bit = Instruction(0, 4, "bts", "dword ptr [esp], eax", "0fab0424")
        bit.operands = [dst, Operand(type="reg", reg="eax")]
        branch = Instruction(4, 2, "jb", "0x20", "7219")
        branch.operands = []
        lifter = Lifter()
        lifter.needs_cf = True
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[bit, branch]))
        generated = "\n".join(lifted)
        self.assertIn("if (_cf)", generated)
        self.assertLess(generated.index("_cf = "), generated.index("= (MEM32"))

    def test_the_snapshot_is_actually_emitted_for_these_functions(self):
        # The other half of the bug. Without this, the consumer would name a
        # _cf the translator never published.
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                self.assertTrue(
                    FunctionTranslator._function_needs_cf(
                        _insns(mnemonic, "jb")),
                    "a carry branch after %s must publish _cf" % mnemonic)

    def test_plain_bt_does_not_force_cf_publication(self):
        self.assertFalse(
            FunctionTranslator._function_needs_cf(_insns("bt", "jb")))


if __name__ == "__main__":
    unittest.main()
