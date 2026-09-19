"""SHLD/SHRD must follow x86's count rules, including a count of zero.

The count is masked to five bits by the hardware, and a masked count of zero
leaves the destination alone. The emitted expression honoured neither, and the
zero case produced a wrong VALUE rather than merely undefined behaviour:

    dst = (dst >> cnt) | (src << (32 - cnt))

with cnt == 0 shifts left by the operand's full width. C leaves that
undefined, and where the host takes the shift amount modulo the width it
shifts by nothing and yields `src`, so the write lands as `dst | src` for an
instruction that should not have written at all.

The sweep compiles the lifter's own statement and runs it against a reference
computed in 64-bit arithmetic, over every count from 0 to 40 -- zero, the
ordinary range, the five-bit wrap at 32, and past it -- for every ordered pair
of a set of values chosen so the two answers differ.

The negative control substitutes the previous expression back and requires the
sweep to catch it, and to catch it at a count of zero specifically; without
that the suite could pass because the sweep is blind.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from .disasm import Instruction, Operand
from .lifter import Lifter

_VALUES = (0x00000000, 0x00000001, 0x80000000, 0xFFFFFFFF,
           0x12345678, 0xDEADBEEF, 0x0000FFFF, 0xFFFF0000)

_PRELUDE = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax, edx, ecx;
#define LO8(v) ((uint8_t)(v))
'''

# x86: mask the count to five bits; a masked count of zero does nothing.
_REFERENCE = {
    "shld": "    return (uint32_t)(((uint64_t)d << n) | ((uint64_t)s >> (32 - n)));",
    "shrd": "    return (uint32_t)((d >> n) | ((uint64_t)s << (32 - n)));",
}

# What the lifter emitted before this change.
_OLD = {
    "shld": "eax = (eax << (LO8(ecx))) | (edx >> (32 - (LO8(ecx))));",
    "shrd": "eax = (eax >> (LO8(ecx))) | (edx << (32 - (LO8(ecx))));",
}


def _reg(name):
    return Operand(type="reg", reg=name)


def _emitted(mnemonic):
    ops = [_reg("eax"), _reg("edx"), _reg("cl")]
    insn = Instruction(0, 3, mnemonic, "eax, edx, cl", "", operands=ops)
    return " ".join(Lifter().lift_instruction(insn))


def _source(mnemonic, body):
    values = ", ".join("0x%08Xu" % v for v in _VALUES)
    return _PRELUDE + '''
static uint32_t emitted(uint32_t d, uint32_t s, uint32_t c)
{
    eax = d; edx = s; ecx = c;
    %s
    return eax;
}

static uint32_t reference(uint32_t d, uint32_t s, uint32_t c)
{
    uint32_t n = c & 31u;
    if (n == 0) return d;
%s
}

int main(void)
{
    static const uint32_t vals[] = { %s };
    int failures = 0;
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++)
        for (unsigned j = 0; j < sizeof vals / sizeof vals[0]; j++)
            for (uint32_t c = 0; c <= 40; c++) {
                uint32_t got = emitted(vals[i], vals[j], c);
                uint32_t want = reference(vals[i], vals[j], c);
                if (got != want) {
                    failures++;
                    if (failures <= 12)
                        printf("d=0x%%08X s=0x%%08X c=%%u got 0x%%08X want 0x%%08X\\n",
                               vals[i], vals[j], c, got, want);
                }
            }
    return failures ? 1 : 0;
}
''' % (body, _REFERENCE[mnemonic], values)


def _build_and_run(source):
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\Program Files\LLVM\bin\clang.exe").exists():
        cc = r"C:\Program Files\LLVM\bin\clang.exe"
    if not cc:
        pytest.skip("C compiler unavailable")
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / "test.c"
        src.write_text(source)
        exe = Path(temp) / "test.exe"
        built = subprocess.run([cc, str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        return subprocess.run([str(exe)], capture_output=True, text=True)


@pytest.mark.parametrize("mnemonic", ("shld", "shrd"))
def test_matches_x86_at_every_count(mnemonic):
    ran = _build_and_run(_source(mnemonic, _emitted(mnemonic)))
    assert ran.returncode == 0, ran.stdout + ran.stderr


@pytest.mark.parametrize("mnemonic", ("shld", "shrd"))
def test_the_count_is_masked_and_zero_is_a_no_op(mnemonic):
    body = _emitted(mnemonic)
    assert "& 31u" in body
    assert "_c &&" in body


@pytest.mark.parametrize("mnemonic", ("shld", "shrd"))
def test_negative_control_the_old_expression_is_caught(mnemonic):
    ran = _build_and_run(_source(mnemonic, _OLD[mnemonic]))
    assert ran.returncode == 1, (
        "the unguarded expression should disagree with x86:\n"
        + ran.stdout + ran.stderr)
    assert " c=0 " in ran.stdout, (
        "it should disagree at a count of zero:\n" + ran.stdout)
