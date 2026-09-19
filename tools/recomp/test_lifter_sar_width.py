"""Self-check for `sar` at 8 and 16 bits, and for its shift count.

`sar` used to lift to "(uint32_t)((int32_t)dst >> cnt)" at every width. Narrow
reads arrive zero-extended -- LO8/HI8/LO16 mask, MEM8/MEM16 are unsigned -- so
at 8 and 16 bits that int32_t cast never saw a sign bit and the ">>" was a
logical shift wearing an arithmetic cast. `sar al, 1` with al = 0x80 produced
0x40 where x86 produces 0xC0: a negative number halved into a positive one,
the shape of a fixed-point divide or a signed average going wrong without
faulting.

The count was also unmasked. x86 masks it to 5 bits for 8-, 16- and 32-bit
operands; in C a shift of 33 on a 32-bit type is undefined.

These checks compile the lifter's own output and sweep it against a reference
that computes the x86 answer with a signed type of the right width. Each one
is paired with a negative control that feeds the same harness the pre-fix
expression and requires the sweep to reject it -- a sweep that passes against
both spellings would be testing nothing.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter, _fmt_operand_read, _fmt_operand_write


def _sar(dst, cnt):
    """Lift one `sar dst, cnt` and return the value-write statement."""
    insn = Instruction(0, 2, "sar", "", "00")
    insn.operands = [dst, cnt]
    lifted = Lifter().lift_instruction(insn)
    # needs_cf is off, so the statements are the write and the result
    # snapshot the condition reads -- see Lifter._result_snapshot. The sweep
    # is about the write; the snapshot rides along harmlessly.
    assert len(lifted) == 2, lifted
    return " ".join(lifted)


def _legacy(dst, cnt):
    """The pre-fix statement, spelled with the lifter's own operand helpers.

    This is the line `_lift_sar` used to emit, verbatim: one int32_t cast at
    every width, and the count passed through unmasked. Built from the same
    helpers rather than hand-written C so the control is the real old code and
    differs from the new one only in the two places the fix touched.
    """
    return _fmt_operand_write(
        dst,
        f"(uint32_t)((int32_t){_fmt_operand_read(dst)} >> {_fmt_operand_read(cnt)})")


# Upstream's register and memory accessors, verbatim from
# templates/runtime/recomp_types.h -- the zero-extension is the point.
PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
/* The result-setter family publishes its ZF/SF here next to the write,
   so a later jcc reads the result and not a destination something
   has since overwritten. */
static uint32_t _fa, _fb;
static int32_t _fas, _fbs;
static uint8_t g_ram[64];
#define MEM8(a)  (*(volatile uint8_t *)(g_ram + (a)))
#define MEM16(a) (*(volatile uint16_t *)(g_ram + (a)))
"""

# One sweep. VALUES is the value loop, STMT the lifted statement, GOT how the
# result is read back, WANT the honest x86 answer.
BODY = r"""
int main(void) {
    for (uint32_t v = 0; v <= VMAX; v++) {
        for (uint32_t c = 0; c <= CMAX; c++) {
            uint32_t eax = 0xAAAAAA00u, ecx = 0xBBBBBB00u, esi = 0;
            (void)esi;
            SEED;
            ecx |= c;
            STMT;
            uint32_t got  = (uint32_t)(GOT);
            uint32_t want = (uint32_t)(WANT);
            if (got != want) {
                printf("FAIL v=0x%X c=%u got=0x%X want=0x%X\n", v, c, got, want);
                return 1;
            }
            if ((eax & KEEPMASK) != (0xAAAAAA00u & KEEPMASK)) {
                printf("FAIL v=0x%X c=%u clobbered eax=0x%X\n", v, c, eax);
                return 1;
            }
        }
    }
    printf("OK\n");
    return 0;
}
"""

# dst operand, seed, read-back, reference, value ceiling, untouched-bits mask.
CASES = {
    "al": (Operand(type="reg", reg="al"),
           "eax = (eax & 0xFFFFFF00u) | v", "LO8(eax)",
           "(uint8_t)((int8_t)v >> (c & 31))", 0xFF, "0xFFFFFF00u"),
    "ah": (Operand(type="reg", reg="ah"),
           "eax = (eax & 0xFFFF00FFu) | (v << 8)", "HI8(eax)",
           "(uint8_t)((int8_t)v >> (c & 31))", 0xFF, "0xFFFF00FFu"),
    "ax": (Operand(type="reg", reg="ax"),
           "eax = (eax & 0xFFFF0000u) | v", "LO16(eax)",
           "(uint16_t)((int16_t)v >> (c & 31))", 0xFFFF, "0xFFFF0000u"),
    "eax": (Operand(type="reg", reg="eax"),
            "eax = v", "eax",
            "(uint32_t)((int32_t)v >> (c & 31))", 0xFF, "0u"),
    "byte [esi]": (Operand(type="mem", mem_base="esi", mem_size=1),
                   "MEM8(0) = (uint8_t)v", "MEM8(0)",
                   "(uint8_t)((int8_t)v >> (c & 31))", 0xFF, "0u"),
}


def _find_cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


class SarWidthTest(unittest.TestCase):
    def _run(self, name, stmt, cmax):
        """Compile and run one sweep. Returns (returncode, output)."""
        _, seed, got, want, vmax, keep = CASES[name]
        src = PRELUDE + (BODY
                         .replace("SEED", seed)
                         .replace("STMT", stmt.rstrip(";"))
                         .replace("GOT", got)
                         .replace("WANT", want)
                         .replace("VMAX", str(vmax))
                         .replace("CMAX", str(cmax))
                         .replace("KEEPMASK", keep))
        # 32-bit sweeps a spread of boundary values, not 0..255.
        if name == "eax":
            src = src.replace(
                "for (uint32_t v = 0; v <= 255; v++) {",
                "static const uint32_t VS[] = {0u, 1u, 2u, 0x7Fu, 0x80u, 0xFFu,\n"
                "        0x7FFFu, 0x8000u, 0xFFFFu, 0x7FFFFFFFu, 0x80000000u,\n"
                "        0xFFFFFFFFu, 0xFFFFFF80u, 0xDEADBEEFu, 0x40000000u};\n"
                "    for (unsigned vi = 0; vi < sizeof(VS)/sizeof(VS[0]); vi++) {\n"
                "        const uint32_t v = VS[vi];")
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

    def setUp(self):
        if not _find_cc():
            self.skipTest("no C compiler on PATH")

    def test_sar_matches_x86_at_every_width(self):
        """The lifted `sar dst, cl` must equal a signed shift of dst's width.

        Counts run to 63, which covers the required spread (0, 1, 7, 8, 31) and
        every count at or past the operand width, where x86 sign-fills and the
        5-bit mask starts to matter.
        """
        for name, (dst, *_rest) in CASES.items():
            with self.subTest(operand=name):
                stmt = _sar(dst, Operand(type="reg", reg="cl"))
                rc, out = self._run(name, stmt, cmax=63)
                self.assertEqual(rc, 0, f"{name}: {out}\n  stmt: {stmt}")
                self.assertTrue(out.startswith("OK"), out)

    def test_pre_fix_expression_is_rejected(self):
        """Negative control: the old statement must fail the same sweep.

        Counts stop at 31 so the control fails on the width bug alone. Past 31
        the old expression is an undefined shift, and a host that happens to
        mask it would make the control pass for the wrong reason.
        """
        for name in ("al", "ah", "ax", "byte [esi]"):
            with self.subTest(operand=name):
                dst = CASES[name][0]
                cl = Operand(type="reg", reg="cl")
                stmt = _legacy(dst, cl)
                self.assertNotEqual(stmt, _sar(dst, cl))
                rc, out = self._run(name, stmt, cmax=31)
                self.assertNotEqual(
                    rc, 0,
                    f"{name}: the pre-fix expression passed the sweep, so the "
                    f"sweep does not test anything: {out}")
                self.assertIn("FAIL", out)

    def test_sar_al_1_on_0x80(self):
        """The measured case, pinned on its own: 0x80 >> 1 is 0xC0, not 0x40."""
        stmt = _sar(Operand(type="reg", reg="al"), Operand(type="imm", imm=1))
        # The width-correct cast, whichever way it is spelled around the read.
        self.assertIn("(int8_t)", stmt)
        src = PRELUDE + (
            "int main(void){ uint32_t eax = 0xAAAAAA80u; " + stmt +
            " printf(\"%02X\\n\", LO8(eax)); return 0; }")
        cc = _find_cc()
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            with open(c, "w") as f:
                f.write(src)
            exe = os.path.join(tmp, "t.exe")
            r = subprocess.run([cc, "-w", c, "-o", exe],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-1500:])
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.stdout.strip(), "C0", r.stdout + r.stderr)

    def test_count_is_masked_to_five_bits(self):
        """x86 masks the count; C leaves a shift of 33 undefined.

        Asserted on the emitted text. Running the unmasked form at a count of
        33 would prove nothing either way: it is undefined behaviour, and the
        common host lowering happens to mask, so it can silently look correct.
        """
        for name, (dst, *_rest) in CASES.items():
            with self.subTest(operand=name):
                self.assertIn("& 31", _sar(dst, Operand(type="reg", reg="cl")))
                self.assertIn("& 31", _sar(dst, Operand(type="imm", imm=33)))

    def test_masked_count_still_matches_x86(self):
        """Counts of 32 and up must fold to count & 31, not saturate."""
        stmt = _sar(CASES["al"][0], Operand(type="reg", reg="cl"))
        rc, out = self._run("al", stmt, cmax=63)
        self.assertEqual(rc, 0, out)
        self.assertTrue(out.startswith("OK"), out)


if __name__ == "__main__":
    unittest.main()
