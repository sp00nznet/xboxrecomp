"""pushfd, popfd and cpuid do what the CPU does.

All three lifted to RECOMP_UNIMPL no-ops. The pair is the classic "does this
CPU have cpuid?" probe -- flip EFLAGS.ID (bit 21) through popfd and see whether
pushfd reads it back -- and as no-ops they left esp four bytes off, so the
function popped its caller's saved registers instead of its own: an EFLAGS.ID
+ CPUID feature probe returned with the caller's callee-saved registers
swapped.

pushfd pushes the g_eflags image after writing the tracked arithmetic flags
into it (publish_flags); popfd loads it.

The text tests pin what is emitted. The runtime tests compile the lifted probe
against the header's own PUSH32/POP32/recomp_pushfd/recomp_popfd and the
runtime's xbox_Cpuid, and run it, because the property that matters is
behavioural: stack balanced, saved registers restored, the ID bit writable,
and MMX/FXSR/SSE reported unless RECOMP_CPUID_NO_SIMD=1 masks them.

The runtime half skips itself if no C compiler is on PATH.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import BasicBlock, Disassembler, Instruction, Operand
from .lifter import Lifter, lift_basic_block, _EFLAGS_PRESERVE

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNTIME = os.path.join(ROOT, "templates", "runtime")
HAL = os.path.join(ROOT, "src", "kernel", "kernel_hal.c")

# A hand-written EFLAGS.ID + CPUID feature probe: save ebx/esi, toggle
# EFLAGS.ID, and if it stuck, return cpuid(1).edx bit 23 (MMX) in eax.
PROBE_VA = 0x00010000
PROBE = bytes.fromhex(
    "53" "56"                         # push ebx; push esi
    "9c" "58" "8bc8"                  # pushfd; pop eax; mov ecx,eax
    "3500002000" "50" "9d"            # xor eax,0x200000; push eax; popfd
    "9c" "58" "33c1" "7411"           # pushfd; pop eax; xor eax,ecx; jz L
    "b801000000" "0fa2"               # mov eax,1; cpuid
    "8bc2" "c1e817" "83e001"          # mov eax,edx; shr eax,23; and eax,1
    "eb02"                            # jmp E
    "33c0"                            # L: xor eax,eax
    "5e" "5b" "c3")                   # E: pop esi; pop ebx; ret


def _insn(mnemonic, operands=(), addr=0):
    i = Instruction(addr, 1, mnemonic, "", "")
    i.operands = list(operands)
    return i


def _reg(name):
    return Operand(type="reg", reg=name)


def _lift_block(insns, needs_cf=False):
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    out, state = lift_basic_block(lifter, BasicBlock(start=0, instructions=insns))
    return lifter, "\n".join(out), state


def _probe_c():
    """The probe lifted block by block into one C function body."""
    insns = Disassembler().disassemble_function(
        PROBE, PROBE_VA, PROBE_VA + len(PROBE))
    starts = {PROBE_VA}
    for i in insns:
        if i.is_branch:
            starts.update({i.jump_target, i.end_address})
    blocks, cur = [], None
    for i in insns:
        if i.address in starts:
            cur = BasicBlock(start=i.address)
            blocks.append(cur)
        cur.instructions.append(i)
    lifter = Lifter()
    lifter.func_start, lifter.func_end = PROBE_VA, PROBE_VA + len(PROBE)
    body, state = [], None
    for bb in blocks:
        body.append(f"loc_{bb.start:08X}: ;")
        stmts, state = lift_basic_block(lifter, bb, flag_state=state)
        body.extend(stmts)
    return "\n".join(body), lifter.unimplemented


def _cpuid_c():
    src = open(HAL, encoding="utf-8").read()
    start = src.index("void xbox_Cpuid(")
    return src[start:src.index("\n}\n", start) + 3]


class TextTest(unittest.TestCase):
    def test_no_unimpl_left(self):
        _, unimpl = _probe_c()
        self.assertEqual(unimpl, {})

    def test_pushfd_without_tracked_flags_reads_g_eflags(self):
        out = Lifter().lift_instruction(_insn("pushfd"))
        self.assertEqual(out, ["PUSH32(esp, recomp_pushfd()); /* pushfd */"])

    def test_pushfd_rebuilds_flags_from_the_tracked_setter(self):
        cmp_ = _insn("cmp", [_reg("eax"), _reg("ebx")])
        _, out, _ = _lift_block([cmp_, _insn("pushfd")])
        self.assertIn("g_eflags = (g_eflags & ~0x8C5u) | ", out)
        self.assertIn("CMP_EQ(_fa, _fb)) ? 0x40u", out)
        self.assertIn("CMP_B(_fa, _fb)) ? 0x1u", out)
        self.assertIn("/* flags for pushfd */\nPUSH32(esp, recomp_pushfd());", out)

    def test_popfd_becomes_the_setter_and_jcc_reads_g_eflags(self):
        cmp_ = _insn("cmp", [_reg("eax"), _reg("ebx")])
        je = _insn("je", addr=4)
        je.jump_target = 0x40
        _, out, state = _lift_block([cmp_, _insn("popfd"), je])
        self.assertIn("recomp_popfd(_efl);", out)
        self.assertIn("if ((g_eflags & 0x40u) /* eflags */)", out)
        self.assertNotIn("CMP_EQ", out.split("popfd")[-1])

    def test_popfd_flag_state_crosses_a_block(self):
        _, _, state = _lift_block([_insn("popfd")])
        self.assertEqual(state, ("eflags", []))
        jl = _insn("jl")
        jl.jump_target = 0x80
        stmts, _ = lift_basic_block(Lifter(), BasicBlock(0, [jl]),
                                    flag_state=state)
        self.assertIn("(((g_eflags >> 7) & 1u) != ((g_eflags >> 11) & 1u))",
                      stmts[0])

    def test_pushfd_after_popfd_takes_every_flag_from_g_eflags(self):
        _, out, _ = _lift_block([_insn("popfd"), _insn("pushfd")])
        self.assertIn("recomp_pushfd()", out)
        self.assertNotIn("g_eflags = (g_eflags", out)

    def test_popfd_feeds_cf_when_the_function_keeps_one(self):
        _, out, _ = _lift_block([_insn("popfd")], needs_cf=True)
        self.assertIn("_cf = (int)(g_eflags & 1u);", out)

    def test_pushfd_after_stc_pushes_the_carry(self):
        lifter, out, state = _lift_block([_insn("stc"), _insn("pushfd")],
                                         needs_cf=True)
        self.assertEqual(state, ("__cf_only", []))
        self.assertIn("_cf = 1; /* stc */", out)
        self.assertIn("g_eflags = (g_eflags & ~0x1u) | ((_cf) ? 0x1u : 0u)", out)
        # The flags stc does not set are not rebuilt, and the site says so.
        self.assertIn('RECOMP_UNIMPL("pushfd: PF,ZF,SF,OF not rebuilt after '
                      'stc/clc/cmc, kept from g_eflags"', out)
        self.assertIn("pushfd (flags)", lifter.unimplemented)

    def test_pushfd_after_popfd_then_stc_has_nothing_missing(self):
        # popfd put PF/ZF/SF/OF in g_eflags; stc only replaces CF.
        lifter, out, _ = _lift_block(
            [_insn("popfd"), _insn("stc"), _insn("pushfd")], needs_cf=True)
        self.assertIn("g_eflags = (g_eflags & ~0x1u) | ((_cf) ? 0x1u : 0u)", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("pushfd (flags)", lifter.unimplemented)

    def test_pushfd_with_no_tracked_setter_says_so(self):
        # Function entry: nothing tracked, so the image is g_eflags as it is.
        # The site says so in a comment; no RECOMP_UNIMPL, since the CPUID
        # probe starts this way (test_no_unimpl_left).
        lifter, out, _ = _lift_block([_insn("pushfd")])
        self.assertNotIn("g_eflags = (g_eflags", out)
        self.assertIn("/* pushfd: no tracked flag setter, pushes g_eflags as "
                      "last written */", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("pushfd (flags)", lifter.unimplemented)

    def test_jb_after_clc_reads_cf(self):
        jb = _insn("jb")
        jb.jump_target = 0x40
        _, out, _ = _lift_block([_insn("clc"), jb], needs_cf=True)
        self.assertIn("if (_cf)", out)

    def test_pushfd_marks_flags_its_setter_cannot_rebuild(self):
        # cmp/sub/add/and/or/xor rebuild every flag
        # (test_lifter_pushfd_arith.py); bt defines only CF.
        bt = _insn("bt", [_reg("eax"), _reg("ebx")])
        lifter, out, _ = _lift_block([bt, _insn("pushfd")], needs_cf=True)
        self.assertIn('RECOMP_UNIMPL("pushfd: PF,ZF,SF,OF not rebuilt after bt, '
                      'kept from g_eflags"', out)
        self.assertIn("pushfd (flags)", lifter.unimplemented)

    def test_cpuid_writes_all_four_and_keeps_the_flags(self):
        out = "".join(Lifter().lift_instruction(_insn("cpuid")))
        self.assertIn("xbox_Cpuid(eax, ecx, _id);", out)
        for reg, n in (("eax", 0), ("ebx", 1), ("ecx", 2), ("edx", 3)):
            self.assertIn(f"{reg} = _id[{n}];", out)
        self.assertIn("cpuid", _EFLAGS_PRESERVE)
        cmp_ = _insn("cmp", [_reg("eax"), _reg("ebx")])
        je = _insn("je")
        je.jump_target = 0x40
        _, out, _ = _lift_block([cmp_, _insn("cpuid"), je])
        self.assertIn("CMP_EQ(_fa, _fb)", out.split("cpuid */")[-1])


HARNESS = r"""
#include "recomp_types.h"
#include <stdio.h>
#include <string.h>
static uint8_t g_mem[0x10000];
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS int g_df = 0;
RECOMP_TLS uint32_t g_eflags = 0x00000202u;
RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; }
/* xbox_Cpuid reads RECOMP_CPUID_NO_SIMD; the test pins its value per build. */
#define getenv(name) ((void)(name), (const char *)CPUID_NO_SIMD_ENV)
%(cpuid)s

static uint32_t eax, ebx, ecx, edx, esi, esp, ebp;
static void probe(void)
{
    uint32_t _fa = 0, _fb = 0; int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
%(body)s
}

int main(void)
{
    uint32_t r[4], v, w;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    esp = 0x8000; ebp = 0x1234; ecx = 0x11111111u; ebx = 0x22222222u;
    esi = 0x33333333u;
    MEM32(esp - 4) = 0xDEADBEEFu; esp -= 4;     /* return address */
    probe();
    printf("probe eax=%%u ebx=%%08X esi=%%08X esp=%%04X\n", eax, ebx, esi, esp);

    /* The ID bit, both ways, plus bit 1 and the read-only bits. */
    esp = 0x8000;
    PUSH32(esp, recomp_pushfd()); POP32(esp, v);
    PUSH32(esp, v ^ 0x00200000u); POP32(esp, w); recomp_popfd(w);
    PUSH32(esp, recomp_pushfd()); POP32(esp, w);
    printf("id %%08X\n", v ^ w);
    recomp_popfd(0xFFFFFFFFu);
    printf("all %%08X df=%%d\n", recomp_pushfd(), g_df);
    recomp_popfd(0);
    printf("none %%08X df=%%d\n", recomp_pushfd(), g_df);
    g_df = 1;
    printf("df %%08X\n", recomp_pushfd());

    for (v = 0; v < 4; v++) {
        xbox_Cpuid(v, 0, r);
        printf("leaf%%u %%08X %%08X %%08X %%08X\n", v, r[0], r[1], r[2], r[3]);
    }
    xbox_Cpuid(0x80000000u, 0, r);
    printf("ext %%08X %%08X %%08X %%08X\n", r[0], r[1], r[2], r[3]);
    xbox_Cpuid(0, 0, r);
    {
        char vendor[13];
        memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4);
        memcpy(vendor + 8, &r[2], 4); vendor[12] = 0;
        printf("vendor %%s\n", vendor);
    }
    return 0;
}
"""


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(_cc(), "no C compiler")
class RuntimeTest(unittest.TestCase):
    @classmethod
    def _run(cls, no_simd_env):
        body, _ = _probe_c()
        src = HARNESS % {"cpuid": _cpuid_c(), "body": body}
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
            open(c, "w").write(src)
            subprocess.run([_cc(), "-std=c11", "-O1", "-Wall", "-I", RUNTIME,
                            "-Wno-unused-label", "-Wno-unused-variable",
                            "-Wno-parentheses-equality",
                            f"-DCPUID_NO_SIMD_ENV={no_simd_env}", "-o", exe, c],
                           check=True)
            out = subprocess.run([exe], check=True, capture_output=True,
                                 text=True).stdout
        return dict(line.split(" ", 1) for line in out.splitlines())

    @classmethod
    def setUpClass(cls):
        cls.simd = cls._run("NULL")
        cls.masked = cls._run('"1"')

    def test_probe_restores_registers_and_stack(self):
        self.assertEqual(self.simd["probe"],
                         "eax=1 ebx=22222222 esi=33333333 esp=8000")

    def test_probe_sees_no_mmx_when_opted_out(self):
        self.assertEqual(self.masked["probe"],
                         "eax=0 ebx=22222222 esi=33333333 esp=8000")

    def test_id_bit_is_writable(self):
        self.assertEqual(self.masked["id"], "00200000")

    def test_popfd_masks_to_the_writable_bits_and_sets_bit_1(self):
        self.assertEqual(self.masked["all"], "00247FD7 df=1")
        self.assertEqual(self.masked["none"], "00000002 df=0")

    def test_pushfd_takes_df_from_g_df(self):
        # After popfd(0), std: DF is the only flag set, beside bit 1.
        self.assertEqual(self.masked["df"], "00000402")

    def test_cpuid_leaves(self):
        self.assertEqual(self.masked["vendor"], "GenuineIntel")
        self.assertEqual(self.masked["leaf0"].split()[0], "00000002")
        self.assertEqual(self.simd["leaf1"],
                         "00000683 00000000 00000000 0383F9FF")
        self.assertEqual(self.masked["leaf2"],
                         "03020101 00000000 00000000 0C040800")
        # Above the max leaf, extended included: the highest basic leaf.
        self.assertEqual(self.masked["leaf3"], self.masked["leaf2"])
        self.assertEqual(self.masked["ext"], self.masked["leaf2"])

    def test_no_simd_masks_mmx_fxsr_and_sse(self):
        # With MMX goes everything that implies it: FXSR and SSE.
        self.assertEqual(self.masked["leaf1"],
                         "00000683 00000000 00000000 0003F9FF")


if __name__ == "__main__":
    unittest.main()
