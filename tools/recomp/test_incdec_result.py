"""Arithmetic flags must survive a MOV that overwrites INC/DEC's operand."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import pytest
from . import config
from .translator import FunctionTranslator
from .c_fixture import compiler, command

def test_saved_incdec_result_compiled():
    cc=compiler()
    if not cc: pytest.skip('C compiler unavailable')
    code='''#include <stdint.h>
#include <stdio.h>
static uint32_t eax,esp;
#define LO8(v) ((uint8_t)(v))
#define LO16(v) ((uint16_t)(v))
#define ZX8(v) ((uint32_t)(uint8_t)(v))
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
#define RECOMP_PARITY8(v) ((0x9669u >> (((v) ^ ((v) >> 4)) & 15u)) & 1u)
'''
    names=[]
    for width,dec,inc in ((8,'fec8','fec0'),(16,'6648','6640'),(32,'48','40')):
        for increment,operation in enumerate((dec,inc)):
            for condition,opcode in (('z','94'),('s','98'),('o','90'),('l','9c')):
                name=f'fixture_{width}_{increment}_{condition}';names.append(name)
                image=bytes.fromhex(operation+'b8efbeadde0f'+opcode+'c00fb6c0c3')
                if condition=='o': image=bytes.fromhex(operation+'b8efbeadde7006b800000000c3b801000000c3')
                base=0x10000
                config._install([config.Section('.text',base,len(image),0,len(image),True)],entry_point=base,kernel_thunk_addr=base,origin='saved-incdec-result')
                db={base:{'start':hex(base),'end':base+len(image),'_addr':base,'size':len(image)}}
                code+=FunctionTranslator(image,db).translate_function(base,db[base]).replace('sub_00010000',name)
    code+='static void (*fixtures[24])(void)={'+','.join(names)+'};\n'
    code+='''int main(void) {
      const uint32_t inputs[]={0,1,2,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,0x7fffffff,0x80000000,0xffffffff};
      unsigned f=0;
      for(unsigned w=8;w<=32;w*=2) for(unsigned inc=0;inc<2;inc++) {
        uint32_t mask=w==32?0xffffffffu:(1u<<w)-1, sign=1u<<(w-1);
        for(unsigned c=0;c<4;c++,f++) for(unsigned i=0;i<12;i++) {
          uint32_t before=inputs[i]&mask, result=(before+(inc?1:0xffffffffu))&mask;
          unsigned sf=(result&sign)!=0, of=inc?before==sign-1:before==sign;
          unsigned expected=c==0?result==0:c==1?sf:c==2?of:sf!=of;
          eax=inputs[i];esp=1024;fixtures[f]();
          if(eax!=expected || esp!=1028) {fprintf(stderr,"w=%u inc=%u c=%u input=%x actual=%u expected=%u",w,inc,c,inputs[i],eax,expected);return 1;}
        }
      }
      return 0;
    }'''
    with tempfile.TemporaryDirectory() as temp:
        source=Path(temp)/'test.c';source.write_text(code)
        exe=Path(temp)/'test.exe'
        result=subprocess.run(command(cc,[source],exe),capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
