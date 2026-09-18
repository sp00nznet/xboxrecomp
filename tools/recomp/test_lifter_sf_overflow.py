"""SF after a compare is the sign bit of the WRAPPED difference.

`cmp` snapshots its operands sign-extended into _fas/_fbs, so the natural
spelling of `js` is

    (int32_t)((_fas) - (_fbs)) < 0

and that is what this lifter emitted. Signed overflow is undefined behaviour in
C, and this subtraction overflows for precisely the inputs the question is
about: `cmp 0x80000000, 1` leaves 0x7FFFFFFF on the hardware, so SF is 0, while
the C expression is INT_MIN - 1.

MEASURED, AND THE MEASUREMENT HAS A TRAP IN IT. At -O2 Apple clang 21 gets
both overflowing vectors wrong:

    cmp 0x80000000, 1          x86 SF=0    emitted 1
    cmp 0x7FFFFFFF, 0xFFFFFFFF x86 SF=1    emitted 0

But only when the operands are not compile-time constants. Call the same
function with literals and clang constant-folds it and produces the correct
wrapped answer, at every -O level -- so a quick check written that way reports
"no problem at -O0 through -O3" and is measuring the constant folder rather
than the emitted code. Guest operands are never literals, so the folded form
is the one that does not matter. The compiled arm below feeds its vectors
through an array for exactly this reason.

Unsigned subtraction is defined to wrap, so the fix is to subtract in the
unsigned type of the operand's own width and take the top bit. That is the
hardware's SF at every width with no undefined case.

Only `s`/`ns` are affected. `jl`/`jge`/`jle`/`jg` are NOT: SF != OF is
mathematically signed-less-than, and the CMP_L/CMP_G macros already compute it
without a subtraction.

The compiled arm below is the load-bearing one -- the defect is invisible at
-O0, so a test that does not optimise cannot see it.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Operand
from .lifter import _make_condition


def _cond(jcc, reg):
    ops = [Operand(type="reg", reg=reg), Operand(type="imm", imm=1)]
    return _make_condition(jcc, "cmp", ops)[0]


# (a, b, SF the hardware leaves after `cmp a, b`)
VECTORS = [
    (0x80000000, 0x00000001, 0),   # INT_MIN - 1 wraps to 0x7FFFFFFF
    (0x7FFFFFFF, 0xFFFFFFFF, 1),   # INT_MAX - (-1) wraps to 0x80000000
    (0x00000000, 0x00000001, 1),   # 0 - 1 = -1
    (0x00000001, 0x00000000, 0),
    (0x80000000, 0x80000000, 0),   # equal
    (0xFFFFFFFF, 0x7FFFFFFF, 1),   # -1 - INT_MAX = 0x80000000
]


class SignFlagOverflowTest(unittest.TestCase):
    def test_no_signed_subtraction_is_emitted(self):
        for jcc in ("js", "jns"):
            for reg in ("eax", "ax", "al"):
                with self.subTest(jcc=jcc, reg=reg):
                    expr = _cond(jcc, reg)
                    # NOT "int32_t)((" -- that is a substring of the
                    # uint32_t form and matched the fix itself.
                    self.assertNotIn("(int32_t)((", expr)
                    self.assertNotIn("(int16_t)((", expr)
                    self.assertNotIn("(int8_t)((", expr)
                    self.assertIn("uint", expr)

    def test_width_follows_the_operand(self):
        self.assertIn(">> 31", _cond("js", "eax"))
        self.assertIn(">> 15", _cond("js", "ax"))
        self.assertIn(">> 7", _cond("js", "al"))

    def test_the_emitted_expression_is_right_at_O2(self):
        """Compile the real emitted text at -O2 and check it against x86.

        This is the arm that would have caught the defect. At -O0 the old
        expression answers correctly and the test would have passed.
        """
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        if not cc:
            self.skipTest("no C compiler available")

        expr = _cond("js", "eax")
        cases = "\n".join(
            "    if (sf(0x%08Xu, 0x%08Xu) != %d) { "
            "printf(\"FAIL %08X %08X\\n\", 0x%08Xu, 0x%08Xu); bad++; }"
            % (a, b, want, a, b, a, b)
            for a, b, want in VECTORS)
        src = """#include <stdint.h>
#include <stdio.h>
static int sf(uint32_t a, uint32_t b) {
    uint32_t _fa = a, _fb = b;
    int32_t _fas = (int32_t)a, _fbs = (int32_t)b;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    return %s ? 1 : 0;
}
int main(void) {
    int bad = 0;
%s
    printf("%%s\\n", bad ? "BAD" : "OK");
    return bad != 0;
}
""" % (expr, cases)

        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, "sf.c")
            exe = os.path.join(d, "sf")
            with open(c, "w") as f:
                f.write(src)
            build = subprocess.run(
                [cc, "-O2", "-fstrict-overflow", "-o", exe, c],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0,
                             "emitted SF disagrees with x86 at -O2:\n"
                             + run.stdout)

    def test_the_old_form_is_undefined_and_the_new_one_is_not(self):
        """The defect, demonstrated rather than asserted.

        The arm above pins the answers. This one pins the CAUSE, so that a
        future compiler which happens to fold the other way cannot make the
        defect look fixed: UBSan traps the old expression on the
        0x80000000 / 1 vector and does not trap the new one. The property the
        fix is for is "this expression is undefined", and that holds whatever
        the optimiser chooses to do with it in any given year.
        """
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        if not cc:
            self.skipTest("no C compiler available")

        def build_and_run(body):
            src = (
                "#include <stdint.h>\n"
                "#include <stdio.h>\n"
                "int main(void) {\n"
                "    uint32_t a = 0x80000000u, b = 1u;\n"
                "    uint32_t _fa = a, _fb = b;\n"
                "    int32_t _fas = (int32_t)a, _fbs = (int32_t)b;\n"
                "    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;\n"
                '    printf("%d\\n", (' + body + ') ? 1 : 0);\n'
                "    return 0;\n"
                "}\n")
            with tempfile.TemporaryDirectory() as d:
                c = os.path.join(d, "t.c")
                exe = os.path.join(d, "t")
                with open(c, "w") as f:
                    f.write(src)
                built = subprocess.run(
                    [cc, "-O2", "-fsanitize=signed-integer-overflow",
                     "-o", exe, c], capture_output=True, text=True)
                if built.returncode != 0:
                    return None
                r = subprocess.run([exe], capture_output=True, text=True)
                return r.stdout + r.stderr

        old = build_and_run("((int32_t)((_fas) - (_fbs)) < 0)")
        if old is None:
            self.skipTest("this compiler has no signed-integer-overflow sanitiser")
        self.assertIn("signed integer overflow", old,
                      "the old form no longer trips UBSan; re-check the "
                      "premise of this fix before deleting it")

        new = build_and_run(_cond("js", "eax"))
        self.assertNotIn("runtime error", new,
                         "the emitted expression is still undefined:\n" + new)
        self.assertEqual(new.strip(), "0",
                         "x86 leaves SF=0 for cmp 0x80000000, 1")


if __name__ == "__main__":
    unittest.main()
