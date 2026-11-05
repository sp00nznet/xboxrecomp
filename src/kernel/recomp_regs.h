/*
 * recomp_regs.h - the recompiled register set as one value: what a runtime
 * path that runs guest code in the middle of other guest code saves first
 * and puts back after.
 *
 * Two such paths share this list: the DPC and posted-interrupt runs on the
 * gate holder (kernel_hal.c) and a mod's guest_call (recomp_guest_call.c).
 * Generated code keeps values in these across a call or an instruction, so
 * anything left out here leaks the inner call's state into the outer one: a
 * frame pointer (g_ebp) or an x87 slot the caller still reads, a rounding
 * mode (g_mxcsr). One list, so a register the lifter adds is saved by both.
 *
 * g_fs_base stays out: the inner code runs on the outer thread's TIB, as
 * on the console's one CPU.
 *
 * Not for the generated code: its register names are macros there
 * (recomp_types.h).
 */
#ifndef RECOMP_REGS_H
#define RECOMP_REGS_H

#include <stdint.h>
#include <string.h>

#include "xbox_memory_layout.h"     /* RECOMP_TLS, RecompXmm, RecompMmx */

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp;
extern RECOMP_TLS uint32_t g_seh_ebp, g_eflags, g_mxcsr;
extern RECOMP_TLS int g_df, g_fp_top, g_fp_cmp;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp, seh_ebp, eflags, mxcsr;
    int df, fp_top, fp_cmp;
    double fp[8];
    uint16_t fp_cw, fp_cc;
    RecompXmm xmm[8];
    RecompMmx mm[8];
} RecompRegs;

static inline void recomp_regs_save(RecompRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp; r->esp = g_esp;
    r->seh_ebp = g_seh_ebp; r->eflags = g_eflags; r->mxcsr = g_mxcsr; r->df = g_df;
    r->fp_top = g_fp_top; r->fp_cmp = g_fp_cmp;
    memcpy(r->fp, g_fp_stack, sizeof r->fp);
    r->fp_cw = g_fp_control_word; r->fp_cc = g_fp_cc;
    r->xmm[0] = g_xmm0; r->xmm[1] = g_xmm1; r->xmm[2] = g_xmm2; r->xmm[3] = g_xmm3;
    r->xmm[4] = g_xmm4; r->xmm[5] = g_xmm5; r->xmm[6] = g_xmm6; r->xmm[7] = g_xmm7;
    r->mm[0] = g_mm0; r->mm[1] = g_mm1; r->mm[2] = g_mm2; r->mm[3] = g_mm3;
    r->mm[4] = g_mm4; r->mm[5] = g_mm5; r->mm[6] = g_mm6; r->mm[7] = g_mm7;
}

static inline void recomp_regs_restore(const RecompRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp; g_esp = r->esp;
    g_seh_ebp = r->seh_ebp; g_eflags = r->eflags; g_mxcsr = r->mxcsr; g_df = r->df;
    g_fp_top = r->fp_top; g_fp_cmp = r->fp_cmp;
    memcpy(g_fp_stack, r->fp, sizeof r->fp);
    g_fp_control_word = r->fp_cw; g_fp_cc = r->fp_cc;
    g_xmm0 = r->xmm[0]; g_xmm1 = r->xmm[1]; g_xmm2 = r->xmm[2]; g_xmm3 = r->xmm[3];
    g_xmm4 = r->xmm[4]; g_xmm5 = r->xmm[5]; g_xmm6 = r->xmm[6]; g_xmm7 = r->xmm[7];
    g_mm0 = r->mm[0]; g_mm1 = r->mm[1]; g_mm2 = r->mm[2]; g_mm3 = r->mm[3];
    g_mm4 = r->mm[4]; g_mm5 = r->mm[5]; g_mm6 = r->mm[6]; g_mm7 = r->mm[7];
}

#endif /* RECOMP_REGS_H */
