"""SAR must sign-extend its operand width and mask the x86 shift count."""
from pathlib import Path
import shutil, subprocess, tempfile
import pytest
from . import config
from .translator import FunctionTranslator
from .c_fixture import compiler, command

def test_sar_width_and_carry_compiled():
    cc=compiler()
    if not cc: pytest.skip('C compiler unavailable')
    code='''#include <stdint.h>
    #include <stdio.h>
    static uint32_t eax,ecx,edx,esp;
    #define LO8(v) ((uint8_t)(v))
    #define LO16(v) ((uint16_t)(v))
    #define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
    #define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
    #define RECOMP_PARITY8(v) ((0x9669u >> (((v) ^ ((v) >> 4)) & 15u)) & 1u)
    '''
    for w,op in ((8,'d2f8'),(16,'66d3f8'),(32,'d3f8')):
        b=bytes.fromhex('f9'+op+'83d200c3');base=0x10000
        config._install([config.Section('.text',base,len(b),0,len(b),True)],entry_point=base,kernel_thunk_addr=base,origin='sar-width')
        db={base:dict(start=hex(base),end=base+len(b),_addr=base,size=len(b))}
        code+=FunctionTranslator(b,db).translate_function(base,db[base]).replace('sub_00010000',f'fixture{w}')
    code+='''int main(void) {
      const uint32_t values[]={0,1,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,0x7fffffff,0x80000000,0xffffffff,0xa58180ff};
      const unsigned counts[]={0,1,4,7,8,15,16,31,32,33,255};
      void (*f[])(void)={fixture8,fixture16,fixture32}; unsigned total=0;
      for(unsigned k=0,w=8;k<3;k++,w*=2) for(unsigned i=0;i<12;i++) for(unsigned j=0;j<11;j++) {
        uint32_t mask=w==32?0xffffffffu:(1u<<w)-1, bits=values[i]&mask;
        int64_t signed_value=(bits&(1u<<(w-1)))?(int64_t)bits-(INT64_C(1)<<w):bits;
        unsigned n=counts[j]&31; int64_t divisor=INT64_C(1)<<n;
        int64_t quotient=signed_value>=0?signed_value/divisor:-((-signed_value+divisor-1)/divisor);
        uint32_t expected=(values[i]&~mask)|((uint32_t)quotient&mask);
        unsigned carry=n?(n>=w?(signed_value<0):((bits>>(n-1))&1)):1;
        eax=values[i];ecx=counts[j];edx=0;esp=1024;f[k]();total++;
        if(eax!=expected||esp!=1028||edx!=carry) {printf("FAIL width=%u input=%08x count=%u actual=%08x expected=%08x carry=%u expected=%u\\n",w,values[i],counts[j],eax,expected,edx,carry);return 1;}
      }
      printf("PASS %u compiled SAR width/count cases\\n",total);return 0;
    }'''

    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'test.c'; path.write_text(code)
        exe=Path(temp)/'test.exe'
        compiled=subprocess.run(command(cc,[path],exe),capture_output=True,text=True)
        assert compiled.returncode==0,compiled.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
