"""Narrow div/idiv, cwd, register xadd, and the carry out of rol/ror.

The conformance fuzzer found each of these lifting wrong:

- div/idiv on an 8- or 16-bit operand divided edx:eax at 32 bits, so `div cl`
  wrote the whole quotient to eax and the remainder to edx, where x86 puts
  them in al and ah;
- cwd, and xadd with a register destination, had no lift and went to
  recomp_unimpl, a silent no-op in a title;
- rol/ror never wrote CF, so a setc or adc after one read whatever carry came
  before it.

Each fixture is lifted, compiled and run over boundary inputs, and every
register it can write is compared with what x86 defines. Inputs that would
raise a divide error on the real CPU are left out. Against the unfixed lifter
every fixture fails.
"""
from pathlib import Path
import subprocess
import tempfile
import pytest
from . import config
from .translator import FunctionTranslator
from .c_fixture import compiler, command

VALUES = (0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
          0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff, 0x12345678)
EDX = (0, 1, 0xffff0000, 0xffffffff)

PREAMBLE = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax, ecx, edx, esp;
#define LO8(v) ((uint8_t)(v))
#define LO16(v) ((uint16_t)(v))
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
#define SET_HI8(v,x) ((v)=((v)&0xffff00ffu)|((uint32_t)(uint8_t)(x)<<8))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
#define RECOMP_PARITY8(v) ((0x9669u >> (((v) ^ ((v) >> 4)) & 15u)) & 1u)
#define RECOMP_UNIMPL(text, va) ((void)0)   /* a no-op, as in a title */
static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31; return n ? ((val << n) | (val >> (32 - n))) : val; }
static inline uint8_t ROR8(uint8_t val, int n) {
    n = (n & 31) % 8; return n ? (uint8_t)((val >> n) | (val << (8 - n))) : val; }
'''


def _sx(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def _tdiv(n, d):
    """C/x86 division: the quotient truncates toward zero."""
    q = abs(n) // abs(d)
    q = q if (n < 0) == (d < 0) else -q
    return q, n - q * d


def _div8(a, c, d):
    n, v = a & 0xffff, c & 0xff
    if not v or n // v > 0xff:
        return None
    return (a & 0xffff0000) | (n % v) << 8 | n // v, c, d


def _idiv8(a, c, d):
    n, v = _sx(a, 16), _sx(c, 8)
    if not v:
        return None
    q, r = _tdiv(n, v)
    if not -0x80 <= q <= 0x7f:
        return None
    return (a & 0xffff0000) | (r & 0xff) << 8 | q & 0xff, c, d


def _div16(a, c, d):
    n, v = (d & 0xffff) << 16 | a & 0xffff, c & 0xffff
    if not v or n // v > 0xffff:
        return None
    return (a & 0xffff0000) | n // v, c, (d & 0xffff0000) | n % v


def _idiv16(a, c, d):
    n, v = _sx((d & 0xffff) << 16 | a & 0xffff, 32), _sx(c, 16)
    if not v:
        return None
    q, r = _tdiv(n, v)
    if not -0x8000 <= q <= 0x7fff:
        return None
    return (a & 0xffff0000) | q & 0xffff, c, (d & 0xffff0000) | r & 0xffff


def _cwd(a, c, d):
    return a, c, (d & 0xffff0000) | (0xffff if a & 0x8000 else 0)


def _xadd32(a, c, d):
    return (a + c) & 0xffffffff, a, d


def _xadd32_setz(a, c, d):
    total = (a + c) & 0xffffffff
    return total, a, (d & 0xffffff00) | (total == 0)


def _xadd8_sets(a, c, d):
    total = (a + c) & 0xff
    return ((a & 0xffffff00) | total, (c & 0xffffff00) | a & 0xff,
            (d & 0xffffff00) | total >> 7)


def _rol32_setc(a, c, d):
    r = (a << 1 | a >> 31) & 0xffffffff
    return r, c, (d & 0xffffff00) | r & 1


def _ror8_setc(a, c, d):
    r = (a & 0xff) >> 1 | (a & 1) << 7
    return (a & 0xffffff00) | r, c, (d & 0xffffff00) | r >> 7


# (name, code before the ret, reference)
FIXTURES = (
    ("div_r8", "f6f1", _div8),                  # div cl
    ("idiv_r8", "f6f9", _idiv8),                # idiv cl
    ("div_r16", "66f7f1", _div16),              # div cx
    ("idiv_r16", "66f7f9", _idiv16),            # idiv cx
    ("cwd", "6699", _cwd),
    ("xadd_r32", "0fc1c8", _xadd32),            # xadd eax, ecx
    ("xadd_r32_setz", "0fc1c80f94c2", _xadd32_setz),  # ...; setz dl
    ("xadd_r8_sets", "0fc0c80f98c2", _xadd8_sets),    # xadd al, cl; sets dl
    ("rol_r32_setc", "d1c00f92c2", _rol32_setc),      # rol eax, 1; setc dl
    ("ror_r8_setc", "d0c80f92c2", _ror8_setc),        # ror al, 1; setc dl
)


def _lift(name, code_hex):
    base = 0x10000
    image = bytes.fromhex(code_hex + "c3")
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="narrow-forms")
    db = {base: {"start": hex(base), "end": base + len(image), "_addr": base,
                 "size": len(image)}}
    return (FunctionTranslator(image, db).translate_function(base, db[base])
            .replace("sub_00010000", "fixture_" + name))


def test_narrow_forms_match_x86_compiled():
    cc = compiler()
    if not cc:
        pytest.skip("C compiler unavailable")
    code = PREAMBLE + "".join(_lift(name, hx) for name, hx, _ in FIXTURES)
    rows = []
    for index, (_name, _hx, reference) in enumerate(FIXTURES):
        for a in VALUES:
            for c in VALUES:
                for d in EDX:
                    out = reference(a, c, d)
                    if out is not None:
                        rows.append("{%d,0x%xu,0x%xu,0x%xu,0x%xu,0x%xu,0x%xu}"
                                    % (index, a, c, d, *out))
    code += "static void (*fixtures[])(void) = {%s};\n" % ",".join(
        "fixture_" + name for name, _, _ in FIXTURES)
    code += "static const char *names[] = {%s};\n" % ",".join(
        '"%s"' % name for name, _, _ in FIXTURES)
    code += ("static const struct { unsigned f; uint32_t a, c, d, ra, rc, rd; }"
             " cases[] = {%s};\n" % ",\n".join(rows))
    code += '''int main(void) {
  unsigned bad = 0;
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    eax = cases[i].a; ecx = cases[i].c; edx = cases[i].d; esp = 1024;
    fixtures[cases[i].f]();
    if (eax != cases[i].ra || ecx != cases[i].rc || edx != cases[i].rd) {
      if (bad++ < 20)
        printf("%s eax=%08x ecx=%08x edx=%08x: got %08x %08x %08x, x86 %08x %08x %08x\\n",
               names[cases[i].f], cases[i].a, cases[i].c, cases[i].d,
               eax, ecx, edx, cases[i].ra, cases[i].rc, cases[i].rd);
    }
  }
  printf("%u of %u wrong\\n", bad, (unsigned)(sizeof cases / sizeof cases[0]));
  return bad != 0;
}'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / "test.c"
        source.write_text(code)
        exe = Path(temp) / "test.exe"
        built = subprocess.run(command(cc, [source], exe), capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
