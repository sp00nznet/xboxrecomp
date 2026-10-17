"""`test a, b` lifts as `cmp (a & b), 0`, and cmp/test joins keep their branch.

One title's state machine joins `cmp eax, 0` and `test esi, 0x100` in
front of one `je`. The merge refused the pair, the je
compiled to the never-assigned `_flags`, and a wait-for-A state advanced on
every frame, freezing the player once every four. Normalising every test to a
compare against zero makes both edges the same operation.

The sweep compiles the lifter's own snapshot and condition and checks every
condition code against x86's definitions: after a test, ZF, SF and PF come
from a & b at the operand width and CF and OF are 0; after a cmp, OF is the
signed overflow of a - b.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from tools.recomp import config
from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter, _make_condition, normalise_zero_test
from tools.recomp.translator import FunctionTranslator, _merge_flag_states

JCCS = ("je", "jne", "jb", "jae", "jbe", "ja", "jl", "jge", "jle", "jg",
        "js", "jns", "jp", "jnp", "jo", "jno")

# x86, from the flags the C reference computes.
REFERENCE = {
    "je": "zf", "jne": "!zf", "jb": "cf", "jae": "!cf",
    "jbe": "(cf || zf)", "ja": "(!cf && !zf)",
    "jl": "(sf != of)", "jge": "(sf == of)",
    "jle": "(zf || sf != of)", "jg": "(!zf && sf == of)",
    "js": "sf", "jns": "!sf", "jp": "pf", "jnp": "!pf",
    "jo": "of", "jno": "!of",
}

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
static uint32_t eax, ebx, _fa, _fb;
static int32_t _fas, _fbs;
static int _cf;
static uint8_t mem[64];
#define MEM8(a)  (*(uint8_t  *)(mem + (a)))
#define MEM16(a) (*(uint16_t *)(mem + (a)))
#define MEM32(a) (*(uint32_t *)(mem + (a)))
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
/* As templates/runtime/recomp_types.h. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
static int recomp_parity8(uint32_t x) {
    x &= 0xFF; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return !(x & 1);
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))
static const uint32_t VS[] = {
    0u, 1u, 2u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0x7FFFu, 0x8000u, 0xFFFFu,
    0x10000u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu,
    0xDEADBEEFu, 0xA5A5A5A5u, 0x5A5A5A5Au, 0xCAFEBABEu
};
#define N (sizeof(VS) / sizeof(VS[0]))
static int fails;
"""

REG = {1: ("al", "bl"), 2: ("ax", "bx"), 4: ("eax", "ebx")}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}


def _reg(name, size):
    return Operand(type="reg", reg=name, mem_size=size)


def _snapshot(mnemonic, ops):
    insn = Instruction(0, 2, mnemonic, "", "", operands=ops)
    lifter = Lifter()
    lifter.needs_cf = True
    return " ".join(lifter.lift_instruction(insn))


def _case(fn, mnemonic, ops, width, b_imm=None):
    """One C function sweeping (a, b) over VS for every condition code."""
    snap = _snapshot(mnemonic, ops)
    kind, nops = normalise_zero_test(mnemonic, ops)
    mask, top = MASK[width], width * 8 - 1
    lines = [f"static void {fn}(void) {{",
             "  for (unsigned i = 0; i < N; i++) for (unsigned j = 0; j < N; j++) {",
             "    uint32_t a = VS[i] & %#xu, b = %s;" % (
                 mask, f"(uint32_t)({b_imm}) & {mask:#x}u" if b_imm is not None
                 else f"VS[j] & {mask:#x}u"),
             "    if (j && %d) continue;" % (b_imm is not None),
             "    eax = VS[i]; ebx = VS[j]; MEM32(0) = VS[i];",
             "    if (%d == 1) eax = (eax & ~0xFFu) | a;" % width,
             f"    {snap}"]
    if mnemonic == "test":
        lines += [f"    uint32_t r = (a & b) & {mask:#x}u;",
                  "    int zf = r == 0, sf = (r >> %d) & 1, pf = recomp_parity8(r);"
                  % top,
                  "    int cf = 0, of = 0;"]
    else:
        lines += [f"    uint32_t r = (a - b) & {mask:#x}u;",
                  "    int zf = r == 0, sf = (r >> %d) & 1, pf = recomp_parity8(r);"
                  % top,
                  "    int cf = a < b;",
                  "    int of = (((a ^ b) & (a ^ r)) >> %d) & 1;" % top]
    for jcc in JCCS:
        got = _make_condition(jcc, kind, nops)
        assert got is not None, (fn, jcc)
        lines.append(
            f'    if (!!({got[0]}) != !!({REFERENCE[jcc]})) {{ fails++; '
            f'printf("{fn} {jcc} a=%#x b=%#x\\n", a, b); }}')
    lines += ["  }", "}"]
    return "\n".join(lines) + "\n"


CASES = [
    ("test_rr8", "test", [_reg("al", 1), _reg("bl", 1)], 1, None),
    ("test_rr16", "test", [_reg("ax", 2), _reg("bx", 2)], 2, None),
    ("test_rr32", "test", [_reg("eax", 4), _reg("ebx", 4)], 4, None),
    ("test_ri32", "test", [_reg("esi" if False else "eax", 4),
                           Operand(type="imm", imm=0x100)], 4, "0x100"),
    ("test_ri32_neg", "test", [_reg("eax", 4),
                               Operand(type="imm", imm=-256)], 4, "-256"),
    ("test_ri8_sign", "test", [_reg("al", 1),
                               Operand(type="imm", imm=0x80)], 1, "0x80"),
    ("test_mi8", "test", [Operand(type="mem", mem_disp=0, mem_size=1),
                          Operand(type="imm", imm=0x81)], 1, "0x81"),
    ("test_mi16", "test", [Operand(type="mem", mem_disp=0, mem_size=2),
                           Operand(type="imm", imm=-2)], 2, "-2"),
    ("cmp_rr8", "cmp", [_reg("al", 1), _reg("bl", 1)], 1, None),
    ("cmp_rr16", "cmp", [_reg("ax", 2), _reg("bx", 2)], 2, None),
    ("cmp_rr32", "cmp", [_reg("eax", 4), _reg("ebx", 4)], 4, None),
]


def _build_and_run(cases):
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    code = PRELUDE + "".join(_case(*c) for c in cases)
    code += ("int main(void) {\n"
             + "".join(f"  {c[0]}();\n" for c in cases)
             + '  printf("fails %d\\n", fails);\n  return fails != 0;\n}\n')
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / "t.c"
        src.write_text(code)
        exe = Path(temp) / "t"
        built = subprocess.run([cc, "-O1", "-w", str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr + code
        return subprocess.run([str(exe)], capture_output=True, text=True)


def test_every_condition_after_test_and_cmp_matches_x86():
    ran = _build_and_run(CASES)
    assert ran.returncode == 0, ran.stdout


def test_a_test_against_an_immediate_becomes_a_compare_of_the_and():
    kind, ops = normalise_zero_test(
        "test", [_reg("esi", 4), Operand(type="imm", imm=0x100)])
    assert kind == "cmp"
    assert ops[0].type == "expr" and ops[0].mem_size == 4
    assert ops[1].type == "imm" and ops[1].imm == 0
    out = _snapshot("test", [_reg("esi", 4), Operand(type="imm", imm=0x100)])
    assert "_fa = (uint32_t)((esi) & (0x100)) & 0xFFFFFFFFu" in out, out
    assert "_fb = (uint32_t)(0)" in out
    assert "test esi, 0x100 (32-bit)" in out
    assert "_cf = 0;" in out


def test_cmp_and_test_of_one_width_merge_as_a_compare():
    from_cmp = ("cmp", [_reg("eax", 4), Operand(type="imm", imm=0, mem_size=4)])
    from_test = normalise_zero_test(
        "test", [_reg("esi", 4), Operand(type="imm", imm=0x100)])
    merged = _merge_flag_states([from_cmp, from_test])
    assert merged is not None and merged[0] == "cmp"


def test_two_tests_of_different_operands_merge():
    one = normalise_zero_test("test", [_reg("eax", 4), _reg("ebx", 4)])
    two = normalise_zero_test("test", [_reg("ecx", 4),
                                       Operand(type="imm", imm=4)])
    assert _merge_flag_states([one, two])[0] == "cmp"


def test_mixed_widths_do_not_merge():
    """`cmp al, 0x80` and `cmp eax, 0x80` with al = 0x80: SF is 0 at 8 bits
    and 1 at 32, so one snapshot cannot answer js for both edges. The states
    stay apart and the join is evaluated on each edge instead (see
    test_mixed_widths_join_on_each_edge_even_for_sign)."""
    narrow = ("cmp", [_reg("al", 1), Operand(type="imm", imm=0x80)])
    wide = ("cmp", [_reg("eax", 4), Operand(type="imm", imm=0x80)])
    assert _merge_flag_states([narrow, wide]) is None


def test_a_compare_never_merges_with_an_arithmetic_setter():
    cmp = ("cmp", [_reg("eax", 4), Operand(type="imm", imm=0, mem_size=4)])
    dec = ("dec", [_reg("eax", 4)])
    assert _merge_flag_states([cmp, dec]) is None


BASE = 0x10000


def _translate(image):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='test-as-cmp')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_the_cmp_test_join_keeps_its_branch():
    # test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: test esi,0x100;
    # join: je out; nop; out: ret -- the shape seen in the title.
    code = _translate(bytes.fromhex(
        '85c97405' '83f800' 'eb06' 'f7c600010000' '7401' '90' 'c3'))
    assert '_flags /* je' not in code, code
    assert 'CMP_EQ(_fa, _fb)' in code


def test_a_mixed_width_join_keeps_setne_and_falls_back_for_sets():
    # test ecx,ecx; jz alt; cmp eax,1; jmp join; alt: cmp bl,1;
    # join: setne al; sets dl; ret
    image = bytes.fromhex('85c97405' '83f801' 'eb03' '80fb01'
                          '0f95c0' '0f98c2' 'c3')
    code = _translate(image)
    assert '_flags /* setne */' not in code, code
    assert '_flags /* sets */' in code, code


def test_mixed_widths_join_on_each_edge_even_for_sign():
    # test ecx,ecx; jz alt; cmp eax,0x80; jmp join; alt: cmp al,0x80;
    # join: js out; nop; out: ret. Each edge sets the join's variable from
    # its own compare, so js reads right at either width.
    code = _translate(bytes.fromhex('85c97407' '3d80000000' 'eb02' '3c80'
                                    '7801' '90' 'c3'))
    assert 'if (_jf_0001000D /* js' in code, code
    assert 'if (_flags' not in code, code
