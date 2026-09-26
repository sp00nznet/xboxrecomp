"""A guarded indirect call behaves exactly like the unguarded one.

test_icall_guarded.py checks the text the lifter emits. This compiles it and
runs it, against RECOMP_ICALL_SAFE_AT taken verbatim from the runtime header,
because the property that makes guarding safe is behavioural: for every
target, the guarded site must call the same function and leave the guest
stack where the generic dispatch would. A recorded target must be taken
directly, without a dispatch lookup; a target the record never saw must fall
through to the lookup; one the lookup cannot resolve must still rewind esp.

The second half round-trips the runtime's per-site record through
src/kernel/icall_feedback.c and the merge tool's parser, since the dump
format is the contract between the two.

Skips itself if no C compiler is on PATH.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Instruction, Operand  # noqa: E402
from tools.recomp.icall_feedback import parse_sites_dump  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402
from tools.recomp.translator import _fixup_icall_esp_save  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")
FEEDBACK_C = os.path.join(ROOT, "src", "kernel", "icall_feedback.c")
FEEDBACK_H_DIR = os.path.join(ROOT, "src", "kernel")

A, B, C = 0x00011000, 0x00011100, 0x00011200
SITE = 0x00010040


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


def _macro(name):
    """One #define from the runtime header, continuation lines included."""
    lines = open(HEADER, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        if re.match(r"#define\s+%s\(" % re.escape(name), line):
            out = [line]
            while out[-1].rstrip().endswith("\\"):
                i += 1
                out.append(lines[i])
            return "\n".join(out)
    raise AssertionError(f"{name} not defined in {HEADER}")


def _lifted_site():
    lifter = Lifter(func_db={A: {"name": "sub_A"}, B: {"name": "sub_B"},
                             C: {"name": "sub_C"}})
    lifter.icall_site_targets = {SITE: [A, B]}
    insn = Instruction(SITE, 2, "call", "eax", "ffd0")
    insn.operands = [Operand(type="reg", reg="eax")]
    lines = lifter.lift_instruction(insn)
    assert "RECOMP_ICALL_GUARD_HIT" in lines[0], lines
    return "\n".join(_fixup_icall_esp_save(lines))


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
typedef void (*recomp_func_t)(void);
#define ICALL_TRACE_SIZE 16
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
volatile uint64_t g_icall_count;
uint32_t g_xbox_code_lo = 0x00010000u, g_xbox_code_hi = 0x00020000u;
uint32_t g_esp, eax;
#define esp g_esp
#define PUSH32(sp, v) do { (sp) -= 4; } while (0)
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ICALL_OBSERVE_SITE(site, va) ((void)0)
#define RECOMP_ABI_CALL(va, fn) (fn)()
static int hits, misses, lookups, fails, called_a, called_b, called_c;
#define RECOMP_ICALL_GUARD_HIT()  ((void)hits++)
#define RECOMP_ICALL_GUARD_MISS() ((void)misses++)
/* Each callee pops its return address, as a guest `ret` would. */
void sub_A(void) { called_a++; g_esp += 4; }
void sub_B(void) { called_b++; g_esp += 4; }
void sub_C(void) { called_c++; g_esp += 4; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; lookups++; return 0; }
recomp_func_t recomp_lookup(uint32_t va) {
    return va == 0x00011000u ? sub_A : va == 0x00011100u ? sub_B
         : va == 0x00011200u ? sub_C : 0;
}
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
void recomp_icall_fail_log(uint32_t va) { (void)va; fails++; }
void recomp_icall_not_code_log(uint32_t va) { (void)va; fails++; }
IS_CODE
SAFE_AT
static void site(void) {
LIFTED
}
static int check(const char *what, uint32_t target, int a, int b, int c,
                 int h, int m, int l, int f) {
    hits = misses = lookups = fails = called_a = called_b = called_c = 0;
    g_esp = 0x1000u; eax = target;
    site();
    if (g_esp != 0x1000u || called_a != a || called_b != b || called_c != c
            || hits != h || misses != m || lookups != l || fails != f) {
        printf("FAIL %s: esp=%X a=%d b=%d c=%d hit=%d miss=%d lookup=%d fail=%d\n",
               what, g_esp, called_a, called_b, called_c, hits, misses,
               lookups, fails);
        return 1;
    }
    return 0;
}
int main(void) {
    int bad = 0;
    bad |= check("recorded A", 0x00011000u, 1, 0, 0, 1, 0, 0, 0);
    bad |= check("recorded B", 0x00011100u, 0, 1, 0, 1, 0, 0, 0);
    bad |= check("unseen C",   0x00011200u, 0, 0, 1, 0, 1, 1, 0);
    bad |= check("unresolved", 0x00011300u, 0, 0, 0, 0, 1, 1, 1);
    bad |= check("not code",   0x00F00000u, 0, 0, 0, 0, 1, 0, 1);
    if (!bad) printf("OK\n");
    return bad;
}
"""


def _run(tmp, srcs, extra=()):
    exe = os.path.join(tmp, "t.exe")
    r = subprocess.run([_cc(), "-w", *extra, *srcs, "-o", exe],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    r = subprocess.run([exe], capture_output=True, text=True, cwd=tmp)
    return r


def test_guarded_site_matches_generic_dispatch_for_every_target():
    if not _cc():
        pytest.skip("no C compiler on PATH")
    src = (HARNESS.replace("IS_CODE", _macro("RECOMP_ICALL_IS_CODE"))
                  .replace("SAFE_AT", _macro("RECOMP_ICALL_SAFE_AT"))
                  .replace("LIFTED", _lifted_site()))
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "h.c")
        with open(p, "w") as f:
            f.write(src)
        r = _run(tmp, [p])
        assert r.returncode == 0 and r.stdout.startswith("OK"), \
            r.stdout + r.stderr + "\n" + src


RECORDER = r"""
#include <stdint.h>
#include "recomp_icall_feedback.h"
int main(void) {
    for (int i = 0; i < 1000; i++)                 /* one hot target */
        RECOMP_ICALL_OBSERVE_SITE(0x00010040u, 0x00011000u);
    RECOMP_ICALL_OBSERVE_SITE(0x00010080u, 0x00011000u);
    RECOMP_ICALL_OBSERVE_SITE(0x00010080u, 0x00011100u);
    RECOMP_ICALL_OBSERVE_SITE(0x00010080u, 0x00011000u);
    for (uint32_t t = 0; t < 9; t++)               /* more than a record holds */
        RECOMP_ICALL_OBSERVE_SITE(0x000100C0u, 0x00012000u + 16u * t);
    recomp_icall_feedback_dump("icall_targets.dump");
    return 0;
}
"""


def test_runtime_site_dump_round_trips_through_the_merge_parser():
    if not _cc():
        pytest.skip("no C compiler on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "rec.c")
        with open(p, "w") as f:
            f.write(RECORDER)
        r = _run(tmp, [p, FEEDBACK_C],
                 ["-DRECOMP_ICALL_FEEDBACK", "-I", FEEDBACK_H_DIR])
        assert r.returncode == 0, r.stdout + r.stderr
        sites = parse_sites_dump(os.path.join(tmp, "icall_sites.dump"))
        assert sites[0x00010040] == ({0x00011000}, False)
        assert sites[0x00010080] == ({0x00011000, 0x00011100}, False)
        targets, saturated = sites[0x000100C0]
        assert saturated and len(targets) < 9
        assert len(sites) == 3
