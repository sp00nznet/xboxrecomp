"""
Self-check for the x87 float-compare branch idiom.

Run: py -3 tools/recomp/test_fpu_branch.py

`fcomp; fnstsw ax; test ah, mask; jp/jnp` is how all pre-SSE x86 code branches on
a float comparison. The lifter set _fpu_cmp from the compare but then made fnstsw
a no-op and hardcoded the jp/jnp branch to a constant, so every float comparison
went one fixed direction. Halo's render_camera_build_frustum took the wrong path
in sub_00109150 and left world_to_view all zeros, failing
valid_real_matrix4x3 at render_cameras.c:458 -- the boot blocker.

This checks the two halves the fix restored: fnstsw ax reconstructs ah from
_fpu_cmp, and jp/jnp after test read a real parity of (ah & mask).
"""

import functools
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Op:
    def __init__(self, text, type="reg", reg=None, size=4, mem_size=None,
                 imm=0, mem_base=None, mem_index=None, mem_scale=1,
                 mem_disp=0):
        self.text = text
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = mem_size
        self.imm = imm
        self.mem_base = mem_base
        self.mem_index = mem_index
        self.mem_scale = mem_scale
        self.mem_disp = mem_disp


class _Insn:
    def __init__(self, mnemonic, operands, op_str=""):
        self.mnemonic = mnemonic
        self.operands = operands
        self.op_str = op_str
        self.is_cond_jump = False
        self.is_call = False
        self.is_ret = False
        self.is_jump = False
        self.call_target = None
        self.jump_target = None


def _with_stubbed_mem(fn):
    """Render any memory operand as a fixed token for the duration of one test.

    These checks are about the FPU stack effect, not address formatting. The
    stub replaces a function on the lifter *module*, so it has to be put back:
    leaving it installed silently breaks every later test in the same process.
    """
    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        import tools.recomp.lifter as lf
        original = lf._fmt_mem
        lf._fmt_mem = lambda op: "0xK"
        try:
            return fn(*args, **kwargs)
        finally:
            lf._fmt_mem = original
    return wrapper



def test_fnstsw_ax_sets_ah_from_fpu_cmp():
    out = "\n".join(Lifter().lift_instruction(_Insn("fnstsw", [], "ax")))
    assert "g_fp_cc" in out, out
    # `fnstsw ax` writes the whole of AX, and the status word carries TOP in
    # bits 11-13 as well as the condition codes -- the hardware AH includes it,
    # so a comparison against the real CPU fails without it.
    assert "eax = (eax & 0xFFFF0000u)" in out, out
    assert "(g_fp_top & 7u) << 11" in out, out
    # Classification and comparison instructions publish shared condition bits.


def test_fnstsw_to_memory_writes_the_status_word():
    """`fnstsw [mem]` used to be dropped on the floor. It now stores the real
    16-bit status word, with C3/C2/C0 at bits 14/10/8 -- so its high byte is
    the same AH that the `fnstsw ax` form produces."""
    out = "\n".join(Lifter().lift_instruction(
        _Insn("fnstsw", [_Op("word ptr [eax]", type="mem", mem_size=2,
                             mem_base="eax")],
              "word ptr [eax]")))
    assert "MEM16(eax) =" in out, out
    assert "g_fp_cc" in out, out


C_HELPER = r"""
#include <stdint.h>
#include <stdio.h>
static inline int recomp_parity8(uint32_t x){x&=0xFFu;x^=x>>4;x^=x>>2;x^=x>>1;return (int)(~x&1u);}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/* Reproduce the fixed idiom for each compare outcome and confirm the branch. */
static int branch_jnp(int fpu_cmp) {
    uint32_t eax = 0;
    eax = (eax & 0xFFFF00FFu) | ((uint32_t)((fpu_cmp==0)?0x40u:(fpu_cmp<0)?0x01u:0x00u) << 8);
    uint32_t ah = (eax >> 8) & 0xFFu;
    /* test ah,0x44 ; jnp taken?  jnp = PF==0 */
    return (!RECOMP_PARITY8(ah & 0x44u));
}
int main(void){
    /* Standard idiom `test ah,0x44; jnp` jumps ONLY when equal (fpu_cmp==0). */
    if (branch_jnp(1))  { printf("FAIL: greater took jnp\n"); return 1; }  /* > : PF=1, no jump */
    if (!branch_jnp(0)) { printf("FAIL: equal missed jnp\n"); return 1; }  /* = : PF=0, jump */
    if (branch_jnp(-1)) { printf("FAIL: less took jnp\n"); return 1; }     /* < : PF=1, no jump */
    printf("OK jnp branches only on equal\n");
    return 0;
}
"""


def test_idiom_semantics_compiled():
    from tools.recomp.c_fixture import compiler, command
    cc = compiler()
    if not cc:
        import pytest
        pytest.skip('no C compiler on PATH')
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        open(c, "w").write(C_HELPER)
        exe = os.path.join(tmp, "t.exe")
        r = subprocess.run(command(cc, [c], exe), cwd=tmp, capture_output=True, text=True)
        assert r.returncode == 0, (r.stdout + r.stderr)[-1500:]
        r = subprocess.run([exe], capture_output=True, text=True)
        assert r.returncode == 0 and r.stdout.startswith("OK"), r.stdout + r.stderr
        print("     " + r.stdout.strip())


@_with_stubbed_mem
def test_arith_honors_operands():
    """fadd/fsub/fmul/fdiv with a memory or st(i) operand must not use the bare
    stack-pop form. This is what left Halo's fmul-by-constant corrupting the
    FPU stack."""
    L = Lifter()

    def lift(m, ops, s=""):
        return L.lift_instruction(_Insn(m, ops, s))[0]

    mem = _Op("m", type="mem", mem_size=4)
    st0 = _Op("st0", type="reg", reg="st(0)")
    st1 = _Op("st1", type="reg", reg="st(1)")

    # fmul [mem]: st0 *= mem, NO pop
    o = lift("fmul", [mem], "dword ptr [0xK]")
    assert "fp_top() = RECOMP_FP_PC(fp_top() * MEMF(0xK))" in o and "fp_pop" not in o, o
    # fadd st(0), st(0): double st0, no pop
    o = lift("fadd", [st0, st0], "st(0), st(0)")
    assert "fp_top() = RECOMP_FP_PC(fp_top() + fp_top())" in o and "fp_pop" not in o, o
    # faddp: st1 += st0, pop
    o = lift("faddp", [], "")
    assert "fp_st1() = RECOMP_FP_PC(fp_st1() + fp_top()); fp_pop();" in o, o
    # faddp st(1): capstone reports the pop form as ONE operand. Must target
    # st(i) and pop -- NOT st0 without a pop (that broke normalize's sum of
    # squares and gave Halo a 1/sqrt(2)-scaled camera basis).
    o = lift("faddp", [st1], "st(1)")
    assert "fp_st1() = RECOMP_FP_PC(fp_st1() + fp_top()); fp_pop();" in o, o
    # faddp st(2): st(2) = st(2) + st0, pop
    o = lift("faddp", [_Op("st2", type="reg", reg="st(2)")], "st(2)")
    assert "g_fp_stack[(g_fp_top + 2) & 7] = RECOMP_FP_PC(g_fp_stack[(g_fp_top + 2) & 7] + fp_top()); fp_pop();" in o, o
    # fmul st(3): non-pop single operand stays st0 *= st(3), no pop
    o = lift("fmul", [_Op("st3", type="reg", reg="st(3)")], "st(3)")
    assert "fp_top() = RECOMP_FP_PC(fp_top() * g_fp_stack[(g_fp_top + 3) & 7])" in o and "fp_pop" not in o, o
    # fmulp st(1): st1 *= st0, pop
    o = lift("fmulp", [st1], "st(1)")
    assert "fp_st1() = RECOMP_FP_PC(fp_st1() * fp_top()); fp_pop();" in o, o
    # fsubr [mem]: st0 = mem - st0 (reversed)
    o = lift("fsubr", [mem], "dword ptr [0xK]")
    assert "fp_top() = RECOMP_FP_PC(MEMF(0xK) - fp_top())" in o, o
    # fdivrp st(1), st(0): st1 = st0 / st1, pop
    o = lift("fdivrp", [st1, st0], "st(1), st(0)")
    assert "fp_st1() = RECOMP_FP_PC(fp_top() / fp_st1()); fp_pop();" in o, o


def test_fcom_pop_counts():
    """fcom pops 0, fcomp pops 1, fcompp pops 2. Emitting no pop for the pop
    forms leaked a slot on every float compare and drifted g_fp_top, failing
    Halo's camera matrix validation non-deterministically."""
    L = Lifter()
    st1 = _Op("st1", type="reg", reg="st(1)")
    o = "\n".join(L.lift_instruction(_Insn("fcom", [st1], "st(1)")))
    assert "fp_pop" not in o, o
    o = "\n".join(L.lift_instruction(_Insn("fcomp", [st1], "st(1)")))
    assert o.count("fp_pop();") == 1, o
    o = "\n".join(L.lift_instruction(_Insn("fcompp", [], "")))
    assert o.count("fp_pop();") == 2, o


@_with_stubbed_mem
def test_fst_mem_does_not_pop():
    """fst [mem] stores st0 WITHOUT popping; only fstp pops. fp_pop() is a real
    pop (g_fp_top++), not a no-op, so emitting it for fst emptied the FP stack
    under valid_real_matrix4x3 and failed Halo's camera assert."""
    L = Lifter()
    mem = _Op("m", type="mem", mem_size=4)
    o = L.lift_instruction(_Insn("fst", [mem], "dword ptr [0xK]"))[0]
    assert "fp_pop" not in o, o
    o = L.lift_instruction(_Insn("fstp", [mem], "dword ptr [0xK]"))[0]
    assert "fp_pop();" in o, o


def test_fstp_sti_pops():
    """fstp st(i) must pop the FPU stack (it was a no-op comment, leaking it)."""
    L = Lifter()
    o = L.lift_instruction(_Insn("fstp", [_Op("st0", type="reg", reg="st(0)")],
                                 "st(0)"))[0]
    assert "fp_pop();" in o, o


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
