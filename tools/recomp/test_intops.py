"""The emitted C must agree with the CPU at width/count boundaries."""
import subprocess

import pytest

from tools.conformance import __main__ as compiler
from .c_fixture import command, compiler as c_compiler
from .disasm import Instruction, Operand
from .fpdiff import cases, run
from .lifter import Lifter


@pytest.mark.parametrize("group", ("shift", "product", "division", "save"))
def test_intops_native(tmp_path, group):
    if compiler._find_vcvars() is None:
        pytest.skip("32-bit MSVC required for the native differential check")
    corpus = [c for c in cases() if c["name"].startswith("intops_")]
    if group == "shift":
        corpus = [c for c in corpus if c["name"].endswith(("imm32", "cl33"))]
    elif group == "product":
        corpus = [c for c in corpus if c["name"].startswith(("intops_mul", "intops_imul"))]
    elif group == "division":
        corpus = [c for c in corpus if c["name"].startswith(("intops_div", "intops_idiv"))]
    else:
        corpus = [c for c in corpus if c["name"].startswith("intops_fnsave")]
    result = run(tmp_path, corpus)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "UNSUPPORTED" not in result.stdout, result.stdout
    assert "C4293" not in (tmp_path / "compile.txt").read_text()


def _lift_division(mnemonic, register, size):
    operand = Operand(type="reg", reg=register, mem_size=size)
    insn = Instruction(0, 2, mnemonic, f"{mnemonic} {register}", "00")
    insn.operands = [operand]
    return "\n".join(Lifter().lift_instruction(insn))


@pytest.mark.parametrize("mnemonic", ("div", "idiv"))
def test_byte_division_uses_ax_and_preserves_edx(mnemonic):
    body = _lift_division(mnemonic, "al", 1)
    assert "LO16(eax)" in body
    assert "SET_LO16(eax" in body
    assert "SET_LO16(edx" not in body
    assert "edx =" not in body


@pytest.mark.parametrize("mnemonic", ("div", "idiv"))
def test_word_division_uses_dx_ax_and_preserves_high_halves(mnemonic):
    body = _lift_division(mnemonic, "ax", 2)
    assert "LO16(edx)" in body and "LO16(eax)" in body
    assert "SET_LO16(eax" in body and "SET_LO16(edx" in body


@pytest.mark.parametrize("mnemonic, register, size", (
    ("div", "al", 1), ("idiv", "al", 1),
    ("div", "ax", 2), ("idiv", "ax", 2),
    ("div", "eax", 4), ("idiv", "eax", 4),
))
def test_divisor_is_snapshotted_before_implicit_outputs(mnemonic, register, size):
    body = _lift_division(mnemonic, register, size)
    assert body.index("_divisor =") < body.index("eax")


def _signed(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def _trunc_div(dividend, divisor):
    quotient = abs(dividend) // abs(divisor)
    if (dividend < 0) != (divisor < 0):
        quotient = -quotient
    return quotient, dividend - quotient * divisor


def _division_result(mnemonic, width, eax, edx, divisor):
    mask = (1 << width) - 1
    if width == 8:
        dividend = eax & 0xFFFF
    elif width == 16:
        dividend = ((edx & 0xFFFF) << 16) | (eax & 0xFFFF)
    else:
        dividend = (edx << 32) | eax

    divisor &= mask
    if mnemonic == "idiv":
        dividend = _signed(dividend, width * 2)
        divisor = _signed(divisor, width)
    quotient, remainder = _trunc_div(dividend, divisor)

    if mnemonic == "idiv":
        assert -(1 << (width - 1)) <= quotient < (1 << (width - 1))
    else:
        assert 0 <= quotient <= mask

    if width == 8:
        eax = ((eax & 0xFFFF0000)
               | ((remainder & 0xFF) << 8) | (quotient & 0xFF))
    elif width == 16:
        eax = (eax & 0xFFFF0000) | (quotient & 0xFFFF)
        edx = (edx & 0xFFFF0000) | (remainder & 0xFFFF)
    else:
        eax, edx = quotient & 0xFFFFFFFF, remainder & 0xFFFFFFFF
    return eax, edx


@pytest.mark.parametrize("mnemonic, register, width, eax, edx, ebx", (
    ("div", "bl", 8, 0xCAFE1234, 0x89ABCDEF, 0x00000034),
    ("idiv", "bl", 8, 0xCAFEFDA8, 0x89ABCDEF, 0x000000F9),
    ("div", "al", 8, 0xCAFE01F5, 0x89ABCDEF, 0),
    ("idiv", "al", 8, 0xCAFEFF9C, 0x89ABCDEF, 0),
    ("div", "bx", 16, 0xCAFE3456, 0x89AB0002, 0x00000123),
    ("idiv", "bx", 16, 0xCAFE7960, 0x89ABFFFE, 0x0000FEBF),
    ("div", "ax", 16, 0xCAFE0101, 0x89AB0001, 0),
    ("idiv", "ax", 16, 0xCAFEFF9C, 0x89ABFFFF, 0),
    ("div", "ebx", 32, 0x12345678, 0x00000002, 0x00010000),
    ("idiv", "ebx", 32, 0xFFF0BDC0, 0xFFFFFFFF, 0xFFFFFEBF),
    ("div", "eax", 32, 0x12345678, 0, 0),
    ("idiv", "eax", 32, 0xFFF0BDC0, 0xFFFFFFFF, 0),
))
def test_division_matches_architectural_width(
        tmp_path, mnemonic, register, width, eax, edx, ebx):
    cc = c_compiler()
    if not cc:
        pytest.skip("no C compiler on PATH")

    source_value = eax if register in ("al", "ax", "eax") else ebx
    want_eax, want_edx = _division_result(
        mnemonic, width, eax, edx, source_value)
    body = _lift_division(mnemonic, register, width // 8)
    source = f"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r) ((uint8_t)((r) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))
int main(void) {{
    uint32_t eax = UINT32_C(0x{eax:08X});
    uint32_t edx = UINT32_C(0x{edx:08X});
    uint32_t ebx = UINT32_C(0x{ebx:08X});
    {body}
    if (eax != UINT32_C(0x{want_eax:08X})
            || edx != UINT32_C(0x{want_edx:08X})) {{
        printf("eax=%08X edx=%08X\\n", eax, edx);
        return 1;
    }}
    return 0;
}}
"""
    source_path = tmp_path / "division.c"
    executable = tmp_path / "division"
    source_path.write_text(source)
    result = subprocess.run(
        command(cc, [str(source_path)], str(executable)),
        capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + "\n" + source
    result = subprocess.run(
        [str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
