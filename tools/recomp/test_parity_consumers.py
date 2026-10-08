"""Parity readers share existing CMP/TEST and float snapshots."""
import pytest
from .disasm import Instruction, Operand, BasicBlock
from .lifter import Lifter, lift_basic_block
from .test_icall_caller_cleans import _build_and_run


def ins(m, *ops):
    item=Instruction(0,1,m,'',''); item.operands=list(ops); return item


def reg(name): return Operand(type='reg',reg=name)


@pytest.mark.parametrize('producer', ['cmp','test','comiss','ucomiss'])
@pytest.mark.parametrize('aliases', [False,True])
@pytest.mark.parametrize('width', [8,16,32])
@pytest.mark.parametrize('memory', [False, True])
def test_compiled_parity_readers(producer, aliases, width, memory):
    is_float=producer.endswith('ss')
    items=[ins(producer,reg('xmm0' if is_float else {8:'al',16:'ax',32:'eax'}[width]),reg('xmm1' if is_float else {8:'cl',16:'cx',32:'ecx'}[width]))]
    items += [ins('mov',reg('eax'),Operand(type='imm',imm=123))]
    for m,dest,src in [('setp','dl',None),('setnp','bl',None),('cmovp','edi','esi'),('cmovnp','ebp','esi')]:
        if aliases: m=m.replace('np','po') if m.endswith('np') else m+'e'
        destination = (Operand(type='mem', mem_base='esp', mem_size=1)
                       if memory and dest=='dl' else reg(dest))
        items.append(ins(m,destination,*([reg(src)] if src else [])))
    for i,item in enumerate(items):item.address=i
    emitted,_=lift_basic_block(Lifter(),BasicBlock(0,items))
    source='''
#include <stdint.h>
#include <math.h>
typedef union { float f[4]; double d[2]; } Xmm;
#define LO16(v) ((v)&65535u)
#define MEM8(v) (*(uint8_t*)(uintptr_t)(v))
#define LO8(v) ((v)&255u)
#define SET_LO8(v,n) ((v)=((v)&0xffffff00u)|((n)&255u))
#define RECOMP_PARITY8(v) ((0x9669u>>(((v)^((v)>>4))&15u))&1u)
#define RECOMP_UNIMPL(a,b) ((void)0)
static int execute(double a,double b,int pf) {
    uint32_t eax=(uint32_t)a,ecx=(uint32_t)b,edx=0x123456a5,ebx=0xabcdefa5;
    uint32_t edi=11,ebp=11,esi=22,_fa=0,_fb=0;
    int32_t _fas=0,_fbs=0; int _flags=0,_cf=0;
    double _fca=0,_fcb=0; Xmm xmm0,xmm1;
    uint8_t guard[3]={0x5a,0xa5,0xc3}; uintptr_t esp=(uintptr_t)&guard[1];
    xmm0.f[0]=(float)a; xmm1.f[0]=(float)b;
'''+'\n'.join(emitted)+'''
    return edx!=(0x12345600u|(unsigned)pf) || ebx!=(0xabcdef00u|!pf) ||
           edi!=(pf?22u:11u) || ebp!=(pf?11u:22u);
}
int main(void) {
    TESTS
    return 0;
}
'''
    if memory:
        source=source.replace('edx!=(0x12345600u|(unsigned)pf)',
                              'edx!=0x123456a5u || guard[0]!=0x5a || guard[1]!=pf || guard[2]!=0xc3')
    if is_float:
        source=source.replace('(uint32_t)a','0').replace('(uint32_t)b','0')
        tests='const double v[]={1,2,0,-0.0,INFINITY,-INFINITY,NAN};'+ \
              'for(unsigned i=0;i<7;i++)for(unsigned j=0;j<7;j++)'+ \
              'if(execute(v[i],v[j],v[i]!=v[i]||v[j]!=v[j]))return 1;'
    else:
        op='-' if producer=='cmp' else '&'
        tests='for(unsigned a=0;a<512;a++)for(unsigned b=0;b<19;b++){'+ \
              'unsigned n=(a '+op+' b)&255u,bits=0;for(unsigned k=0;k<8;k++)bits+=(n>>k)&1u;'+ \
              'if(execute(a,b,(bits%2)==0))return 1;}'
    ran=_build_and_run(source.replace('TESTS',tests))
    assert ran.returncode==0,ran.stdout+ran.stderr
