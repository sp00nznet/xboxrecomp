"""A sign test after an 8- or 16-bit result reads that width's sign bit."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import pytest
from . import config
from .translator import FunctionTranslator

ALU = {'add': 0, 'or': 1, 'adc': 2, 'sbb': 3, 'and': 4, 'sub': 5, 'xor': 6}
SHIFT = {'shl': 0xE0, 'sar': 0xF8}
INPUTS = (0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
          0x7fffffff, 0x80000000, 0xffffffff, 0x1234807f)


def _encode(op, width, reg):
    prefix = b'\x66' if width == 16 else b''
    wide = width > 8
    if op in ALU:  # op reg, -1
        return prefix + bytes([0x83 if wide else 0x80, 0xC0 | ALU[op] << 3 | reg, 0xFF])
    if op == 'neg':
        return prefix + bytes([0xF7 if wide else 0xF6, 0xD8 | reg])
    return prefix + bytes([0xD1 if wide else 0xD0, SHIFT[op] | reg])


def _sign(op, width, a):
    mask = (1 << width) - 1
    sign = 1 << (width - 1)
    result = {'add': a + mask, 'adc': a + mask, 'sub': a - mask, 'sbb': a - mask,
              'and': a & mask, 'or': a | mask, 'xor': a ^ mask, 'neg': -a,
              'shl': a << 1, 'sar': (a - (1 << width) if a & sign else a) >> 1}[op]
    return int(bool(result & mask & sign))


def test_result_sign_uses_operand_width():
    cc = shutil.which('clang') or shutil.which('gcc')
    if not cc and Path(r'C:\Program Files\LLVM\bin\clang.exe').exists():
        cc = r'C:\Program Files\LLVM\bin\clang.exe'
    if not cc:
        pytest.skip('C compiler unavailable')
    code = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax,esp;
#define LO8(v) ((uint8_t)(v))
#define HI8(v) ((uint8_t)((v)>>8))
#define LO16(v) ((uint16_t)(v))
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
#define SET_HI8(v,x) ((v)=((v)&0xffff00ffu)|((uint32_t)(uint8_t)(x)<<8))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
'''
    names, cases = [], []
    base = 0x10000
    for width, reg, shift in ((8, 0, 0), (8, 4, 8), (16, 0, 0), (32, 0, 0)):
        for op in (*ALU, 'neg', 'shl', 'sar'):
            # clc; op; js taken; mov eax, 0; ret; taken: mov eax, 1; ret
            image = (b'\xf8' + _encode(op, width, reg)
                     + bytes.fromhex('7806b800000000c3b801000000c3'))
            config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                            entry_point=base, kernel_thunk_addr=base, origin='result-sign-width')
            db = {base: {'start': hex(base), 'end': base + len(image), '_addr': base, 'size': len(image)}}
            name = f'fixture_{op}_{width}_{reg}'
            code += FunctionTranslator(image, db).translate_function(base, db[base]).replace('sub_00010000', name)
            for value in INPUTS:
                a = (value >> shift) & ((1 << width) - 1)
                cases.append(f'{{{len(names)},0x{value:x}u,{_sign(op, width, a)}u}}')
            names.append(name)
    code += f'static void (*fixtures[])(void)={{{",".join(names)}}};\n'
    code += f'static const struct {{ unsigned f; uint32_t in, sf; }} cases[]={{{",".join(cases)}}};\n'
    code += '''int main(void) {
      for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        eax = cases[i].in; esp = 1024; fixtures[cases[i].f]();
        if (eax != cases[i].sf) { fprintf(stderr, "fixture %u input %x: js %u, SF %u", cases[i].f, cases[i].in, eax, cases[i].sf); return 1; }
      }
      return 0;
    }'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'test.c'
        source.write_text(code)
        exe = Path(temp) / 'test.exe'
        result = subprocess.run([cc, str(source), '-o', str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
