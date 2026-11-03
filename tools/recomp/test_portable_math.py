"""RECOMP_PORTABLE_MATH routes every x87 transcendental to the project's own.

A project that compares guest state across machines (netplay) defines it and
links recomp_sin and friends, so no host's libm reaches generated code. The
compiled run gives each one a sentinel and checks the lifted ops return it.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction
from .lifter import Lifter
from .translator import FP_STACK_MACROS

_ROOT = Path(__file__).resolve().parents[2]

_OPS = ("fsin", "fcos", "fsincos", "fptan", "fpatan", "fyl2x", "fyl2xp1", "f2xm1")

_SOURCE = r"""
#define RECOMP_GENERATED_CODE 1
#define RECOMP_PORTABLE_MATH 1
#include "recomp_types.h"
#include <stdio.h>
RECOMP_TLS uint32_t g_seh_ebp, g_eflags;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
@MACROS@
static unsigned used;
double recomp_sin(double x) { (void)x; used |= 1; return 0.25; }
double recomp_cos(double x) { (void)x; used |= 2; return 0.5; }
double recomp_tan(double x) { (void)x; used |= 4; return 0.75; }
double recomp_atan2(double y, double x) { (void)y; (void)x; used |= 8; return 1.25; }
double recomp_log2(double x) { (void)x; used |= 16; return 1.5; }
double recomp_log1p(double x) { (void)x; used |= 32; return 1.75; }
double recomp_expm1(double x) { (void)x; used |= 64; return 2.25; }
@OPS@
int main(void) {
@CALLS@
    printf("%u\n", used);
    return used == 127 ? 0 : 1;
}
"""


def _lift(mnemonic):
    insn = Instruction(0, 2, mnemonic, "", "")
    insn.operands = []
    return "\n".join(Lifter().lift_instruction(insn))


class PortableMathTest(unittest.TestCase):
    def test_every_transcendental_calls_the_projects_math(self):
        cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        ops = "\n".join(f"static void op_{m}(void) {{\n    {_lift(m)}\n}}" for m in _OPS)
        # Two operands of 0.5 and 2.0: in range, finite and positive for every op.
        calls = "\n".join(f"    g_fp_top = 0; fp_push(2.0); fp_push(0.5); op_{m}();" for m in _OPS)
        source = (_SOURCE.replace("@MACROS@", "\n".join(FP_STACK_MACROS))
                  .replace("@OPS@", ops).replace("@CALLS@", calls))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "portable.c"
            path.write_text(source)
            exe = Path(tmp) / "portable.exe"
            command = [cc, "-w", str(path), "-o", str(exe),
                       "-I", str(_ROOT / "templates" / "runtime"), "-I", str(_ROOT / "src")]
            if os.name != "nt":
                command.append("-lm")
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-2000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
