"""
Self-check for the flat, directly-indexed dispatch table.

Run: py -3 tools/recomp/test_dispatch_flat.py

This one actually compiles and runs the generated C, because the thing being
tested IS generated C on the hottest path in the program -- every indirect call
in the title goes through recomp_lookup. A Python-level check that the right
text was emitted would not catch an off-by-one in the index or a signedness bug
in the bounds check, which are exactly the mistakes available here.

The property that matters: the flat table and the binary search must return the
same answer for every address, including the ones outside the span. The flat
path is only a safe optimisation if it is indistinguishable.

Skips itself if no C compiler is on PATH.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import BatchTranslator  # noqa: E402


def _find_cc():
    for cc in ("clang", "gcc", "cc"):
        p = shutil.which(cc)
        if p:
            return p
    for p in (r"C:\Program Files\LLVM\bin\clang.exe",):
        if os.path.exists(p):
            return p
    return None


# Deliberately awkward: a sparse span with a big hole, a first and a last entry
# whose offsets exercise both ends of the bounds check.
TRANSLATIONS = [
    (0x00011000, "f_a", None),
    (0x00011004, "f_b", None),
    (0x00012000, "f_c", None),
    (0x000203FC, "f_d", None),
]

HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t);
int    recomp_dispatch_init(void);
size_t recomp_dispatch_flat_bytes(void);

void f_a(void) {} void f_b(void) {} void f_c(void) {} void f_d(void) {}

static const uint32_t known[] = {0x00011000,0x00011004,0x00012000,0x000203FC};
static const uint32_t absent[] = {
    0x00000000, 0x00000004, 0x00010FFF, 0x00011001, 0x00011008,
    0x00011FFF, 0x000203FB, 0x000203FD, 0x00020400, 0xFFFFFFFF,
    0xFE000000, 0x7FFFFFFF, 0x80000000
};

int main(void) {
    recomp_func_t before_known[4], before_absent[13];
    size_t i;
    /* search path first -- init has not been called yet */
    if (recomp_dispatch_flat_bytes() != 0) { printf("FAIL: flat before init\n"); return 1; }
    for (i = 0; i < 4;  i++) before_known[i]  = recomp_lookup(known[i]);
    for (i = 0; i < 13; i++) before_absent[i] = recomp_lookup(absent[i]);

    for (i = 0; i < 4; i++)
        if (!before_known[i]) { printf("FAIL: search missed %08X\n", known[i]); return 1; }
    for (i = 0; i < 13; i++)
        if (before_absent[i]) { printf("FAIL: search invented %08X\n", absent[i]); return 1; }

    if (!recomp_dispatch_init()) { printf("FAIL: init returned 0\n"); return 1; }
    if (recomp_dispatch_flat_bytes() == 0) { printf("FAIL: no flat bytes\n"); return 1; }
    if (!recomp_dispatch_init()) { printf("FAIL: re-init returned 0\n"); return 1; }

    /* flat path must be indistinguishable from the search */
    for (i = 0; i < 4; i++)
        if (recomp_lookup(known[i]) != before_known[i]) {
            printf("FAIL: flat disagrees at %08X\n", known[i]); return 1; }
    for (i = 0; i < 13; i++)
        if (recomp_lookup(absent[i]) != before_absent[i]) {
            printf("FAIL: flat disagrees at absent %08X\n", absent[i]); return 1; }

    if (recomp_lookup(0x00011000) != (recomp_func_t)f_a) { printf("FAIL: wrong fn\n"); return 1; }
    if (recomp_lookup(0x000203FC) != (recomp_func_t)f_d) { printf("FAIL: wrong last fn\n"); return 1; }
    printf("OK flat=%zu bytes\n", recomp_dispatch_flat_bytes());
    return 0;
}
"""


def test_generated_dispatch_flat_matches_binary_search():
    cc = _find_cc()
    if not cc:
        print("  SKIP no C compiler on PATH")
        return
    with tempfile.TemporaryDirectory() as tmp:
        # The generated file includes the per-title funcs header; a stub is
        # enough, the harness provides the real symbols.
        with open(os.path.join(tmp, "recomp_funcs.h"), "w") as f:
            f.write("#include <stdint.h>\n#include <stddef.h>\n"
                    "void f_a(void); void f_b(void); void f_c(void); void f_d(void);\n")
        disp = os.path.join(tmp, "recomp_dispatch.c")
        BatchTranslator._write_dispatch_table(
            object.__new__(BatchTranslator), TRANSLATIONS, disp, "recomp_funcs.h")

        src = open(disp).read()
        assert "recomp_dispatch_init" in src, "generator did not emit the flat table"
        assert "g_flat_base = 0x00011000u" in src, src[:400]
        # span must cover the last entry inclusively: 0x203FC - 0x11000 + 1
        assert "g_flat_span = 0x0000F3FD" in src, \
            [l for l in src.splitlines() if "g_flat_span" in l]

        hp = os.path.join(tmp, "harness.c")
        with open(hp, "w") as f:
            f.write(HARNESS)
        exe = os.path.join(tmp, "t.exe")
        r = subprocess.run([cc, "-w", "-I", tmp, hp, disp, "-o", exe],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-2000:]
        r = subprocess.run([exe], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        assert r.stdout.startswith("OK"), r.stdout
        print("     " + r.stdout.strip())


def test_empty_translation_set_does_not_divide_by_zero():
    with tempfile.TemporaryDirectory() as tmp:
        disp = os.path.join(tmp, "d.c")
        BatchTranslator._write_dispatch_table(
            object.__new__(BatchTranslator), [], disp, "recomp_funcs.h")
        src = open(disp).read()
        assert "g_flat_span = 0x00000000u" in src, src[:400]
        assert "if (!g_flat_span) return 0;" in src


def test_reserved_name_mangled_everywhere():
    # The recompiled image carries its own CRT; Black's function named
    # `onexit` clashed with UCRT's onexit_t __cdecl onexit(onexit_t) (C2373)
    # in the dispatch TU, which includes <stdlib.h>. The identifier, its
    # forward declaration, every call site and the dispatch row must share one
    # mangled name.
    from tools.recomp.lifter import _func_ident

    assert _func_ident(0x000A1C60, "onexit") == "onexit_000A1C60"
    assert _func_ident(0x000A1C60, "sub_000A1C60") == "sub_000A1C60"
    assert _func_ident(0x000A1C60, None) == "sub_000A1C60"


def test_reserved_dispatch_row_uses_mangled_name():
    with tempfile.TemporaryDirectory() as tmp:
        disp = os.path.join(tmp, "d.c")
        BatchTranslator._write_dispatch_table(
            object.__new__(BatchTranslator),
            [(0x000A1C60, "onexit_000A1C60", "void onexit_000A1C60(void) {}")],
            disp, "recomp_funcs.h")
        src = open(disp).read()
        assert "(recomp_func_t)onexit_000A1C60" in src


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
