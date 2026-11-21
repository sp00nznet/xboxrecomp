"""The Pentium III's SSE-integer additions on mm registers: pminub, pmaxub,
pmulhuw and maskmovq.

All four reached RECOMP_UNIMPL. The first three are byte clamps and the
unsigned high multiply that video and image decoders lean on; maskmovq is
the byte-masked store some of them write their output with. Each is decoded
here from its real encoding, so the operand shapes are capstone's and not a
hand-built guess, and the lifted C is then compiled against the runtime
header and run: a text check cannot tell an unsigned compare from a signed
one, or a masked store from a plain one.
"""

import subprocess
import tempfile
import unittest
from pathlib import Path

from . import c_fixture
from .disasm import Disassembler
from .lifter import Lifter, _EFLAGS_PRESERVE

_ROOT = Path(__file__).resolve().parents[2]

# (encoding, mnemonic) -- reg-reg and reg-mem for the arithmetic, and
# maskmovq with no prefix and with an fs override.
_ARITH = {
    "pminub": ("0fdac1", "0fda4010"),     # pminub mm0, mm1 / [eax+0x10]
    "pmaxub": ("0fdec1", "0fde4010"),
    "pmulhuw": ("0fe4c1", "0fe44010"),
}
_HELPER = {"pminub": "MMX_PMINUB", "pmaxub": "MMX_PMAXUB",
           "pmulhuw": "MMX_PMULHUW"}


def _lift(hexbytes):
    code = bytes.fromhex(hexbytes)
    insns = Disassembler().disassemble_function(code, 0x1000, 0x1000 + len(code))
    assert len(insns) == 1, insns
    lifter = Lifter()
    return lifter, insns[0], "\n".join(lifter.lift_instruction(insns[0]))


class SseIntegerMmxLiftTest(unittest.TestCase):

    def test_arithmetic_lifts_to_its_helper(self):
        for m, encodings in _ARITH.items():
            for enc in encodings:
                with self.subTest(mnemonic=m, encoding=enc):
                    lifter, insn, out = _lift(enc)
                    self.assertEqual(insn.mnemonic, m)
                    self.assertIn(f"mm0 = {_HELPER[m]}(mm0, ", out)
                    self.assertNotIn("RECOMP_UNIMPL", out)
                    self.assertEqual(lifter.unimplemented, {})
        _, _, out = _lift("0fdec1")
        self.assertIn("MMX_PMAXUB(mm0, mm1)", out)
        _, _, out = _lift("0fde4010")
        self.assertIn("MMX_PMAXUB(mm0, MMX_MEM(eax + 0x10))", out)

    def test_maskmovq_stores_through_edi(self):
        lifter, insn, out = _lift("0ff7c1")            # maskmovq mm0, mm1
        self.assertEqual(insn.mnemonic, "maskmovq")
        self.assertIn("MMX_MASKMOVQ(edi, mm0, mm1);", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_maskmovq_fs_override_is_offset(self):
        _, _, out = _lift("640ff7c1")
        self.assertIn("MMX_MASKMOVQ(XBOX_FS_BASE + edi, mm0, mm1);", out)

    def test_maskmovq_forms_it_cannot_address_stay_loud(self):
        # 67: the address would be DI, not EDI. 65: gs has no base here.
        for enc in ("670ff7c1", "650ff7c1"):
            with self.subTest(encoding=enc):
                lifter, _, out = _lift(enc)
                self.assertIn("RECOMP_UNIMPL", out)
                self.assertIn("maskmovq", lifter.unimplemented)

    def test_none_of_them_writes_eflags(self):
        for m in ("pminub", "pmaxub", "pmulhuw", "maskmovq"):
            self.assertIn(m, _EFLAGS_PRESERVE)


_RUNTIME_TEST = r'''
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t nxt(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t pick(unsigned i) {
    static const uint64_t edges[] = { 0, ~0ull, 0x8080808080808080ull,
        0x7F7F7F7F7F7F7F7Full, 0x8000800080008000ull, 0xFF00FF00FF00FF00ull,
        0x0001000100010001ull, 0x00FF00FF00FF00FFull };
    return i < 8 ? edges[i] : nxt();
}

static void lifted_pminub(void)  { @PMINUB@ }
static void lifted_pmaxub(void)  { @PMAXUB@ }
static void lifted_pmulhuw(void) { @PMULHUW@ }
static void lifted_maskmovq(void) { @MASKMOVQ@ }

int main(void) {
    unsigned i, j, k;
    for (i = 0; i < 64; i++) for (j = 0; j < 64; j++) {
        uint64_t a = pick(i), b = pick(j);
        RecompMmx want;
        /* Independent byte/word references for the unsigned semantics: a
         * signed compare or a signed multiply differs on every lane with
         * the top bit set, which the edge values cover. */
        for (k = 0; k < 8; k++) {
            uint8_t x = (uint8_t)(a >> (8 * k)), y = (uint8_t)(b >> (8 * k));
            want.ub[k] = x < y ? x : y;
        }
        g_mm0.q = a; g_mm1.q = b; lifted_pminub();
        if (g_mm0.q != want.q) { printf("pminub %016llx %016llx\n", (unsigned long long)a, (unsigned long long)b); return 1; }
        for (k = 0; k < 8; k++) {
            uint8_t x = (uint8_t)(a >> (8 * k)), y = (uint8_t)(b >> (8 * k));
            want.ub[k] = x > y ? x : y;
        }
        g_mm0.q = a; g_mm1.q = b; lifted_pmaxub();
        if (g_mm0.q != want.q) { printf("pmaxub\n"); return 2; }
        for (k = 0; k < 4; k++) {
            uint32_t x = (uint16_t)(a >> (16 * k)), y = (uint16_t)(b >> (16 * k));
            want.uw[k] = (uint16_t)((x * y) >> 16);
        }
        g_mm0.q = a; g_mm1.q = b; lifted_pmulhuw();
        if (g_mm0.q != want.q) { printf("pmulhuw\n"); return 3; }

        /* maskmovq mm0, mm1: bytes of mm0 where mm1's byte has bit 7, to
         * [edi]; every other byte of the buffer -- inside the 8 and around
         * them -- keeps its value. */
        {
            unsigned char mem[32], ref[32];
            uint32_t at = 4 + (i + j) % 17;
            for (k = 0; k < sizeof mem; k++)
                mem[k] = ref[k] = (unsigned char)(k * 37u + i);
            for (k = 0; k < 8; k++)
                if ((b >> (8 * k + 7)) & 1) ref[at + k] = (unsigned char)(a >> (8 * k));
            g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;
            g_mm0.q = a; g_mm1.q = b; g_edi = at;
            lifted_maskmovq();
            if (memcmp(mem, ref, sizeof mem) || g_edi != at || g_mm0.q != a) {
                printf("maskmovq %016llx %016llx\n", (unsigned long long)a, (unsigned long long)b);
                return 4;
            }
        }
    }
    return 0;
}
'''


class SseIntegerMmxRuntimeTest(unittest.TestCase):

    def test_lifted_code_matches_the_instruction(self):
        cc = c_fixture.compiler()
        if not cc:
            self.skipTest("no C compiler available")
        source = _RUNTIME_TEST
        for key, enc in (("PMINUB", "0fdac1"), ("PMAXUB", "0fdec1"),
                         ("PMULHUW", "0fe4c1"), ("MASKMOVQ", "0ff7c1")):
            _, _, out = _lift(enc)
            source = source.replace(f"@{key}@", out)
        with tempfile.TemporaryDirectory(prefix="mmx-sse-int-") as tmp:
            src, exe = Path(tmp) / "test.c", Path(tmp) / "test.exe"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                c_fixture.command(cc, [src], exe,
                                  include_dirs=[_ROOT / "templates" / "runtime"]),
                cwd=tmp, capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True)
            self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
