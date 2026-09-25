"""FRNDINT must round under the guest's x87 rounding control, not the host's.

It was lifted as `rint(fp_top())`, which rounds in the HOST's mode -- nearest,
always, because the guest's `fldcw` only ever reaches g_fp_control_word. MSVC's
floor() and ceil() are `_ctrlfp(RC=down/up); _frnd(); _ctrlfp(old)`, and
_frnd is a bare frndint, so both of them rounded to nearest: floor of 2.7 was 3.
A title that indexes a keyframe table with floor() and ceil() then reads one
key past the end once per animation loop.

The emitted text is checked here, and the helper is compiled and run: a text
check cannot tell a correct nearest-even from one that rounds halves up.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction
from .lifter import Lifter

_ROOT = Path(__file__).resolve().parents[2]


class FrndintLiftTest(unittest.TestCase):
    def test_frndint_reads_the_guest_control_word(self):
        insn = Instruction(0x0017EEDB, 2, "frndint", "", "")
        insn.operands = []
        out = "\n".join(Lifter().lift_instruction(insn))
        self.assertIn("fp_top() = recomp_frndint(fp_top(), g_fp_control_word);", out)
        self.assertNotIn("rint(fp_top())", out.replace("recomp_frndint(fp_top()", ""))


class FrndintRuntimeTest(unittest.TestCase):
    def test_each_rounding_control_rounds_its_own_way(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        source = r'''
#include <stdint.h>
#include <math.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

#define NEAR 0x037Fu   /* the x87 default: RC=00 */
#define DOWN 0x077Fu   /* RC=01, what floor() loads */
#define UP   0x0B7Fu   /* RC=10, what ceil() loads */
#define CHOP 0x0F7Fu   /* RC=11 */

int main(void) {
    /* The defect: floor and ceil of a fraction above one half. */
    if (recomp_frndint(59.6, DOWN) != 59.0) return __LINE__;
    if (recomp_frndint(59.6, UP)   != 60.0) return __LINE__;
    if (recomp_frndint(59.4, UP)   != 60.0) return __LINE__;
    if (recomp_frndint(-2.5, DOWN) != -3.0) return __LINE__;
    if (recomp_frndint(-2.5, UP)   != -2.0) return __LINE__;
    if (recomp_frndint(-2.7, CHOP) != -2.0) return __LINE__;
    if (recomp_frndint(2.7, CHOP)  !=  2.0) return __LINE__;
    /* Nearest-even: halves go to the even neighbour. */
    if (recomp_frndint(2.5, NEAR)  !=  2.0) return __LINE__;
    if (recomp_frndint(3.5, NEAR)  !=  4.0) return __LINE__;
    if (recomp_frndint(-1.5, NEAR) != -2.0) return __LINE__;
    if (recomp_frndint(2.4, NEAR)  !=  2.0) return __LINE__;
    /* Integral values, huge values and non-finite values pass through. */
    if (recomp_frndint(60.0, DOWN) != 60.0) return __LINE__;
    if (recomp_frndint(60.0, UP)   != 60.0) return __LINE__;
    if (recomp_frndint(1e300, UP) != 1e300) return __LINE__;
    if (!isnan(recomp_frndint((double)NAN, DOWN))) return __LINE__;
    if (recomp_frndint((double)INFINITY, CHOP) != (double)INFINITY) return __LINE__;
    /* A zero result keeps the operand's sign. */
    if (!signbit(recomp_frndint(-0.3, NEAR))) return __LINE__;
    if (!signbit(recomp_frndint(-0.7, UP)))   return __LINE__;
    if (signbit(recomp_frndint(0.3, DOWN)))   return __LINE__;
    /* FIST shares the rounding, and keeps its integer indefinite. */
    if (recomp_fist(59.6, DOWN, 32) != 59) return __LINE__;
    if (recomp_fist(59.6, NEAR, 32) != 60) return __LINE__;
    if (recomp_fist((double)NAN, NEAR, 32) != INT32_MIN) return __LINE__;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="frndint-runtime-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode,
                             f"check on line {ran.returncode} failed")


if __name__ == "__main__":
    unittest.main()
