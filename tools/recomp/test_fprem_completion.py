"""Completed host reduction must clear the x87 C2 polling bit."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import pytest
from .disasm import Instruction
from .lifter import Lifter


def build_run(source):
    cc = shutil.which('cl') or shutil.which('clang') or shutil.which('gcc')
    if not cc:
        pytest.skip('C compiler unavailable')
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp)/'fixture.c', Path(tmp)/'fixture.exe'
        c.write_text(source, encoding='utf-8')
        args = ([cc, '/nologo', '/W0', '/O2', str(c), '/Fe:'+str(exe)]
                if Path(cc).stem.lower() == 'cl' else
                [cc, '-O2', str(c), '-o', str(exe), '-lm'])
        built = subprocess.run(args, cwd=tmp, capture_output=True, text=True)
        assert built.returncode == 0, built.stdout+built.stderr
        return subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True, timeout=5)


@pytest.mark.parametrize('mnemonic,expected', [('fprem', 2.0), ('fprem1', -1.0)])
def test_completed_remainder_releases_c2_polling(mnemonic, expected):
    emitted = chr(10).join(Lifter().lift_instruction(Instruction(0, 2, mnemonic, '', '')))
    source = """
#include <stdint.h>
#include <math.h>
static double values[8];
static unsigned g_fp_top;
static uint16_t g_fp_cc;
#define fp_top() values[g_fp_top]
#define fp_st1() values[(g_fp_top + 1) & 7]
static void reduce(void) {
""" + emitted + """
}
int main(void) {
    for (unsigned top=0; top<8; top++) {
        g_fp_top=top; fp_top()=8.0; fp_st1()=3.0;
        g_fp_cc=0x4500; /* includes FXAM's C2; unrelated bits retained */
        unsigned polls=0;
        do { reduce(); } while ((g_fp_cc & 0x0400) && ++polls<3);
        if (polls || g_fp_cc!=0x4100 || g_fp_top!=top ||
            fp_st1()!=3.0 || fp_top()!=EXPECTED) return 1;
    }
    return 0;
}
""".replace('EXPECTED', repr(expected))
    ran=build_run(source)
    assert ran.returncode==0, ran.stdout+ran.stderr
    # Mutation proves the same bounded harness detects stale C2.
    old=source.replace('g_fp_cc &= (uint16_t)~0x0400u;', '')
    if old != source:
        assert build_run(old).returncode==1
