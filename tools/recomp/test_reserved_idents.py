"""Self-check that no reserved name reaches the generated C unmangled.

`_func_ident` is the last guard before a func_db name becomes a C token, and
the only one every name source passes through -- Ghidra, IDA, a hand-written
symbols file. `tools/ghidra_naming/merge_names.py` filters a similar set
earlier, but it sees the Ghidra path alone.

The C99 `<math.h>` classification names are the sharp case, because they are
function-like *macros*. A guest function recovered as `isnan` does not
collide, it expands: `void isnan(void);` becomes
`void (fpclassify(void) == FP_NAN);` and the compiler reports a bad parameter
declarator inside `corecrt_math.h`, naming neither the guest function nor the
clash. Reported from a FIFA Street 2 port, where it cost a day.

Both checks compile against the headers a generated TU actually includes, and
the positive sweep is paired with a negative control that feeds the same
harness the unmangled name and requires it to fail -- a sweep that passes
against both spellings is testing nothing.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .lifter import _FUNC_RESERVED_IDENT, _func_ident


ADDR = 0x000A1C60

# What a generated chunk sees: recomp_types.h pulls in string.h and math.h,
# the dispatch TU adds stdlib.h, and setjmp.h arrives with the SEH helpers.
HEADERS = """#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdint.h>
"""

# The names most likely to be recovered from a real title, and the ones whose
# failure mode is a preprocessor expansion rather than a redeclaration. These
# are declared by any hosted C library, so the negative control can demand them
# everywhere.
SHARP_PORTABLE = ("isnan", "isinf", "isfinite", "isnormal", "signbit",
                  "fpclassify", "round", "trunc", "div", "exit", "abs")

# Declared only by the Microsoft CRT. It belongs in the reserved set -- a title
# built with MSVC can well have a function named `onexit`, and there it does
# collide -- but elsewhere the name is simply free, so demanding that it fail to
# compile would be asserting a fact about Windows on a machine that is not
# Windows.
SHARP_MSVC = ("onexit",)


def _find_cc():
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc and os.path.exists(r"C:\Program Files\LLVM\bin\clang.exe"):
        cc = r"C:\Program Files\LLVM\bin\clang.exe"
    return cc


def _targets_msvc_crt():
    """Does the compiler under test build against the Microsoft CRT?"""
    rc, _ = _compile("#ifndef _MSC_VER\n#error not the Microsoft CRT\n#endif\n")
    return rc == 0

def sharp_names():
    """The names the negative control demands for this toolchain."""
    return SHARP_PORTABLE + (SHARP_MSVC if _targets_msvc_crt() else ())

def _compile(body):
    """Compile one TU. Returns (returncode, stderr)."""
    cc = _find_cc()
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "t.c")
        with open(src, "w") as f:
            f.write(HEADERS + body)
        r = subprocess.run([cc, "-c", src, "-o", os.path.join(tmp, "t.o")],
                           capture_output=True, text=True)
        return r.returncode, r.stderr


class ReservedIdentTest(unittest.TestCase):
    def setUp(self):
        if not _find_cc():
            self.skipTest("no C compiler on PATH")

    def test_every_reserved_name_compiles_once_mangled(self):
        """The whole set, emitted the way the translator emits a function."""
        body = []
        for name in sorted(_FUNC_RESERVED_IDENT):
            if not name.isidentifier():
                continue
            ident = _func_ident(ADDR, name)
            self.assertNotEqual(ident, name, f"{name} was not mangled")
            body.append(f"void {ident}(void);\nvoid {ident}(void) {{ }}")
        rc, err = _compile("\n".join(body))
        self.assertEqual(rc, 0, err[-2000:])

    def test_unmangled_names_are_rejected(self):
        """Negative control: without the mangle, these must fail to build.

        Guards the sweep above. If the reserved set were emptied, every name
        would compile as an ordinary identifier and that test would still
        pass.
        """
        names = sharp_names()
        self.assertTrue(names, "no names to check: the control tests nothing")
        for name in names:
            with self.subTest(name=name):
                self.assertIn(name, _FUNC_RESERVED_IDENT,
                              f"{name} is missing from the reserved set")
                rc, _ = _compile(f"void {name}(void);\nvoid {name}(void) {{ }}")
                self.assertNotEqual(
                    rc, 0,
                    f"`void {name}(void);` compiled clean, so it does not "
                    f"need to be in the reserved set -- or the headers this "
                    f"test includes no longer match a generated TU")

    def test_ordinary_names_are_left_alone(self):
        """Mangling is not free: it shows up in every trace and backtrace."""
        for name in ("MyGameFunc", "sub_00401000", "CPlayer__Update",
                     "isnan_helper", "rounding"):
            self.assertEqual(_func_ident(ADDR, name), name)

    def test_merge_names_is_not_the_stronger_list(self):
        """The generation-time guard must cover the merge-time one.

        merge_names only sees Ghidra. If a name is filtered there but not
        here, the IDA path and any hand-written symbols file walk straight
        past it into the generated C.
        """
        from tools.ghidra_naming.merge_names import RESERVED
        missing = sorted(
            n for n in RESERVED
            if n.isidentifier() and n in _MATH_AND_STDLIB and
            _func_ident(ADDR, n) == n)
        self.assertEqual(missing, [], f"reachable unmangled: {missing}")


# merge_names guards a wider surface than the generated TU has headers for
# (ctype, time, wchar, MSVC internals). Only the ones a generated chunk can
# actually collide with are required to be in both.
_MATH_AND_STDLIB = frozenset("""
sqrt pow exp log log10 sin cos tan asin acos atan atan2 sinh cosh tanh
ceil floor fabs fmod frexp ldexp modf hypot nextafter copysign round trunc
rint cbrt log2 log1p expm1 asinh acosh atanh erf erfc lgamma tgamma fmin
fmax fma fpclassify isfinite isinf isnan isnormal signbit
setjmp longjmp
""".split())


if __name__ == "__main__":
    unittest.main()
