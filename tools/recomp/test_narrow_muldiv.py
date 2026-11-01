"""The 8- and 16-bit one-operand MUL/IMUL/DIV/IDIV keep the registers x86 keeps.

They were lifted as the 32-bit form, which writes all of EDX. Auto
Modellista's menu auto-repeat loads DL, runs "imul bl" and reads DL back;
with EDX overwritten the repeat threshold became zero and the menu moved on
its own. Each fixture is one instruction plus RET, translated, compiled and
run against x86's results.
"""
from pathlib import Path
import subprocess
import tempfile
import pytest
from . import config
from .translator import FunctionTranslator
from .c_fixture import compiler, command

# (name, bytes, op, width, source): op 0-3 is mul, imul, div, idiv. The source
# says where the operand comes from, so the model reads the same value:
# 'b' is BL/BX, 'ah' and 'dx' alias the registers the instruction writes, and
# 'mem' is [ecx]. The aliasing forms catch a lifter that writes AX or DX
# before it has read the operand.
FIXTURES = (
    ('mul8', 'f6e3', 0, 8, 'b'), ('imul8', 'f6eb', 1, 8, 'b'),
    ('div8', 'f6f3', 2, 8, 'b'), ('idiv8', 'f6fb', 3, 8, 'b'),
    ('mul16', '66f7e3', 0, 16, 'b'), ('imul16', '66f7eb', 1, 16, 'b'),
    ('div16', '66f7f3', 2, 16, 'b'), ('idiv16', '66f7fb', 3, 16, 'b'),
    ('mul_ah', 'f6e4', 0, 8, 'ah'), ('idiv_ah', 'f6fc', 3, 8, 'ah'),
    ('mul_dx', '66f7e2', 0, 16, 'dx'),
    ('mul_m8', 'f621', 0, 8, 'mem'), ('idiv_m8', 'f639', 3, 8, 'mem'),
    ('mul_m16', '66f721', 0, 16, 'mem'), ('div_m16', '66f731', 2, 16, 'mem'),
)

# div dx divides DX:AX by DX: with DX nonzero the quotient is at least 0x10000
# and x86 raises #DE, so no run can check it. The lifted text must still take
# the divisor from DX before it writes DX.
TEXT_ONLY = (('div_dx', '66f7f2'), ('idiv_dx', '66f7fa'))

SOURCES = {'b': 0, 'ah': 1, 'dx': 2, 'mem': 3}

HEADER = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_ebp,g_seh_ebp;
static uint8_t g_mem[16];
#define LO8(v) ((uint8_t)(v))
#define HI8(v) ((uint8_t)((v) >> 8))
#define LO16(v) ((uint16_t)(v))
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
#define SET_HI8(v,x) ((v)=((v)&0xffff00ffu)|((uint32_t)(uint8_t)(x)<<8))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
#define MEM8(a) (g_mem[(a) & 15])
#define MEM16(a) ((uint16_t)(g_mem[(a) & 15] | (g_mem[((a) + 1) & 15] << 8)))
'''

MAIN = r'''
struct ref { uint32_t eax, edx; };
/* x86's result for op (0 mul, 1 imul, 2 div, 3 idiv) at a width, with s the operand
   value. Sets *ok to 0 when x86 would raise #DE. */
static struct ref model(int op, int w, uint32_t a, uint32_t s, uint32_t d, int *ok) {
  struct ref r = { a, d };
  uint32_t ax = a & 0xffff, dx = d & 0xffff;
  *ok = 1;
  if (w == 8) {
    uint8_t b = (uint8_t)s;
    switch (op) {
    case 0: r.eax = (a & 0xffff0000u) | (uint16_t)((uint8_t)a * b); break;
    case 1: r.eax = (a & 0xffff0000u) | (uint16_t)((int8_t)a * (int8_t)b); break;
    case 2: if (!b || ax / b > 0xff) { *ok = 0; break; }
      r.eax = (a & 0xffff0000u) | ((ax % b) << 8) | (uint8_t)(ax / b); break;
    case 3: { int n = (int16_t)ax, q; if (!b) { *ok = 0; break; }
      q = n / (int8_t)b; if (q < -128 || q > 127) { *ok = 0; break; }
      r.eax = (a & 0xffff0000u) | ((uint8_t)(n % (int8_t)b) << 8) | (uint8_t)q; break; }
    }
  } else {
    uint16_t b = (uint16_t)s;
    switch (op) {
    case 0: { uint32_t p = ax * b; r.eax = (a & 0xffff0000u) | (p & 0xffff); r.edx = (d & 0xffff0000u) | (p >> 16); break; }
    case 1: { uint32_t p = (uint32_t)((int16_t)ax * (int16_t)b); r.eax = (a & 0xffff0000u) | (p & 0xffff); r.edx = (d & 0xffff0000u) | (p >> 16); break; }
    case 2: { uint32_t n = (dx << 16) | ax; if (!b || n / b > 0xffff) { *ok = 0; break; }
      r.eax = (a & 0xffff0000u) | (uint16_t)(n / b); r.edx = (d & 0xffff0000u) | (uint16_t)(n % b); break; }
    case 3: { int32_t n = (int32_t)((dx << 16) | ax), q; if (!b) { *ok = 0; break; }
      q = n / (int16_t)b; if (q < -32768 || q > 32767) { *ok = 0; break; }
      r.eax = (a & 0xffff0000u) | (uint16_t)q; r.edx = (d & 0xffff0000u) | (uint16_t)(n % (int16_t)b); break; }
    }
  }
  return r;
}
int main(void) {
  const uint32_t as[] = { 0xdead0007u, 0x12340085u, 0xffff00ffu, 0x0000fe10u, 0x55550001u, 0x00000703u, 0x0000f9ccu,
                          /* idiv ah only fits its quotient in a byte when AH is -1 and AL >= 0x81 */
                          0x1234ff85u, 0xabcdffc3u };
  const uint32_t bs[] = { 0x00000003u, 0xffffff7fu, 0x000000feu, 0x0000ffffu, 0x00007ff1u };
  const uint32_t ds[] = { 0xcafe0000u, 0x5a5a0000u, 0x1234ffffu, 0x00000003u };
  for (int f = 0; f < NFIX; f++) {
    int ran = 0;
    for (unsigned i = 0; i < 9; i++) for (unsigned j = 0; j < 5; j++) for (unsigned k = 0; k < 4; k++) {
      uint32_t a = as[i], b = bs[j], d = ds[k], s = b, c = 4;
      int ok;
      switch (srcs[f]) { case 1: s = a >> 8; break; case 2: s = d; break; }
      struct ref want = model(ops[f], widths[f], a, s, d, &ok);
      if (!ok) continue;
      g_mem[4] = (uint8_t)b; g_mem[5] = (uint8_t)(b >> 8);
      eax = a; ebx = b; ecx = c; edx = d; esp = 1024;
      fixtures[f]();
      ran++;
      if (eax != want.eax || edx != want.edx || ebx != b || ecx != c || esp != 1028) {
        fprintf(stderr, "%s a=%x b=%x d=%x: eax=%x edx=%x, want eax=%x edx=%x\n",
                names[f], a, b, d, eax, edx, want.eax, want.edx);
        return 1;
      }
    }
    if (!ran) { fprintf(stderr, "%s: no case ran\n", names[f]); return 1; }
  }
  return 0;
}
'''


def _translate(name, hexbytes):
    image = bytes.fromhex(hexbytes + 'c3')
    base = 0x10000
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='narrow-muldiv')
    db = {base: {'start': hex(base), 'end': base + len(image), '_addr': base, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(base, db[base]).replace('sub_00010000', name)


def test_narrow_forms_leave_the_wide_halves():
    """Lifted text: no byte or word form may write EDX as a whole."""
    for name, hexbytes in [f[:2] for f in FIXTURES] + list(TEXT_ONLY):
        code = _translate(name, hexbytes)
        assert 'edx = (uint32_t)' not in code, (name, code)
        assert '_dividend' not in code and '_r >> 32' not in code, (name, code)


def test_divisor_dx_read_before_dx_written():
    for name, hexbytes in TEXT_ONLY:
        code = _translate(name, hexbytes)
        divisor = code.index('_d = ')
        assert 'LO16(edx)' in code[divisor:code.index(';', divisor)], (name, code)
        assert divisor < code.index('SET_LO16(edx'), (name, code)


def test_narrow_muldiv_compiled():
    cc = compiler()
    if not cc:
        pytest.skip('C compiler unavailable')
    n = len(FIXTURES)
    code = HEADER + ''.join(_translate('fx_' + f[0], f[1]) for f in FIXTURES)
    code += f'#define NFIX {n}\n'
    code += f'static void (*fixtures[{n}])(void)={{' + ','.join('fx_' + f[0] for f in FIXTURES) + '};\n'
    code += f'static const char *names[{n}]={{' + ','.join(f'"{f[0]}"' for f in FIXTURES) + '};\n'
    code += f'static const int ops[{n}]={{' + ','.join(str(f[2]) for f in FIXTURES) + '};\n'
    code += f'static const int widths[{n}]={{' + ','.join(str(f[3]) for f in FIXTURES) + '};\n'
    code += f'static const int srcs[{n}]={{' + ','.join(str(SOURCES[f[4]]) for f in FIXTURES) + '};\n'
    code += MAIN
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'test.c'
        source.write_text(code)
        exe = Path(temp) / 'test.exe'
        result = subprocess.run(command(cc, [source], exe), capture_output=True, text=True)
        assert result.returncode == 0, result.stderr[-3000:]
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
