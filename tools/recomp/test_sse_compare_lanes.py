"""COMISD register reads must use the 64-bit lane; single/memory controls."""
import pytest
from .disasm import Instruction, Operand
from .lifter import Lifter, _make_condition
from .test_icall_caller_cleans import _build_and_run


@pytest.mark.parametrize('mnemonic', ['comisd','ucomisd','comiss','ucomiss'])
@pytest.mark.parametrize('memory', [False, True])
def test_compare_snapshot_lane_and_operand_clobber(mnemonic, memory):
    double=mnemonic.endswith('sd')
    lhs=Operand(type='reg',reg='xmm0')
    rhs=(Operand(type='mem',mem_base='eax',mem_size=8 if double else 4)
         if memory else Operand(type='reg',reg='xmm1'))
    ins=Instruction(0,4,mnemonic,'',''); ins.operands=[lhs,rhs]
    emitted=chr(10).join(Lifter().lift_instruction(ins))
    conditions=[_make_condition(j,mnemonic,[lhs,rhs])[0] for j in ['jb','ja','je','jp']]
    result=' | '.join('((!!('+c+')) << '+str(i)+')' for i,c in enumerate(conditions))
    lane='d' if double else 'f'
    source="""
#include <stdint.h>
#include <math.h>
typedef union { float f[4]; double d[2]; } Xmm;
static Xmm xmm0,xmm1;
static double _fca,_fcb;
static uintptr_t eax;
#define MEMD(a) (*(double*)(uintptr_t)(a))
#define MEMF(a) (*(float*)(uintptr_t)(a))
static unsigned compare(double a,double b) {
    xmm0.LANE[0]=a; xmm1.LANE[0]=b; eax=(uintptr_t)&xmm1;
""" + emitted + """
    /* Flags refer to snapshots even after operands/address are clobbered. */
    xmm0.LANE[0]=42; xmm1.LANE[0]=-42; eax=0;
    return RESULT;
}
int main(void) {
    const double values[]={1,2,-1,-2,0,-0.0,INFINITY,-INFINITY,NAN};
    for(unsigned i=0;i<9;i++) for(unsigned j=0;j<9;j++) {
        double a=values[i],b=values[j]; int unordered=(a!=a||b!=b);
        unsigned want=(a<b||unordered) | ((a>b&&!unordered)<<1) |
                      ((a==b||unordered)<<2) | (unordered<<3);
        if(compare(a,b)!=want) return 1;
    }
    return 0;
}
"""
    source=source.replace('LANE',lane).replace('RESULT',result)
    ran=_build_and_run(source)
    assert ran.returncode==0,ran.stdout+ran.stderr
    if double and not memory and '.d[0]' in emitted:
        mutated=source.replace(emitted, emitted.replace('.d[0]', '.f[0]'))
        assert _build_and_run(mutated).returncode==1
