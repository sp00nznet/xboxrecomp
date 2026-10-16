"""pushfd after cmp, sub, add, and, or and xor pushes the flags the CPU would.

pushfd publishes CF/PF/ZF/SF/OF into g_eflags through publish_flags, which
reads each flag through the rule a jcc on it uses, and pushes that image. A
flag with no rule would keep a stale value: before the OF/PF rules for these
setters, signed 8-bit -128 - 1 (which wraps to 127 and sets OF) went out
of `cmp al, bl; pushfd` with OF clear.

The text tests pin the rules. The runtime test lifts `<op> dst, src; pushfd;
pop ecx` at 8, 16 and 32 bits, compiles it against the real
recomp_types.h at -O0 and -O2 (the SF rule once changed meaning with the
optimisation level, through signed overflow), and checks the whole pushed
word against a reference model of the SDM's flag definitions: 8 bits
exhaustively, 16 bits with one operand swept over all 65,536 values against
edge values of the other plus random pairs, 32 bits with edge pairs plus
random pairs. AF is not modelled (lahf and pushfd both read it as 0), so the
model clears it; the system half of the word comes from g_eflags.

The runtime half skips itself if no C compiler is on PATH.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNTIME = os.path.join(ROOT, "templates", "runtime")

REGS = {8: ("al", "bl"), 16: ("ax", "bx"), 32: ("eax", "ebx")}
OPS = ("cmp", "sub", "add", "and", "or", "xor")
# The model's operation per mnemonic: cmp is sub without the write.
KIND = {"cmp": 0, "sub": 0, "add": 1, "and": 2, "or": 3, "xor": 4}


def _insn(mnemonic, operands=(), addr=0):
    i = Instruction(addr, 1, mnemonic, "", "")
    i.operands = list(operands)
    return i


def _reg(name):
    return Operand(type="reg", reg=name)


def _lift(m, width):
    dst, src = REGS[width]
    lifter = Lifter()
    lifter.needs_cf = True
    insns = [_insn(m, [_reg(dst), _reg(src)]), _insn("pushfd"),
             _insn("pop", [_reg("ecx")])]
    out, _ = lift_basic_block(lifter, BasicBlock(start=0, instructions=insns))
    return lifter, "\n".join(out)


class TextTest(unittest.TestCase):
    def test_every_arithmetic_flag_is_rebuilt(self):
        for m in OPS:
            for width in REGS:
                lifter, out = _lift(m, width)
                with self.subTest(m=m, width=width):
                    self.assertNotIn("RECOMP_UNIMPL", out)
                    self.assertNotIn("pushfd (flags)", lifter.unimplemented)
                    self.assertIn("0x800u", out)    # OF
                    self.assertIn("0x4u", out)      # PF

    def test_jo_after_cmp_reads_the_snapshot(self):
        jo = _insn("jo", addr=4)
        jo.jump_target = 0x40
        lifter = Lifter()
        out, _ = lift_basic_block(lifter, BasicBlock(start=0, instructions=[
            _insn("cmp", [_reg("al"), _reg("bl")]), jo]))
        text = "\n".join(out)
        self.assertIn("uint8_t", text.split("_fbs")[-1])
        self.assertIn(">> 7", text.split("_fbs")[-1])


HARNESS = r"""
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eflags = 0x00000202u;
RECOMP_TLS int g_df = 0;
static unsigned long g_unimpl;
void recomp_unimpl(const char *text, uint32_t va)
{
    (void)text; (void)va;
    g_unimpl++;
}

static uint32_t eax, ebx, ecx, esp;
static uint8_t g_stack[64];

%(funcs)s

static int parity8(uint32_t r)
{
    int n = 0, i;
    for (i = 0; i < 8; i++)
        n += (r >> i) & 1;
    return !(n & 1);
}

/* The flags the SDM defines for a - b (cmp, sub), a + b (add) and the
 * logical ops (CF and OF cleared), AF aside. */
static uint32_t model(int kind, uint32_t a, uint32_t b, int width,
                      uint32_t *res)
{
    uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
    uint32_t top = 1u << (width - 1), r, f = 0;
    if (kind == 1) {
        r = (a + b) & mask;
        if (r < a) f |= 0x001u;
        if (~(a ^ b) & (a ^ r) & top) f |= 0x800u;
    } else if (kind == 0) {
        r = (a - b) & mask;
        if (a < b) f |= 0x001u;
        if ((a ^ b) & (a ^ r) & top) f |= 0x800u;
    } else {
        r = kind == 2 ? a & b : kind == 3 ? a | b : a ^ b;
    }
    if (parity8(r)) f |= 0x004u;
    if (r == 0) f |= 0x040u;
    if (r & top) f |= 0x080u;
    *res = r;
    return f | 0x202u;   /* IF and bit 1 from g_eflags' reset image */
}

static unsigned long g_cases, g_bad;

static void check(const char *op, int kind, int keeps,
                  void (*fn)(void), int width, uint32_t a, uint32_t b)
{
    uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
    uint32_t want_r, want = model(kind, a, b, width, &want_r);
    /* Junk above the operand width: the flags must ignore it. */
    uint32_t hi_a = 0xA5A5A5A5u & ~mask, hi_b = 0x5A5A5A5Au & ~mask;
    uint32_t want_eax = hi_a | (keeps ? a : want_r);
    eax = hi_a | a; ebx = hi_b | b; ecx = 0xCCCCCCCCu;
    esp = 64;
    fn();
    g_cases++;
    if (ecx != want || eax != want_eax || esp != 64) {
        if (g_bad++ < 8)
            printf("bad %%s%%d a=%%08X b=%%08X flags=%%08X want=%%08X"
                   " eax=%%08X want=%%08X\n", op, width, a, b, ecx, want,
                   eax, want_eax);
    }
}

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

static void run(const char *op, int kind, int keeps, int width,
                void (*fn)(void))
{
    static const uint32_t edge32[] = {
        0, 1, 2, 0x0F, 0x10, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
        0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu,
        0x12345678u, 0xDEADBEEFu};
    uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
    uint32_t top = 1u << (width - 1);
    uint32_t edge[24], a, b;
    unsigned long cases0 = g_cases, bad0 = g_bad;
    size_t n = 0, i, j;
    for (i = 0; i < sizeof edge32 / sizeof edge32[0]; i++)
        edge[n++] = edge32[i] & mask;
    edge[n++] = top - 1; edge[n++] = top; edge[n++] = top + 1;
    if (width == 8) {
        for (a = 0; a < 256; a++)
            for (b = 0; b < 256; b++)
                check(op, kind, keeps, fn, width, a, b);
        goto done;
    }
    if (width == 16) {
        for (a = 0; a < 0x10000; a++)
            for (i = 0; i < n; i++) {
                check(op, kind, keeps, fn, width, a, edge[i]);
                check(op, kind, keeps, fn, width, edge[i], a);
            }
    }
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            check(op, kind, keeps, fn, width, edge[i], edge[j]);
    for (i = 0; i < 200000; i++)
        check(op, kind, keeps, fn, width, rnd() & mask, rnd() & mask);
done:
    printf("%%s%%d cases %%lu bad %%lu\n", op, width, g_cases - cases0,
           g_bad - bad0);
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_stack;
%(runs)s
    printf("cases %%lu bad %%lu unimpl %%lu\n", g_cases, g_bad, g_unimpl);
    return 0;
}
"""


def _harness():
    funcs, runs = [], []
    for m in OPS:
        for width in REGS:
            _, body = _lift(m, width)
            name = f"lifted_{m}{width}"
            funcs.append(
                f"static void {name}(void)\n{{\n"
                "    uint32_t _fa = 0, _fb = 0; int32_t _fas = 0, _fbs = 0;"
                " int _cf = 0;\n"
                "    (void)_fa; (void)_fb; (void)_fas; (void)_fbs; (void)_cf;\n"
                f"{body}\n}}\n")
            runs.append(f'    run("{m}", {KIND[m]}, '
                        f'{int(m == "cmp")}, {width}, {name});')
    return HARNESS % {"funcs": "\n".join(funcs), "runs": "\n".join(runs)}


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(_cc(), "no C compiler")
class RuntimeTest(unittest.TestCase):
    def _run(self, opt):
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
            with open(c, "w", encoding="utf-8") as f:
                f.write(_harness())
            subprocess.run([_cc(), "-std=c11", opt, "-I", RUNTIME,
                            "-Wall", "-Wextra", "-Werror",
                            "-o", exe, c], check=True)
            return subprocess.run([exe], check=True, capture_output=True,
                                  text=True).stdout

    def test_flags_match_the_model(self):
        for opt in ("-O0", "-O2"):
            with self.subTest(opt=opt):
                out = self._run(opt)
                self.assertRegex(out.splitlines()[-1],
                                 r"^cases \d+ bad 0 unimpl 0$", out)


if __name__ == "__main__":
    unittest.main()
