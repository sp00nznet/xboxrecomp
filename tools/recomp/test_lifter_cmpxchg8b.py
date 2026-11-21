"""cmpxchg8b m64 (locked or not) is a real 64-bit compare-and-swap.

It is what MSVC compiles InterlockedCompareExchange64 to, and what the SList
push/pop use to swap a pointer and its sequence count together. It used to
lift to RECOMP_UNIMPL: the swap never happened and edx:eax kept whatever it
held, so a lock-free retry loop spun forever or, worse, believed it had won.

Semantics (SDM): compare edx:eax with the qword; if equal, ZF=1 and ecx:ebx is
stored; otherwise ZF=0 and the qword is loaded into edx:eax. No other flag is
written.

The text tests pin the lowering and the ZF rule. The runtime tests compile
the lifted block against the real recomp_types.h: one checks the match and
both half-mismatch cases against the definition above, the other runs a
cmpxchg8b increment loop on several host threads across the 32-bit carry, so
a CAS that tore into two 32-bit halves, or was not atomic at all, would lose
counts. Both skip themselves without a C compiler; the threaded one also
skips on Windows, where the harness has no pthreads.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

from . import config
from .disasm import BasicBlock, Instruction, Operand
from .lifter import (Lifter, _EFLAGS_SETTERS, _FLAGS_UNDEFINED,
                     _make_condition, lift_basic_block)
from .translator import FunctionTranslator

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNTIME = os.path.join(ROOT, "templates", "runtime")


def _qword(base="esi", disp=0):
    return Operand(type="mem", mem_base=base, mem_index=None, mem_scale=1,
                   mem_disp=disp, mem_size=8)


def _insn(mnemonic, operands=(), addr=0):
    i = Instruction(addr, 1, mnemonic, "", "")
    i.operands = list(operands)
    return i


def _lift_block(insns):
    lifter = Lifter()
    out, state = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=list(insns)))
    return lifter, "\n".join(out), state


def _lift_with_sete(mnemonic):
    """`<mnemonic> qword ptr [esi]; sete cl` -- the CAS and its ZF."""
    return _lift_block([_insn(mnemonic, [_qword()]),
                        _insn("sete", [Operand(type="reg", reg="cl")], 1)])


class TextTest(unittest.TestCase):
    def test_both_spellings_lift_to_a_64_bit_cas(self):
        for m in ("cmpxchg8b", "lock cmpxchg8b"):
            with self.subTest(mnemonic=m):
                lifter, out, _ = _lift_block([_insn(m, [_qword("esi", 8)])])
                self.assertIn("RECOMP_ATOMIC_CAS64(XBOX_PTR(esi + 8)", out)
                self.assertNotIn("RECOMP_UNIMPL", out)
                self.assertNotIn("TODO", out)
                self.assertEqual(lifter.unimplemented, {})

    def test_comparand_is_edx_eax_and_replacement_ecx_ebx(self):
        _, out, _ = _lift_block([_insn("lock cmpxchg8b", [_qword()])])
        self.assertIn("((uint64_t)edx << 32) | eax", out)
        self.assertIn("((uint64_t)ecx << 32) | ebx", out)

    def test_edx_eax_is_only_loaded_on_a_mismatch(self):
        _, out, _ = _lift_block([_insn("lock cmpxchg8b", [_qword()])])
        self.assertIn("if (_old8 != _cmp8) { eax = (uint32_t)_old8;"
                      " edx = (uint32_t)(_old8 >> 32); }", out)

    def test_zf_is_snapshotted_before_edx_eax_changes(self):
        # A jz after it must test the compare the instruction did, not the
        # registers it may just have reloaded.
        _, out, _ = _lift_block([_insn("lock cmpxchg8b", [_qword()])])
        self.assertLess(out.index("_fa = (uint32_t)(_old8 != _cmp8); _fb = 0;"),
                        out.index("eax = (uint32_t)_old8"))

    def test_lock_prefix_is_tracked_as_the_bare_setter(self):
        for m in ("cmpxchg8b", "lock cmpxchg8b"):
            with self.subTest(mnemonic=m):
                _, _, state = _lift_block([_insn(m, [_qword()])])
                self.assertEqual(state[0], "cmpxchg8b")
        self.assertIn("cmpxchg8b", _EFLAGS_SETTERS)
        self.assertNotIn("cmpxchg8b", _FLAGS_UNDEFINED)

    def test_zf_consumers_read_the_snapshot(self):
        ops = [_qword()]
        self.assertEqual(_make_condition("je", "cmpxchg8b", ops)[0],
                         "(_fa == _fb)")
        self.assertEqual(_make_condition("jne", "cmpxchg8b", ops)[0],
                         "(_fa != _fb)")
        _, out, _ = _lift_with_sete("lock cmpxchg8b")
        self.assertIn("SET_LO8(ecx, ((_fa == _fb)) ? 1 : 0);", out)

    def test_flags_it_does_not_write_are_not_invented(self):
        # cmpxchg8b only writes ZF. A carry or sign test after it has no rule
        # here, rather than a made-up one.
        for jcc in ("jb", "jae", "js", "jl", "jg", "jo", "jp"):
            with self.subTest(jcc=jcc):
                self.assertIsNone(_make_condition(jcc, "cmpxchg8b", [_qword()]))


class TranslatorDeclTest(unittest.TestCase):
    """A function whose only flag setter is cmpxchg8b declares _fa/_fb."""

    def _translate(self, hexcode):
        image = bytes.fromhex(hexcode)
        base = 0x10000
        config._install([config.Section(".text", base, len(image), 0,
                                        len(image), True)],
                        entry_point=base, kernel_thunk_addr=base,
                        origin="cmpxchg8b-decl-test")
        db = {base: {"start": hex(base), "end": base + len(image),
                     "_addr": base, "size": len(image)}}
        return FunctionTranslator(image, db).translate_function(base, db[base])

    def test_locked_and_unlocked_declare_the_snapshot(self):
        # lock cmpxchg8b [esi]; ret   and   cmpxchg8b [esi]; ret
        for hexcode in ("f00fc70e" "c3", "0fc70e" "c3"):
            with self.subTest(code=hexcode):
                code = self._translate(hexcode)
                self.assertIn("RECOMP_ATOMIC_CAS64", code)
                self.assertIn("uint32_t _fa = 0", code)
                self.assertNotIn("RECOMP_UNIMPL(\"", code.split(
                    "RECOMP_ATOMIC_CAS64")[1].split("\n")[0])


HARNESS = r"""
#include "recomp_types.h"
#include <stdio.h>
#include <string.h>
%(threads_include)s

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eflags = 0x00000202u;
RECOMP_TLS int g_df = 0;
static volatile unsigned long g_unimpl;
void recomp_unimpl(const char *text, uint32_t va)
{
    (void)text; (void)va;
    g_unimpl++;
}

static _Alignas(16) uint8_t g_mem[64];

/* The lifted `%(mnemonic)s qword ptr [esi]; sete cl`, with the guest
 * registers as locals so every host thread has its own. Returns ZF. */
static int cas8(uint32_t va, uint32_t *peax, uint32_t *pedx,
                uint32_t ebx, uint32_t ecx)
{
    uint32_t eax = *peax, edx = *pedx, esi = va;
    uint32_t _fa = 0, _fb = 0; int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
%(body)s
    *peax = eax; *pedx = edx;
    return (int)(ecx & 0xFFu);
}

static uint64_t rd64(uint32_t va)
{
    uint64_t v; memcpy(&v, g_mem + va, 8); return v;
}

static void wr64(uint32_t va, uint64_t v) { memcpy(g_mem + va, &v, 8); }

static unsigned long g_cases, g_bad;

/* One CAS against the SDM definition. */
static void check(uint64_t mem, uint64_t cmp, uint64_t val)
{
    uint32_t eax = (uint32_t)cmp, edx = (uint32_t)(cmp >> 32);
    int want_zf = mem == cmp, zf;
    uint64_t want_mem = want_zf ? val : mem;
    uint64_t want_edx_eax = want_zf ? cmp : mem;
    wr64(8, mem);
    zf = cas8(8, &eax, &edx, (uint32_t)val, (uint32_t)(val >> 32));
    g_cases++;
    if (zf != want_zf || rd64(8) != want_mem
            || ((uint64_t)edx << 32 | eax) != want_edx_eax
            || rd64(0) != 0x1111111111111111ull
            || rd64(16) != 0x2222222222222222ull) {
        if (g_bad++ < 8)
            printf("bad mem=%%016llX cmp=%%016llX val=%%016llX zf=%%d\n",
                   (unsigned long long)mem, (unsigned long long)cmp,
                   (unsigned long long)val, zf);
    }
}

%(threads)s

int main(void)
{
    static const uint64_t v[] = {
        0, 1, 0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFF00000000ull,
        0x00000001FFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
        0x0123456789ABCDEFull, 0x0123456789ABCDEEull, 0x0123456689ABCDEFull};
    size_t n = sizeof v / sizeof v[0], i, j;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    wr64(0, 0x1111111111111111ull);
    wr64(16, 0x2222222222222222ull);
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            check(v[i], v[j], 0xDEADBEEFCAFEF00Dull);
            check(v[i], v[i], v[j]);
        }
%(run_threads)s
    printf("cases %%lu bad %%lu unimpl %%lu\n", g_cases, g_bad, g_unimpl);
    return 0;
}
"""

THREADS = r"""
#define NTHREADS 4
#define PER_THREAD 100000u
#define START 0x00000000FFFFF000ull  /* the low half carries into the high */

/* InterlockedIncrement64 as MSVC writes it: read, add, cmpxchg8b, retry. A
 * failed CAS hands back the current value in edx:eax, which is the next
 * comparand -- so this also checks the mismatch reload under contention. */
static void *worker(void *arg)
{
    unsigned k;
    uint32_t eax = 0, edx = 0;   /* deliberately stale: the CAS reloads */
    (void)arg;
    for (k = 0; k < PER_THREAD; k++) {
        for (;;) {
            uint64_t next = ((uint64_t)edx << 32 | eax) + 1;
            if (cas8(32, &eax, &edx, (uint32_t)next, (uint32_t)(next >> 32)))
                break;
        }
    }
    return NULL;
}

static void run_threads(void)
{
    pthread_t t[NTHREADS];
    int i;
    uint64_t want = START + (uint64_t)NTHREADS * PER_THREAD;
    wr64(32, START);
    for (i = 0; i < NTHREADS; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (i = 0; i < NTHREADS; i++)
        pthread_join(t[i], NULL);
    g_cases++;
    if (rd64(32) != want) {
        g_bad++;
        printf("threads: counter %016llX want %016llX\n",
               (unsigned long long)rd64(32), (unsigned long long)want);
    }
}
"""


def _harness(mnemonic, threaded):
    _, body, _ = _lift_with_sete(mnemonic)
    return HARNESS % {
        "mnemonic": mnemonic,
        "body": body,
        "threads_include": "#include <pthread.h>" if threaded else "",
        "threads": THREADS if threaded else "",
        "run_threads": "    run_threads();" if threaded else "",
    }


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(_cc(), "no C compiler")
class RuntimeTest(unittest.TestCase):
    def _run(self, mnemonic, opt, threaded):
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
            with open(c, "w", encoding="utf-8") as f:
                f.write(_harness(mnemonic, threaded))
            cmd = [_cc(), "-std=c11", opt, "-I", RUNTIME,
                   "-Wall", "-Wextra", "-Werror", "-o", exe, c]
            if threaded:
                cmd.insert(1, "-pthread")
            subprocess.run(cmd, check=True)
            return subprocess.run([exe], check=True, capture_output=True,
                                  text=True, timeout=120).stdout

    def test_matches_the_definition(self):
        for m in ("cmpxchg8b", "lock cmpxchg8b"):
            for opt in ("-O0", "-O2"):
                with self.subTest(mnemonic=m, opt=opt):
                    out = self._run(m, opt, threaded=False)
                    self.assertRegex(out.splitlines()[-1],
                                     r"^cases \d+ bad 0 unimpl 0$", out)

    @unittest.skipIf(os.name == "nt", "the threaded harness uses pthreads")
    def test_is_atomic_across_host_threads(self):
        for opt in ("-O0", "-O2"):
            with self.subTest(opt=opt):
                out = self._run("lock cmpxchg8b", opt, threaded=True)
                self.assertRegex(out.splitlines()[-1],
                                 r"^cases \d+ bad 0 unimpl 0$", out)


if __name__ == "__main__":
    unittest.main()
