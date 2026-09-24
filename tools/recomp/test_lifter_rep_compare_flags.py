"""REPE CMPS / SCAS leave CF, and leave the flags alone at a zero count.

The rep-compare lift recorded ZF in `_flags` and nothing else, which answers
je/jne/sete and nothing more. Two things it missed, both found in the G56
census of JSRF's generated tree (experiments/lifter_flags/census.md):

1. CF. MSVC's memcmp and basic_string::compare end a mismatch with
   `sbb eax, eax; sbb eax, -1`, turning CF into -1 or +1. Nothing wrote _cf,
   so the sbb read the `xor eax, eax` before the compare (CF 0) and the
   compare answered +1 -- "greater" -- for every mismatch. JSRF's string
   compare, sub_00179AE0, is exactly this shape.

2. A zero count. x86 then leaves EFLAGS untouched, so the je after
   `xor eax, eax; repe cmpsb` sees ZF=1 from the xor and reports "equal".
   `_flags` instead kept its last value -- 0 on entry -- so an empty compare
   went down the mismatch arm. sub_00179AE0 loads ECX with the shorter
   length, so comparing anything with an empty string did this.

Each fixture below is compiled from the translator's own output and run
against the architectural answer over a sweep of buffers and counts. The
negative controls remove each half of the fix from the emitted C and require
the sweep to catch it.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import pytest

from . import config
from .translator import FunctionTranslator

BASE = 0x10000

# xor eax, eax; <rep op>; je +5; sbb eax, eax; sbb eax, -1; ret
# -- memcmp's tail: 0 when equal, else -1 / +1 from CF.
_SBB_TAIL = "74051BC083D8FFC3"
# xor eax, eax; <rep op>; jb +8; ja +12; mov eax, 0; ret;
# mov eax, -1; ret; mov eax, 1; ret -- CF and ZF read by branches instead.
_BRANCH_TAIL = "7208770CB800000000C3B8FFFFFFFFC3B801000000C3"

FIXTURES = {
    # name: (rep opcode, tail, element size, is_scas)
    "cmpsb_sbb": ("F3A6", _SBB_TAIL, 1, False),
    "cmpsd_sbb": ("F3A7", _SBB_TAIL, 4, False),
    "cmpsb_branch": ("F3A6", _BRANCH_TAIL, 1, False),
    "cmpsw_branch": ("66F3A7", _BRANCH_TAIL, 2, False),
    "scasb_branch": ("F3AE", _BRANCH_TAIL, 1, True),
}

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t eax, ecx, esi, edi, esp;
static uint8_t mem[512];
#define MEM8(a)  (*(uint8_t  *)(mem + (a)))
#define MEM16(a) (*(uint16_t *)(mem + (a)))
#define MEM32(a) (*(uint32_t *)(mem + (a)))
#define LO8(v)  ((uint8_t)(v))
#define LO16(v) ((uint16_t)(v))
#define RECOMP_DF_STEP(n) (n)
#define CMP_EQ(a, b) ((a) == (b))
"""

MAIN = r"""
static uint32_t elem(uint32_t a, unsigned size) {
    return size == 1 ? MEM8(a) : size == 2 ? MEM16(a) : MEM32(a);
}
/* x86: compare up to n elements, stop at the first mismatch; a zero count
   leaves ZF=1, CF=0 from the xor before it. Result: 0 / -1 / +1. */
static uint32_t expect(uint32_t s, uint32_t d, uint32_t n, unsigned size,
                       int scas, uint32_t acc) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
        uint32_t a = scas ? (acc & mask) : elem(s + i * size, size);
        uint32_t b = elem(d + i * size, size);
        if (a != b) return a < b ? 0xFFFFFFFFu : 1u;
    }
    return 0;
}
int main(void) {
    static const uint8_t patterns[][8] = {
        {1,2,3,4,5,6,7,8}, {1,2,3,4,5,6,7,9}, {1,2,3,4,5,6,7,7},
        {0,2,3,4,5,6,7,8}, {0xFF,2,3,4,5,6,7,8}, {1,2,0x80,4,5,6,7,8},
        {1,2,3,4,0x7F,6,7,8}, {1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0},
    };
    const unsigned P = sizeof patterns / sizeof patterns[0];
    unsigned checked = 0;
    for (unsigned f = 0; f < NF; f++)
    for (unsigned p = 0; p < P; p++)
    for (unsigned q = 0; q < P; q++)
    for (uint32_t n = 0; n <= 8 / sizes[f]; n++) {
        memset(mem, 0, sizeof mem);
        memcpy(mem + 0x40, patterns[p], 8);
        memcpy(mem + 0x80, patterns[q], 8);
        uint32_t acc = patterns[p][0] | (patterns[p][0] << 8)
                     | ((uint32_t)patterns[p][0] << 16)
                     | ((uint32_t)patterns[p][0] << 24);
        uint32_t want = expect(0x40, 0x80, n, sizes[f], scas[f], acc);
        esi = 0x40; edi = 0x80; ecx = n; eax = acc; esp = 1024;
        fixtures[f]();
        if (eax != want) {
            fprintf(stderr, "fixture %s pattern %u vs %u count %u: got %d want %d\n",
                    names[f], p, q, n, (int)eax, (int)want);
            return 1;
        }
        checked++;
    }
    printf("%u\n", checked);
    return 0;
}
"""


def _translate(name, image):
    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="rep-compare-flags")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return (FunctionTranslator(image, db).translate_function(BASE, db[BASE])
            .replace("sub_00010000", f"fixture_{name}"))


def _generated():
    """{name: C source of the translated fixture}."""
    out = {}
    for name, (rep, tail, _size, _scas) in FIXTURES.items():
        # A known ZF=1, CF=0 for a zero count to keep: `xor eax, eax`, or
        # for scas, which compares against eax, `cmp eax, eax` so the
        # accumulator the harness loaded survives.
        head = "39C0" if FIXTURES[name][3] else "31C0"
        out[name] = _translate(name, bytes.fromhex(head + rep + tail))
    return out


def _build_and_run(sources):
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    names = list(FIXTURES)
    code = PRELUDE + "".join(sources[n] for n in names)
    code += f"#define NF {len(names)}\n"
    code += ("static void (*fixtures[])(void) = {"
             + ", ".join(f"fixture_{n}" for n in names) + "};\n")
    code += ("static const char *names[] = {"
             + ", ".join(f'"{n}"' for n in names) + "};\n")
    code += ("static const unsigned sizes[] = {"
             + ", ".join(str(FIXTURES[n][2]) for n in names) + "};\n")
    code += ("static const int scas[] = {"
             + ", ".join("1" if FIXTURES[n][3] else "0" for n in names)
             + "};\n")
    code += MAIN
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / "t.c"
        src.write_text(code)
        exe = Path(temp) / "t"
        built = subprocess.run([cc, "-O1", str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        return subprocess.run([str(exe)], capture_output=True, text=True)


def test_rep_compare_matches_x86_for_every_count_and_order():
    ran = _build_and_run(_generated())
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert int(ran.stdout) > 1000, ran.stdout   # the sweep really ran


def test_negative_control_without_cf_the_order_is_lost():
    srcs = {n: re.sub(r"\n\s*_cf = \(M[^\n]*;", "", s)
            for n, s in _generated().items()}
    assert all("_cf = (M" not in s for s in srcs.values())
    ran = _build_and_run(srcs)
    assert ran.returncode == 1, "dropping CF should make a -1 come out +1"
    assert "want -1" in ran.stderr, ran.stderr


def test_negative_control_without_the_zf_preload_a_zero_count_fails():
    srcs = {n: re.sub(r"\n\s*_flags = \([^\n]*ZF in[^\n]*", "", s)
            for n, s in _generated().items()}
    assert all("ZF in" not in s for s in srcs.values())
    ran = _build_and_run(srcs)
    assert ran.returncode == 1, "a zero count should then read 'not equal'"
    assert "count 0" in ran.stderr, ran.stderr


def test_cf_is_only_computed_where_the_function_reads_it():
    # repe cmpsd; jne; ret -- the QueryInterface/GUID shape. Nothing reads
    # CF, so _cf is neither declared nor written: 256 of JSRF's 259 rep
    # compares are this, and they should cost nothing new.
    code = _translate("guid", bytes.fromhex("F3A77501C3C3"))
    assert "_cf" not in code, code
    assert "_flags = (MEM32(esi) == MEM32(edi));" in code, code
