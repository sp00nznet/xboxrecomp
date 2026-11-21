"""x87/SSE state save and restore, and FXTRACT.

FNINIT, FNSTENV, FLDENV, FXSAVE, FXRSTOR and FXTRACT fell to RECOMP_UNIMPL,
so a title that saved the FPU state around a routine stored nothing and later
restored whatever that memory held. They are lifted onto the state the model
tracks -- the control word, TOP, C0-C3, the eight registers, MXCSR and the
XMM file -- with the tag word derived from TOP (see recomp_types.h for what
is approximated).

Three levels here: the emitted text, a compiled run of the runtime helpers
against hand-worked images, and -- on an x86-64 host with a GNU-style
compiler -- the same lifted C run side by side with the real instructions,
compared on every field the model claims to keep. The 64-bit host stores the
same FXSAVE layout as the console's CPU except for the instruction and
operand pointers (and MXCSR_MASK), and those are exactly the fields the model
does not keep, so they are skipped.
"""
import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FP_STACK_MACROS

_ROOT = Path(__file__).resolve().parents[2]


def _insn(mnemonic, op_str="", operands=(), raw=""):
    instruction = Instruction(0, max(len(raw) // 2, 2), mnemonic, op_str, raw)
    instruction.operands = list(operands)
    return instruction


def _mem(base="eax", disp=0, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


def _lift(mnemonic, op_str="", operands=(), raw=""):
    return "\n".join(Lifter().lift_instruction(_insn(mnemonic, op_str, operands, raw)))


class FpEnvLiftTest(unittest.TestCase):
    def test_fninit_resets_through_the_runtime(self):
        self.assertEqual(_lift("fninit"), "recomp_fninit(); /* fninit */")
        self.assertEqual(_lift("finit"), "recomp_fninit(); /* finit */")

    def test_environment_forms_pick_the_layout_from_the_66_prefix(self):
        # Capstone gives the environment operand 28 bytes with or without
        # the prefix, so mem_size cannot choose the layout.
        for mnemonic, raw, helper, short in (
                ("fnstenv", "d930", "recomp_fnstenv", 0),
                ("fnstenv", "66d930", "recomp_fnstenv", 1),
                ("fstenv", "d930", "recomp_fnstenv", 0),
                ("fldenv", "d920", "recomp_fldenv", 0),
                ("fldenv", "66d920", "recomp_fldenv", 1),
                ("fnsave", "dd30", "recomp_fnsave", 0),
                ("fnsave", "66dd30", "recomp_fnsave", 1),
                ("fsave", "dd30", "recomp_fnsave", 0),
                ("frstor", "66dd20", "recomp_frstor", 1)):
            with self.subTest(mnemonic=mnemonic, raw=raw):
                out = _lift(mnemonic, "[eax]", [_mem(size=28)], raw)
                self.assertTrue(out.startswith(f"{helper}(eax, {short});"), out)

    def test_a_disp8_of_0x66_is_not_a_prefix(self):
        # fnstenv [eax+0x66]: the 0x66 follows the opcode.
        out = _lift("fnstenv", "[eax + 0x66]", [_mem(disp=0x66, size=28)], "d97066")
        self.assertTrue(out.startswith("recomp_fnstenv(eax + 0x66, 0);"), out)

    def test_fxsave_and_fxrstor(self):
        self.assertTrue(_lift("fxsave", "[esp]", [_mem("esp")], "0fae0424")
                        .startswith("recomp_fxsave(esp);"))
        self.assertTrue(_lift("fxrstor", "[eax + 0x10]", [_mem(disp=16)], "0fae4810")
                        .startswith("recomp_fxrstor(eax + 0x10);"))

    def test_fxtract_replaces_st0_and_pushes_the_significand(self):
        out = _lift("fxtract")
        self.assertIn("fp_top() = recomp_fxtract(fp_top(), &_sig); fp_push(_sig);", out)
        self.assertIn("g_fp_cc & ~0x0200u", out)

    def test_none_is_unimplemented(self):
        for args in (("fninit",), ("fxtract",),
                     ("fnstenv", "[eax]", [_mem(size=28)], "d930"),
                     ("fldenv", "[eax]", [_mem(size=28)], "d920"),
                     ("fxsave", "[eax]", [_mem()], "0fae00"),
                     ("fxrstor", "[eax]", [_mem()], "0fae08")):
            with self.subTest(mnemonic=args[0]):
                self.assertNotIn("RECOMP_UNIMPL", _lift(*args))

    def test_register_operands_stay_unimplemented(self):
        # No register form exists; a decoder that produced one gets flagged,
        # not silently given a bogus address.
        self.assertIn("RECOMP_UNIMPL",
                      _lift("fxsave", "eax", [Operand(type="reg", reg="eax")]))

    def test_a_compare_survives_them(self):
        # None of these writes EFLAGS, so a jcc after them still reads the
        # cmp before them.
        insns = [_insn("cmp", "eax, ecx", [Operand(type="reg", reg="eax"),
                                           Operand(type="reg", reg="ecx")]),
                 _insn("fxsave", "[esp]", [_mem("esp")], "0fae0424"),
                 _insn("fnstenv", "[esp]", [_mem("esp", size=28)], "d93424"),
                 _insn("fninit"),
                 _insn("fxtract"),
                 _insn("jb", "0x20", [Operand(type="imm", imm=0x20)])]
        for i, insn in enumerate(insns):
            insn.address = 2 * i
        insns[-1].jump_target = 0x20
        stmts, setter = lift_basic_block(Lifter(), BasicBlock(start=0, instructions=insns))
        self.assertEqual(setter[0], "cmp")
        self.assertIn("CMP_B(_fa, _fb)", "\n".join(stmts))

    def test_translator_reaches_the_helpers_from_real_bytes(self):
        from . import fpdiff
        code = bytes.fromhex(
            "dbe3"          # fninit
            "d930"          # fnstenv [eax]
            "66d920"        # fldenv [eax] (14-byte)
            "0fae00"        # fxsave [eax]
            "0fae08"        # fxrstor [eax]
            "d9e8"          # fld1
            "d9f4"          # fxtract
            "ddd8ddd8")     # fstp st(0) x2
        text = fpdiff.translate(code, "fpenv")
        self.assertNotIn("RECOMP_UNIMPL", text)
        for call in ("recomp_fninit();", "recomp_fnstenv(eax, 0);",
                     "recomp_fldenv(eax, 1);", "recomp_fxsave(eax);",
                     "recomp_fxrstor(eax);", "recomp_fxtract(fp_top(), &_sig)"):
            self.assertIn(call, text)


def _c_compiler():
    return next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)


def _build_and_run(test, source, name):
    cc = _c_compiler()
    if not cc:
        test.skipTest("no C compiler available")
    with tempfile.TemporaryDirectory(prefix=f"{name}-") as tmp:
        tmp = Path(tmp)
        src, exe = tmp / f"{name}.c", tmp / name
        src.write_text(source, encoding="utf-8")
        built = subprocess.run(
            [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-I", str(_ROOT / "templates" / "runtime"), str(src), "-o", str(exe)]
            + ([] if os.name == "nt" else ["-lm"]),
            capture_output=True, text=True)
        test.assertEqual(0, built.returncode, built.stdout + built.stderr)
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        test.assertEqual(0, ran.returncode, f"check on line {ran.returncode} failed\n"
                         + ran.stdout + ran.stderr)


def _lifted_function(name, *args):
    body = Lifter().lift_instruction(_insn(*args))
    return f"static void {name}(void) {{\n    " + "\n    ".join(body) + "\n}\n"


# Globals the helpers touch, the x87 accessors the lifted text is written
# against, and one lifted function per instruction under test. eax is the
# base register of every memory operand; guest address = offset in g_guest.
_PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"

RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
RECOMP_TLS uint32_t g_mxcsr = 0x1F80u;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
ptrdiff_t g_xbox_mem_offset;
static uint32_t eax;
#if defined(_MSC_VER)
static __declspec(align(16)) uint8_t g_guest[0x1000];
#else
static uint8_t g_guest[0x1000] __attribute__((aligned(16)));
#endif
''' + "\n".join(FP_STACK_MACROS) + "\n"


def _lifted_set():
    mem28, mem4 = _mem(size=28), _mem()
    return "".join((
        _lifted_function("l_fninit", "fninit"),
        _lifted_function("l_ftst", "ftst"),
        _lifted_function("l_fxtract", "fxtract"),
        _lifted_function("l_fnstenv", "fnstenv", "[eax]", [mem28], "d930"),
        _lifted_function("l_fnstenv16", "fnstenv", "[eax]", [mem28], "66d930"),
        _lifted_function("l_fldenv", "fldenv", "[eax]", [mem28], "d920"),
        _lifted_function("l_fldenv16", "fldenv", "[eax]", [mem28], "66d920"),
        _lifted_function("l_fnsave", "fnsave", "dword ptr [eax]", [mem4], "dd30"),
        _lifted_function("l_frstor", "frstor", "dword ptr [eax]", [mem4], "dd20"),
        _lifted_function("l_fxsave", "fxsave", "[eax]", [mem4], "0fae00"),
        _lifted_function("l_fxrstor", "fxrstor", "[eax]", [mem4], "0fae08"),
    ))


_MODEL_CHECKS = r'''
#define G(a) (g_guest + (a))
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }

int main(void) {
    unsigned i;
    double one = 1.0, sig, ex;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest;

    /* FNINIT then FNSTENV: the documented reset image, reserved halves set. */
    g_fp_control_word = 0x0C7Fu; g_fp_top = 5; g_fp_cc = 0x4500u;
    l_fninit();
    if (g_fp_control_word != 0x037F || g_fp_top != 0 || g_fp_cc != 0) return __LINE__;
    memset(g_guest, 0xAA, sizeof g_guest);
    eax = 0; l_fnstenv();
    if (rd32(0) != 0xFFFF037Fu || rd32(4) != 0xFFFF0000u || rd32(8) != 0xFFFFFFFFu) return __LINE__;
    if (rd32(12) || rd32(16) || rd32(20) || rd32(24) != 0xFFFF0000u) return __LINE__;
    if (g_guest[28] != 0xAA) return __LINE__;          /* 28 bytes, no more */

    /* Two values: physical 7 = 1.5 (valid), 6 = -0.0 (zero); after a compare. */
    fp_push(1.5); fp_push(-0.0); l_ftst();
    g_fp_control_word = 0x0360u;                        /* exceptions unmasked */
    eax = 0x40; l_fnstenv16();
    if (rd16(0x40) != 0x0360 || rd16(0x42) != (0x3000 | 0x4000)) return __LINE__;
    if (rd16(0x44) != 0x1FFF || g_guest[0x40 + 14] != 0xAA) return __LINE__;
    if (g_fp_control_word != 0x037F) return __LINE__;   /* FNSTENV masks */

    /* Specials and the empty/valid split around TOP. */
    fp_push((double)INFINITY); fp_push((double)NAN); fp_push(0x1p-1074);
    eax = 0; l_fnstenv();
    /* physical 7 valid, 6 zero, 5 special, 4 special, 3 valid (subnormal
     * double, normal in extended), 0-2 empty */
    if (rd16(8) != (0x003F | (0u << 6) | (2u << 8) | (2u << 10) | (1u << 12) | (0u << 14)))
        return __LINE__;

    /* FLDENV takes CW, TOP and C0-C3 and leaves the registers alone. */
    memcpy(G(0x80), G(0), 28);
    memcpy(G(0x84), &(uint16_t){ (uint16_t)(0x3000 | 0x4100 | 0x00FF) }, 2);
    l_fninit(); eax = 0x80; l_fldenv();
    if (g_fp_top != 6 || g_fp_cc != 0x4100 || g_fp_cmp != 0) return __LINE__;
    if (fp_top() != 0.0 || !signbit(fp_top()) || fp_st1() != 1.5) return __LINE__;
    if (g_fp_control_word != 0x037F) return __LINE__;
    memset(G(0x90), 0, 14);
    memcpy(G(0x90), &(uint16_t){ 0x0A7Fu }, 2);
    /* C0 (less), plus an exception flag the model drops */
    memcpy(G(0x92), &(uint16_t){ 0x3800u | 0x0100u | 0x0001u }, 2);
    eax = 0x90; l_fldenv16();
    if (g_fp_control_word != 0x0A7F || g_fp_top != 7 || g_fp_cc != 0x0100
        || g_fp_cmp != -1) return __LINE__;

    /* FNSAVE/FRSTOR round trip through the shared environment code. */
    l_fninit(); fp_push(2.0); fp_push(-3.25); g_fp_control_word = 0x027F;
    memset(g_guest, 0xAA, sizeof g_guest);
    eax = 0x100; l_fnsave();
    if (g_fp_control_word != 0x037F || g_fp_top != 0) return __LINE__;
    if (rd16(0x108) != 0x0FFF || g_guest[0x100 + 108] != 0xAA) return __LINE__;
    g_fp_stack[6] = g_fp_stack[7] = 99.0;
    eax = 0x100; l_frstor();
    if (g_fp_control_word != 0x027F || g_fp_top != 6 || fp_top() != -3.25
        || fp_st1() != 2.0) return __LINE__;

    /* FXSAVE: header, abridged tags, MXCSR, registers in stack order, XMM,
     * and nothing written at or past byte 288. */
    for (i = 0; i < 4; ++i) {
        g_xmm0.u[i] = 0x10101010u * i; g_xmm7.u[i] = 0xC0DE0000u + i;
    }
    g_mxcsr = 0x3F81u;
    memset(g_guest, 0xAA, sizeof g_guest);
    eax = 0x200; l_fxsave();
    if (rd16(0x200) != 0x027F || rd16(0x202) != 0x3000 || g_guest[0x204] != 0xC0
        || g_guest[0x205] != 0) return __LINE__;
    for (i = 6; i < 24; ++i) if (g_guest[0x200 + i]) return __LINE__;
    if (rd32(0x218) != 0x3F81u || rd32(0x21C) != 0) return __LINE__;
    if (rd32(0x220) != 0 || rd32(0x224) != 0xD0000000u || rd16(0x228) != 0xC000) return __LINE__;
    for (i = 0x22A; i < 0x230; ++i) if (g_guest[i]) return __LINE__;
    if (rd32(0x230 + 4) != 0x80000000u || rd16(0x230 + 8) != 0x4000) return __LINE__;
    if (rd32(0x2A4) != 0x10101010u || rd32(0x31C) != 0xC0DE0003u) return __LINE__;
    for (i = 0x200 + 288; i < 0x400; ++i) if (g_guest[i] != 0xAA) return __LINE__;
    if (g_fp_top != 6 || g_fp_control_word != 0x027F) return __LINE__;  /* no reset */

    /* FXRSTOR brings all of it back. */
    l_fninit(); g_mxcsr = 0x1F80u; memset(&g_xmm0, 0, 16); memset(&g_xmm7, 0, 16);
    eax = 0x200; l_fxrstor();
    if (g_fp_top != 6 || g_fp_control_word != 0x027F || g_mxcsr != 0x3F81u) return __LINE__;
    if (fp_top() != -3.25 || fp_st1() != 2.0) return __LINE__;
    if (g_xmm0.u[3] != 0x30303030u || g_xmm7.u[2] != 0xC0DE0002u) return __LINE__;

    /* FXTRACT edge cases (the C1 bit is cleared). */
    l_fninit(); fp_push(-12.0); g_fp_cc = 0x0200; l_fxtract();
    if (g_fp_top != 6 || fp_top() != -1.5 || fp_st1() != 3.0 || g_fp_cc) return __LINE__;
    l_fninit(); fp_push(-0.0); l_fxtract();
    if (fp_top() != 0.0 || !signbit(fp_top()) || fp_st1() != -(double)INFINITY) return __LINE__;
    l_fninit(); fp_push(-(double)INFINITY); l_fxtract();
    if (fp_top() != -(double)INFINITY || fp_st1() != (double)INFINITY) return __LINE__;
    l_fninit(); fp_push((double)NAN); l_fxtract();
    if (!isnan(fp_top()) || !isnan(fp_st1())) return __LINE__;
    l_fninit(); fp_push(0x1p-1074); l_fxtract();
    if (fp_top() != 1.0 || fp_st1() != -1074.0) return __LINE__;
    ex = recomp_fxtract(one, &sig);
    if (ex != 0.0 || sig != 1.0) return __LINE__;
    return 0;
}
'''


class FpEnvRuntimeTest(unittest.TestCase):
    def test_helpers_build_the_documented_images(self):
        _build_and_run(self, _PRELUDE + _lifted_set() + _MODEL_CHECKS, "fpenv-model")


# The same state built twice: once by the real instructions, once by the
# lifted C, then compared. Restores are checked both ways round: the model
# loading the CPU's images, and the CPU loading the model's.
_NATIVE = r'''
#define A_ENV   0x000u
#define A_ENV16 0x040u
#define A_SV    0x080u
#define A_ENV2  0x100u
#define A_FX    0x200u
#define A_FX2   0x400u
#define A_TMP   0x600u
#define A_SV2   0x700u

typedef struct {
    uint8_t fx[512], fx3[512];
    uint8_t env[28], env16[14], sv[108], env2[28], env3[28];
    uint16_t cw_after;
} Native;
#if defined(_MSC_VER)
static __declspec(align(16)) Native nat;
#else
static Native nat __attribute__((aligned(16)));
#endif

static void native_capture(const double *vals, long n, long test, uint16_t cw,
                           uint32_t mxcsr, const uint8_t *xmm) {
    uint32_t saved;
    __asm__ __volatile__(
        "stmxcsr %[saved]\n\t"
        "movdqu 0(%[xmm]), %%xmm0\n\t"   "movdqu 16(%[xmm]), %%xmm1\n\t"
        "movdqu 32(%[xmm]), %%xmm2\n\t"  "movdqu 48(%[xmm]), %%xmm3\n\t"
        "movdqu 64(%[xmm]), %%xmm4\n\t"  "movdqu 80(%[xmm]), %%xmm5\n\t"
        "movdqu 96(%[xmm]), %%xmm6\n\t"  "movdqu 112(%[xmm]), %%xmm7\n\t"
        "fninit\n\t"
        "fldcw %[cw]\n\t"
        "test %[n], %[n]\n\t"
        "jz 2f\n"
        "1:\n\t"
        "fldl (%[vals])\n\t"
        "add $8, %[vals]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n"
        "2:\n\t"
        "test %[test], %[test]\n\t"
        "jz 3f\n\t"
        "ftst\n"
        "3:\n\t"
        "fnstenv %[env]\n\t"
        "fnstcw %[cwa]\n\t"
        "fldcw %[cw]\n\t"
        ".byte 0x66\n\t"
        "fnstenv %[env16]\n\t"
        "fldcw %[cw]\n\t"
        "ldmxcsr %[mx]\n\t"
        "fxsave %[fx]\n\t"
        "ldmxcsr %[saved]\n\t"
        "fnsave %[sv]\n\t"
        "fnstenv %[env2]\n\t"
        "fninit\n\t"
        : [vals] "+r"(vals), [n] "+r"(n), [saved] "=m"(saved),
          [env] "=m"(nat.env), [env16] "=m"(nat.env16), [fx] "=m"(nat.fx),
          [sv] "=m"(nat.sv), [env2] "=m"(nat.env2), [cwa] "=m"(nat.cw_after)
        : [cw] "m"(cw), [mx] "m"(mxcsr), [xmm] "r"(xmm), [test] "r"(test)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "cc", "memory");
}

/* The CPU loads an image the model wrote, and stores it back. In 64-bit
 * mode FXRSTOR also loads XMM8-15 from bytes 288-415, which the model leaves
 * alone, so those are clobbered too. FXSAVE goes first: FNSTENV masks the
 * exceptions. */
static void native_reload(const uint8_t *image) {
    uint32_t saved;
    __asm__ __volatile__(
        "stmxcsr %[saved]\n\t"
        "fxrstor (%[in])\n\t"
        "fxsave %[fx3]\n\t"
        "fnstenv %[env3]\n\t"
        "fninit\n\t"
        "ldmxcsr %[saved]\n\t"
        : [saved] "=m"(saved), [env3] "=m"(nat.env3), [fx3] "=m"(nat.fx3)
        : [in] "r"(image)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "cc", "memory");
}

static void native_fxtract(double x, double *ex, double *sig, uint16_t *sw) {
    __asm__ __volatile__(
        "fninit\n\t"
        "fldl %[x]\n\t"
        "fxtract\n\t"
        "fnstsw %[sw]\n\t"
        "fstpl %[sig]\n\t"
        "fstpl %[ex]\n\t"
        "fninit\n\t"
        : [ex] "=m"(*ex), [sig] "=m"(*sig), [sw] "=m"(*sw)
        : [x] "m"(x)
        : "cc", "memory");
}

static void model_capture(const double *vals, long n, long test, uint16_t cw,
                          uint32_t mxcsr, const uint8_t *xmm) {
    long i;
    l_fninit();
    g_fp_control_word = cw;
    for (i = 0; i < n; ++i) fp_push(vals[i]);
    if (test) l_ftst();
    eax = A_ENV; l_fnstenv();
    nat.cw_after ^= g_fp_control_word;     /* zero when they agree */
    g_fp_control_word = cw;
    eax = A_ENV16; l_fnstenv16();
    g_fp_control_word = cw;
    memcpy(&g_xmm0, xmm, 16);       memcpy(&g_xmm1, xmm + 16, 16);
    memcpy(&g_xmm2, xmm + 32, 16);  memcpy(&g_xmm3, xmm + 48, 16);
    memcpy(&g_xmm4, xmm + 64, 16);  memcpy(&g_xmm5, xmm + 80, 16);
    memcpy(&g_xmm6, xmm + 96, 16);  memcpy(&g_xmm7, xmm + 112, 16);
    g_mxcsr = mxcsr;
    eax = A_FX; l_fxsave();
    eax = A_SV; l_fnsave();
    eax = A_ENV2; l_fnstenv();
}

/* Compare the modelled fields. The status word's low byte holds exception
 * flags the model does not record (the CPU raises IE on a NaN compare and DE
 * on a subnormal load), so it is not compared; neither are the pointer
 * fields, nor MXCSR_MASK (the model reports a P3's, the host its own). */
static int same(const char *what, int vec, const uint8_t *native, const uint8_t *model,
                unsigned len, unsigned status_at, const unsigned (*skip)[2], int nskip) {
    unsigned i; int k;
    for (i = 0; i < len; ++i) {
        int skipped = i == status_at;
        for (k = 0; k < nskip; ++k)
            if (i >= skip[k][0] && i < skip[k][1]) skipped = 1;
        if (!skipped && native[i] != model[i]) {
            printf("%s vec %d: byte %u native=%02X model=%02X\n", what, vec, i,
                   native[i], model[i]);
            return 0;
        }
    }
    return 1;
}

static const unsigned ENV32_SKIP[][2] = { { 12, 26 } };   /* FIP, FCS/FOP, FDP, FDS */
static const unsigned ENV16_SKIP[][2] = { { 6, 14 } };
static const unsigned SAVE_SKIP[][2] = { { 12, 26 } };
static const unsigned FX_SKIP[][2] = { { 6, 24 }, { 28, 32 }, { 288, 512 } };

static int check_vector(int vec, const double *vals, long n, long test, uint16_t cw,
                        uint32_t mxcsr) {
    uint8_t xmm[128];
    int i, ok = 1;
    for (i = 0; i < 128; ++i) xmm[i] = (uint8_t)(i * 37 + vec * 11 + 5);

    native_capture(vals, n, test, cw, mxcsr, xmm);
    memset(g_guest, 0xAA, sizeof g_guest);
    model_capture(vals, n, test, cw, mxcsr, xmm);
    if (nat.cw_after) { printf("cw after fnstenv vec %d\n", vec); ok = 0; }
    ok &= same("fnstenv", vec, nat.env, G(A_ENV), 28, 4, ENV32_SKIP, 1);
    ok &= same("fnstenv16", vec, nat.env16, G(A_ENV16), 14, 2, ENV16_SKIP, 1);
    ok &= same("fxsave", vec, nat.fx, G(A_FX), 512, 2, FX_SKIP, 3);
    ok &= same("fnsave", vec, nat.sv, G(A_SV), 108, 4, SAVE_SKIP, 1);
    ok &= same("fnstenv after fnsave", vec, nat.env2, G(A_ENV2), 28, 4, ENV32_SKIP, 1);

    /* The model loads the CPU's images and stores them back unchanged. */
    memcpy(G(A_TMP), nat.env, 28);
    eax = A_TMP; l_fldenv();
    eax = A_TMP + 0x40; l_fnstenv();
    ok &= same("fldenv(cpu)", vec, nat.env, G(A_TMP + 0x40), 28, 4, ENV32_SKIP, 1);
    memcpy(G(A_TMP), nat.env16, 14);
    eax = A_TMP; l_fldenv16();
    eax = A_TMP + 0x40; l_fnstenv16();
    ok &= same("fldenv16(cpu)", vec, nat.env16, G(A_TMP + 0x40), 14, 2, ENV16_SKIP, 1);
    memcpy(G(A_TMP), nat.sv, 108);
    eax = A_TMP; l_frstor();
    eax = A_SV2; l_fnsave();
    ok &= same("frstor(cpu)", vec, nat.sv, G(A_SV2), 108, 4, SAVE_SKIP, 1);
    memcpy(G(A_FX2), nat.fx, 512);
    eax = A_FX2; l_fxrstor();
    if (g_mxcsr != mxcsr || memcmp(&g_xmm3, xmm + 48, 16)) {
        printf("fxrstor(cpu) vec %d: mxcsr/xmm\n", vec); ok = 0;
    }
    memset(G(A_TMP), 0xAA, 512);
    eax = A_TMP; l_fxsave();
    ok &= same("fxrstor(cpu)", vec, nat.fx, G(A_TMP), 512, 2, FX_SKIP, 3);

    /* The CPU loads the model's FXSAVE image: its own FNSTENV then derives
     * the full tag word from the abridged one and the register values. */
    native_reload(G(A_FX));
    ok &= same("cpu fxrstor(model) fnstenv", vec, nat.env3, nat.env, 28, 4, ENV32_SKIP, 1);
    ok &= same("cpu fxrstor(model) fxsave", vec, nat.fx3, G(A_FX), 512, 2, FX_SKIP, 3);
    return ok;
}

static int check_fxtract(double x) {
    double ex, sig;
    uint16_t sw;
    native_fxtract(x, &ex, &sig, &sw);
    l_fninit(); fp_push(x); g_fp_cc = 0x0200; l_fxtract();
    if (g_fp_top != 6) return 0;
    if (!((isnan(ex) && isnan(fp_st1())) || !memcmp(&ex, &fp_st1(), 8))
        || !((isnan(sig) && isnan(fp_top())) || !memcmp(&sig, &fp_top(), 8))) {
        printf("fxtract(%a): native %a, %a model %a, %a\n", x, ex, sig,
               fp_st1(), fp_top());
        return 0;
    }
    if ((sw & 0x0200) != (g_fp_cc & 0x0200)) { printf("fxtract(%a) C1\n", x); return 0; }
    /* The CPU flags divide-by-zero for a zero; the model records no flags. */
    if ((x == 0.0) != ((sw & 0x0004) != 0)) { printf("fxtract(%a) ZE\n", x); return 0; }
    return 1;
}

int main(void) {
    static const double sets[][7] = {
        { 0 },
        { 1.5 },
        { 1.5, -0.0 },
        { (double)INFINITY, -(double)INFINITY, (double)NAN, 3.0 },
        { 0x1p-1074, -1e-310, 1e300, -2.5, 0.0, 1.0, 7.0 },
        { -0.1, 0x1.fffffffffffffp+1023, -(double)NAN, 4.0, 5.0, 6.0 },
    };
    static const long counts[] = { 0, 1, 2, 4, 7, 6 };
    static const uint16_t cws[] = { 0x037F, 0x027F, 0x0C7F, 0x007F, 0x0360 };
    static const uint32_t mxcsrs[] = { 0x1F80u, 0x3F81u, 0x9F80u, 0x5FA5u };
    static const double fx[] = {
        0.0, -0.0, (double)INFINITY, -(double)INFINITY, (double)NAN, -(double)NAN,
        1.0, -1.0, 1.5, 3.0, -12.0, 0.1, 6.02e23, 1e300, -1e-300, 0x1p-1074,
        -0x1.fffffffffffffp-1023, 0x1p1023, 0x1.fffffffffffffp+1023, 1e-310 };
    int vec = 0, failed = 0;
    unsigned s, c, m, i;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest;
    for (s = 0; s < sizeof counts / sizeof counts[0]; ++s)
        for (c = 0; c < sizeof cws / sizeof cws[0]; ++c)
            for (m = 0; m < sizeof mxcsrs / sizeof mxcsrs[0]; ++m) {
                long test;
                /* Unmasked exceptions only with operands that raise none. */
                if ((cws[c] & 0x3F) != 0x3F && s != 1 && s != 2) continue;
                for (test = 0; test < 2; ++test) {
                    if (test && !counts[s]) continue;
                    failed += !check_vector(vec++, sets[s], counts[s], test, cws[c], mxcsrs[m]);
                }
            }
    for (i = 0; i < sizeof fx / sizeof fx[0]; ++i) failed += !check_fxtract(fx[i]);
    printf("%d vectors, %d failed\n", vec, failed);
    return failed ? 1 : 0;
}
'''


class FpEnvNativeTest(unittest.TestCase):
    def test_lifted_state_images_match_the_cpu(self):
        if platform.machine().lower() not in ("x86_64", "amd64"):
            self.skipTest("needs an x86-64 host")
        cc = _c_compiler()
        if cc and Path(cc).stem.lower() == "cl":
            self.skipTest("the native side is GNU inline assembly")
        source = _PRELUDE + _lifted_set() + "#define G(a) (g_guest + (a))\n" + _NATIVE
        _build_and_run(self, source, "fpenv-native")


if __name__ == "__main__":
    unittest.main()
