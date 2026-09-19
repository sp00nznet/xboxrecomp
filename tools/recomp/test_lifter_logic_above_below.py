"""Self-check for `jbe`/`ja` after and/or/xor.

and/or/xor clear CF, so `jb` and `jae` after one of them really are the
constants 0 and 1. `jbe` and `ja` are not: jbe is CF|ZF and ja is !CF && !ZF,
and ZF is whatever the result was. Folding all four to constants meant
"and eax, eax; jbe" never branched and "and eax, eax; ja" always did -- a
branch that is wrong exactly when the result is zero, and right the rest of
the time, so it survives most traffic and then takes the wrong arm on the
empty list, the null handle, the zero count.

The lifter already spells both conditions out correctly elsewhere: the
CF_TRACKED path emits "(_cf || lhs == 0)" and "(!_cf && lhs != 0)", and `test`
goes through CMP_BE/CMP_A. With CF known 0 those are exactly ZF and !ZF.

The sweep below compiles the lifter's own condition and compares it against CF
and ZF computed from x86's definitions, and is paired with a negative control
that feeds the same harness the pre-fix constants and requires them to fail.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Operand
from .lifter import _make_condition


SETTERS = ("and", "or", "xor")
C_OP = {"and": "&", "or": "|", "xor": "^"}

# jcc -> how x86 defines it, from CF and ZF.
REFERENCE = {
    "jb":  "(cf)",
    "jae": "(!cf)",
    "jbe": "(cf || zf)",
    "jna": "(cf || zf)",
    "ja":  "(!cf && !zf)",
}

SOURCE = r"""
#include <stdint.h>
#include <stdio.h>
/* and/or/xor publish their result here next to the write, so the condition
   reads what the instruction produced rather than a destination something
   may have overwritten between the two. */
static uint32_t _fa, _fb;
static int32_t _fas, _fbs;
int main(void) {
    static const uint32_t VS[] = {
        0u, 1u, 2u, 3u, 0xFFu, 0x100u, 0x8000u, 0xFFFFu, 0x10000u,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu, 0xA5A5A5A5u,
        0x5A5A5A5Au, 0xF0F0F0F0u, 0x0F0F0F0Fu, 0xCAFEBABEu
    };
    const unsigned N = sizeof(VS) / sizeof(VS[0]);
    unsigned saw_zero = 0, saw_nonzero = 0;
    for (unsigned i = 0; i < N; i++) {
        for (unsigned j = 0; j < N; j++) {
            /* The instruction writes its result to the destination; the jcc
               that follows reads that destination. */
            uint32_t eax = VS[i] OP VS[j];
            /* ...and the condition reads the published result. */
            _fa = eax; _fas = (int32_t)_fa;
            /* x86: and/or/xor clear CF and OF, and set ZF from the result. */
            const int cf = 0;
            const int zf = (eax == 0);
            if (zf) saw_zero++; else saw_nonzero++;
            const int got  = !!(COND);
            const int want = !!(REF);
            if (got != want) {
                printf("FAIL a=0x%X b=0x%X res=0x%X got=%d want=%d\n",
                       VS[i], VS[j], eax, got, want);
                return 1;
            }
        }
    }
    /* A sweep that never produced a zero result would pass against the old
       constants too, and prove nothing. */
    if (!saw_zero || !saw_nonzero) {
        printf("FAIL sweep degenerate: zero=%u nonzero=%u\n",
               saw_zero, saw_nonzero);
        return 1;
    }
    printf("OK %u zero %u nonzero\n", saw_zero, saw_nonzero);
    return 0;
}
"""


def _find_cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


class LogicAboveBelowTest(unittest.TestCase):
    def setUp(self):
        if not _find_cc():
            self.skipTest("no C compiler on PATH")
        self.ops = [Operand(type="reg", reg="eax"),
                    Operand(type="reg", reg="eax")]

    def _sweep(self, setter, jcc, cond):
        src = (SOURCE
               .replace("OP", C_OP[setter])
               .replace("COND", cond)
               .replace("REF", REFERENCE[jcc]))
        cc = _find_cc()
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            with open(c, "w") as f:
                f.write(src)
            exe = os.path.join(tmp, "t.exe")
            r = subprocess.run([cc, "-w", "-O2", c, "-o", exe],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-1500:] + "\n" + src)
            r = subprocess.run([exe], capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

    def test_below_or_equal_and_above_follow_zf(self):
        """jbe/ja after and/or/xor must track the result, not a constant."""
        for setter in SETTERS:
            for jcc in ("jbe", "jna", "ja"):
                with self.subTest(setter=setter, jcc=jcc):
                    cond = _make_condition(jcc, setter, self.ops)
                    self.assertIsNotNone(cond, f"{setter}/{jcc} lifted to nothing")
                    rc, out = self._sweep(setter, jcc, cond[0])
                    self.assertEqual(rc, 0, f"{setter}/{jcc} -> {cond[0]}: {out}")
                    self.assertTrue(out.startswith("OK"), out)

    def test_pre_fix_constants_are_rejected(self):
        """Negative control: the old constants must fail the same sweep.

        jbe folded to "0" and ja folded to "1".
        """
        for setter in SETTERS:
            for jcc, old in (("jbe", "0"), ("jna", "0"), ("ja", "1")):
                with self.subTest(setter=setter, jcc=jcc):
                    self.assertNotEqual(
                        old, _make_condition(jcc, setter, self.ops)[0])
                    rc, out = self._sweep(setter, jcc, old)
                    self.assertNotEqual(
                        rc, 0,
                        f"{setter}/{jcc}: the constant {old} passed the sweep, "
                        f"so the sweep does not test anything: {out}")
                    self.assertIn("FAIL", out)

    def test_carry_only_conditions_stay_constant(self):
        """jb/jae really are constants after and/or/xor: CF is 0.

        Kept as a sweep rather than a text assertion so that the claim is
        checked the same way as the one above, against the same CF/ZF model.
        """
        for setter in SETTERS:
            for jcc, expect in (("jb", "0"), ("jae", "1")):
                with self.subTest(setter=setter, jcc=jcc):
                    cond = _make_condition(jcc, setter, self.ops)
                    self.assertEqual(cond[0], expect)
                    rc, out = self._sweep(setter, jcc, cond[0])
                    self.assertEqual(rc, 0, f"{setter}/{jcc}: {out}")

    def test_matches_how_the_lifter_spells_it_elsewhere(self):
        """The same two conditions, via CF_TRACKED, with CF folded out.

        sub is CF-tracked, so it emits the full form. With CF = 0 -- which is
        what and/or/xor leave -- it reduces to what and/or/xor should emit.

        Both spell the result `_fa`, the snapshot the setter published next
        to its write, rather than re-reading the destination at the branch.
        """
        self.assertEqual(
            _make_condition("jbe", "sub", self.ops)[0], "(_cf || _fa == 0)")
        self.assertEqual(
            _make_condition("ja", "sub", self.ops)[0], "(!_cf && _fa != 0)")
        for setter in SETTERS:
            self.assertEqual(
                _make_condition("jbe", setter, self.ops)[0], "(_fa == 0)")
            self.assertEqual(
                _make_condition("ja", setter, self.ops)[0], "(_fa != 0)")


if __name__ == "__main__":
    unittest.main()
