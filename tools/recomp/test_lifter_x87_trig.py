"""x87 fsin/fcos/fsincos/fptan lift through the 66-bit-pi helpers.

The x87 reduces FSIN/FCOS/FSINCOS/FPTAN operands with a 66-bit pi, so above
pi/4 it returns f(x * pi / pi66), not libm's f(x). The lifts keep
recomp_fp_trig_in_range as the gate (C2 and an untouched st0 for
|x| >= 2^63) and call the recomp_x87_* value helpers inside it;
tests/x87_trig checks the helpers' values against an mpmath model. Here:
the emitted calls, and a compiled run of the lifted CRT idiom
`fsin; fnstsw ax; sahf; jp reduce` plus fsincos/fptan stack depth.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FP_STACK_MACROS

_ROOT = Path(__file__).resolve().parents[2]


def _insn(address, size, mnemonic, op_str="", operands=(), **attrs):
    instruction = Instruction(address, size, mnemonic, op_str, "")
    instruction.operands = list(operands)
    for key, value in attrs.items():
        setattr(instruction, key, value)
    return instruction


def _lift(mnemonic):
    return "\n".join(Lifter().lift_instruction(_insn(0, 2, mnemonic)))


def _block(instructions):
    stmts = lift_basic_block(Lifter(), BasicBlock(start=0, instructions=instructions))[0]
    return "\n".join("    " + s for s in stmts)


class TrigEmitTest(unittest.TestCase):
    def test_each_op_calls_its_helper_inside_the_range_check(self):
        for mnemonic, helper in (("fsin", "recomp_x87_sin(fp_top())"),
                                 ("fcos", "recomp_x87_cos(fp_top())"),
                                 ("fsincos", "recomp_x87_sincos(fp_top(), &_c)"),
                                 ("fptan", "recomp_x87_tan(_a)")):
            with self.subTest(mnemonic=mnemonic):
                out = _lift(mnemonic)
                self.assertTrue(out.startswith("if (recomp_fp_trig_in_range(fp_top()))"), out)
                self.assertIn(helper, out)
                self.assertNotIn("= sin(", out)
                self.assertNotIn("= tan(", out)
                self.assertNotIn("RECOMP_FP_PC", out)

    def test_fsincos_and_fptan_push_only_in_range(self):
        self.assertIn("fp_push(_c); }", _lift("fsincos"))
        self.assertIn("fp_push(isfinite(_a) ? 1.0 : _a - _a); }", _lift("fptan"))


# The jp leaves the block, so it lifts to a call of sub_00000040, which
# records that it was taken.
_SOURCE = r"""
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <stdio.h>
RECOMP_TLS uint32_t g_seh_ebp, g_eflags;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
@MACROS@
static int taken;
static void sub_00000040(void) { taken = 1; }
static void crt_sin(void) {
    uint32_t eax = 0, ebp = 0; uint32_t _fa = 0, _fb = 0; (void)ebp; (void)_fa; (void)_fb;
@SIN@
}
static void op_sincos(void) {
@SINCOS@
}
static void op_tan(void) {
@TAN@
}
static int run_sin(double x) {
    g_fp_top = 0; fp_push(x); g_fp_cc = 0; taken = 0;
    crt_sin();
    return taken;
}
int main(void) {
    /* Out of range: C2 set, st0 kept, jp taken to the CRT's fprem1 loop. */
    if (!run_sin(18446744073709551616.0)) return 10;
    if (fp_top() != 18446744073709551616.0 || g_fp_top != 7) return 11;
    /* 1e10: the x87's sine (model.py), not libm's -0.4875060250875107. */
    if (run_sin(1e10)) return 12;
    if (fabs(fp_top() - -0.48750602507627) > 1e-15) return 13;
    /* pi: Dawson's measured fsin(pi). */
    run_sin(3.141592653589793);
    if (fabs(fp_top() - 1.2246063538223773e-16) > 1e-30) return 14;
    /* Below pi/4: libm's value. */
    run_sin(0.5);
    if (fp_top() != sin(0.5)) return 15;
    /* fsincos: depth + 1 in range (fp_push counts down from 7), unchanged out
     * of range. */
    g_fp_top = 0; fp_push(1e10); op_sincos();
    if (g_fp_top != 6 || fabs(fp_top() - 0.8731196226831323) > 1e-15
        || fabs(fp_st1() - -0.48750602507627) > 1e-15) return 16;
    g_fp_top = 0; fp_push(1e300); op_sincos();
    if (g_fp_top != 7 || fp_top() != 1e300 || !(g_fp_cc & 0x0400u)) return 17;
    /* fptan: tan then 1.0; measured tan(1e10) on hardware. */
    g_fp_top = 0; fp_push(1e10); op_tan();
    if (g_fp_top != 6 || fp_top() != 1.0
        || fabs(fp_st1() - -0.55834963779435409) > 1e-15) return 18;
    puts("OK");
    return 0;
}
"""


def _sources():
    ax = Operand(type="reg", reg="ax")
    sin = _block([
        _insn(0, 2, "fsin"),
        _insn(2, 2, "fnstsw", "ax", [ax]),
        _insn(4, 1, "sahf"),
        _insn(5, 2, "jp", "0x40", [Operand(type="imm", imm=0x40)], jump_target=0x40),
    ])
    return sin, "    " + _lift("fsincos"), "    " + _lift("fptan")


def _compile_and_run(sin, sincos, tan):
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("C compiler unavailable")
    source = (_SOURCE.replace("@MACROS@", "\n".join(FP_STACK_MACROS))
              .replace("@SIN@", sin).replace("@SINCOS@", sincos)
              .replace("@TAN@", tan))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "trig.c"
        path.write_text(source)
        exe = Path(tmp) / "trig.exe"
        command = [cc, "-w", str(path), "-o", str(exe),
                   "-I", str(_ROOT / "templates" / "runtime"), "-I", str(_ROOT / "src")]
        if os.name != "nt":
            command.append("-lm")
        built = subprocess.run(command, capture_output=True, text=True)
        assert built.returncode == 0, built.stderr[-2000:]
        return subprocess.run([str(exe)], capture_output=True, text=True)


class TrigCompiledTest(unittest.TestCase):
    def test_lifted_trig_matches_the_x87(self):
        result = _compile_and_run(*_sources())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_negative_control_plain_libm_fails(self):
        """The previous lifting (libm inside the range check) must fail it."""
        sin, sincos, tan = _sources()
        sin = sin.replace("recomp_x87_sin(", "sin(")
        result = _compile_and_run(sin, sincos, tan)
        self.assertEqual(result.returncode, 13, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
