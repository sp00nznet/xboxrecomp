"""A result-setter's flags, read after something overwrote its destination.

and/or/xor, add/sub, adc/sbb, neg and the shifts all WRITE their destination,
and the jcc that reads their flags can be several instructions -- or several
basic blocks -- later. The condition was rebuilt at the branch by re-reading
that destination, so anything in between could change the answer: a mov, a
pop, a lea, a reloaded loop pointer.

    and eax, 0x0F        ; ZF answers "were the low four bits clear"
    mov eax, 0x99        ; the answer is no longer obtainable from eax
    jne taken

x86 branches on the flags the `and` left, so at eax = 0x10 the result is zero
and `jne` is NOT taken, whatever the `mov` put there afterwards. Reading live
eax asks about 0x99, which is nonzero, and takes the branch every time.

inc and dec already avoid this -- _make_condition says why, "Result at the
flag-setting instruction, before later MOVs" -- by publishing the result into
_fa next to the write. The rest of the family had the identical hole, and this
is that rule applied to it.

The sweep compiles the lifter's own statements for `and / mov / jcc` at 8, 16
and 32 bits and runs them against x86's answer computed independently. The
negative control substitutes the pre-fix expression back and requires the
sweep to catch it.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, _make_condition, lift_basic_block

# (width, destination register, how C reads it back, the mask the `and` uses)
_WIDTHS = ((1, "al", "LO8(eax)", 0x0F),
           (2, "ax", "LO16(eax)", 0x0FFF),
           (4, "eax", "eax", 0x0000000F))

_VALUES = (0x00000000, 0x00000001, 0x0000000F, 0x00000010, 0x0000007F,
           0x00000080, 0x000000FF, 0x00001000, 0x0000FFFF, 0x7FFFFFFF,
           0x80000000, 0xFFFFFFFF, 0xDEADBE80, 0x12345678)

# Deliberately nonzero and positive, so a condition that reads it instead of
# the result gets `jne` taken and `js` not taken -- the opposite of the right
# answer over a good half of the sweep.
_CLOBBER = 0x99

_PRELUDE = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax, ecx;
static uint32_t _fa, _fb;
static int32_t _fas, _fbs;
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
'''


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _lift(mnemonic, operands, op_str=""):
    insn = Instruction(0, 3, mnemonic, op_str, "", operands=operands)
    return " ".join(Lifter().lift_instruction(insn))


def _cc():
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\Program Files\LLVM\bin\clang.exe").exists():
        cc = r"C:\Program Files\LLVM\bin\clang.exe"
    if not cc:
        pytest.skip("C compiler unavailable")
    return cc


def _source(rewrite=None):
    cases, calls = [], []
    for width, lo, dest_read, mask in _WIDTHS:
        write = _lift("and", [_reg(lo), _imm(mask)], f"{lo}, {mask:#x}")
        clobber = _lift("mov", [_reg(lo), _imm(_CLOBBER)],
                        f"{lo}, {_CLOBBER:#x}")
        sign = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[width]
        for jcc, op in (("je", "=="), ("jne", "!="),
                        ("js", "<"), ("jns", ">=")):
            made = _make_condition(jcc, "and", [_reg(lo), _imm(mask)])
            if made is None:
                continue
            cond = made[0] if rewrite is None else rewrite(made[0], dest_read)
            ref = (f"(({sign})(uint32_t)(a & {mask:#x}u) {op} 0)"
                   if jcc in ("js", "jns")
                   else f"((uint32_t)(a & {mask:#x}u) {op} 0)")
            name = f"case_{width}_{jcc}"
            cases.append(
                f"static int {name}(uint32_t a) {{\n"
                f"    eax = a; ecx = 1;\n"
                f"    {write}\n"
                f"    {clobber}\n"
                f"    return ({cond}) ? 1 : 0;\n"
                f"}}\n"
                f"static int ref_{name}(uint32_t a) {{ return {ref} ? 1 : 0; }}")
            calls.append(
                f"    for (unsigned i = 0; i < NV; i++) {{\n"
                f"        uint32_t a = vals[i];\n"
                f"        if ({name}(a) != ref_{name}(a)) {{ failures++;\n"
                f'            printf("and+mov+{jcc} w{width} a=0x%08X\\n", a); }}\n'
                f"    }}")
    values = ", ".join("0x%08Xu" % v for v in _VALUES)
    return (_PRELUDE + "\n".join(cases) + f'''
static const uint32_t vals[] = {{ {values} }};
#define NV (sizeof vals / sizeof vals[0])
int main(void) {{
    int failures = 0;
''' + "\n".join(calls) + '''
    return failures ? 1 : 0;
}
''')


def _build_and_run(source):
    cc = _cc()
    with tempfile.TemporaryDirectory(prefix="result-clobber-") as tmp:
        c = Path(tmp) / "t.c"
        c.write_text(source)
        exe = Path(tmp) / "t.exe"
        built = subprocess.run([cc, "-w", "-O2", str(c), "-o", str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr + "\n" + source
        return subprocess.run([str(exe)], capture_output=True, text=True)


def test_the_branch_survives_a_clobbered_destination():
    ran = _build_and_run(_source())
    assert ran.returncode == 0, ran.stdout + ran.stderr


def test_negative_control_reading_the_destination_fails():
    """Put the pre-fix expression back; the sweep must catch it."""
    src = _source(rewrite=lambda cond, dest: cond.replace("_fa", dest))
    ran = _build_and_run(src)
    assert ran.returncode == 1, (
        "reading the live destination should disagree with x86 once a mov "
        "has overwritten it, and did not:\n" + ran.stdout + ran.stderr)


@pytest.mark.parametrize("mnemonic,operands", (
    ("and", [_reg("eax"), _imm(0x0F)]),
    ("or", [_reg("eax"), _imm(1)]),
    ("xor", [_reg("eax"), _imm(0x0F)]),
    ("add", [_reg("eax"), _imm(1)]),
    ("sub", [_reg("eax"), _imm(1)]),
    ("neg", [_reg("eax")]),
    ("adc", [_reg("eax"), _imm(1)]),
    ("sbb", [_reg("eax"), _imm(1)]),
    ("shl", [_reg("eax"), _reg("cl")]),
    ("shr", [_reg("eax"), _reg("cl")]),
    ("sar", [_reg("eax"), _reg("cl")]),
))
def test_every_setter_publishes_what_its_condition_reads(mnemonic, operands):
    assert "_fa =" in _lift(mnemonic, operands)


def test_a_condition_never_re_reads_the_destination():
    for width, lo, dest_read, mask in _WIDTHS:
        for mnemonic, ops in (("and", [_reg(lo), _imm(mask)]),
                              ("sub", [_reg(lo), _imm(1)]),
                              ("neg", [_reg(lo)]),
                              ("sar", [_reg(lo), _reg("cl")])):
            for jcc in ("je", "jne", "js", "jns"):
                made = _make_condition(jcc, mnemonic, ops)
                if made is None:
                    continue
                assert dest_read not in made[0], (mnemonic, jcc, made[0])
