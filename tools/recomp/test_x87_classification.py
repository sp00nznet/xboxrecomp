"""Compile lifted FXAM/FNSTSW across functions against Intel status patterns."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import pytest
from .disasm import Instruction, Operand
from .lifter import Lifter
from .c_fixture import compiler, command as compile_command

def test_classification_status_compiled():
    cc=compiler()
    if not cc: pytest.skip('C compiler unavailable')
    header=(Path(__file__).resolve().parents[2]/'templates/runtime/recomp_types.h').read_text()
    helper=re.search(r'static inline uint16_t recomp_fxam\(.*?\n}',header,re.S).group()
    macros='\n'.join(line for line in header.splitlines() if line.startswith(('#define RECOMP_FCMP(', '#define RECOMP_FCMP_CC(')))
    source='#include <stdint.h>\n#include <math.h>\n#include <string.h>\n'+helper+'\n'+macros+'''
static double values[8];
static unsigned g_fp_top;
static int g_fp_cmp;
static uint16_t g_fp_cc;
static uint32_t eax;
#define fp_top() values[g_fp_top]
'''
    for name,mnemonic,op in [('examine','fxam',''),('status','fnstsw','ax'),('testzero','ftst','')]:
        ins=Instruction(0,2,mnemonic,op,'')
        if op: ins.operands=[Operand(type='reg',reg=op)]
        source+='static void '+name+'(void) {'+'\n'.join(Lifter().lift_instruction(ins))+'}\n'
    source+='''
int main(void) {
  const double input[]={0,1,4.712,0x1p-1022,0x1p-1074,INFINITY,NAN};
  const uint16_t expected[]={0x4000,0x400,0x400,0x400,0x400,0x500,0x100};
  for(unsigned top=0;top<8;top++) for(unsigned i=0;i<7;i++) for(unsigned sign=0;sign<2;sign++) {
    g_fp_top=top; fp_top()=copysign(input[i],sign?-1:1);
    double original=fp_top(); g_fp_cmp=2; g_fp_cc=0x4500; eax=0xABCD1234;
    examine(); status();
    if(eax!=(0xABCD0000u|(top<<11)|expected[i]|(sign?0x200:0))) return 1;
    if(g_fp_top!=top || memcmp(&original,&fp_top(),8)) return 2;
    testzero(); status();
    unsigned result=i==6?0x4500:i==0?0x4000:sign?0x100:0;
    if((eax&0xffff)!=((top<<11)|result)) return 3;
  }
  return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path=Path(tmp)/'classify.c'; path.write_text(source)
        exe=Path(tmp)/'classify.exe'
        command=compile_command(cc,[path],exe,gnu_flags=[] if os.name=='nt' else ['-lm'])
        result=subprocess.run(command,capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
