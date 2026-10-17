/*
 * kernel_prof.h - the RECOMP_TRACE=dpc profiler's API (kernel_prof.c).
 *
 * A header of its own, needing only <stdint.h>, so the APU model, which
 * does not include kernel.h, shares these values instead of copying them.
 */
#ifndef XBOXRECOMP_KERNEL_PROF_H
#define XBOXRECOMP_KERNEL_PROF_H

#include <stdint.h>

/* RECOMP_TRACE=dpc: per-routine time of the host-run ISRs and DPCs, and
 * what they wait on (kernel_prof.c). Check xbox_ProfOn() first; the rest
 * is for when it is on. Begin makes the routine the thread's current one,
 * so a wait recorded while it runs is charged to it; End records the run
 * and restores what Begin returned. */
enum { XBOX_PROF_DPC_TIMER, XBOX_PROF_DPC_QUEUE, XBOX_PROF_ISR_VBLANK,
       XBOX_PROF_ISR_APU, XBOX_PROF_ISR_OHCI, XBOX_PROF_SYNC, XBOX_PROF_KINDS };
/* WAIT_APULOCK has had no caller since voice_lock stopped taking the APU
 * lock (apu_vp.c); it stays so dpc logs keep their columns across that. */
enum { XBOX_PROF_WAIT_GATE, XBOX_PROF_WAIT_APULOCK, XBOX_PROF_WAIT_STALL,
       XBOX_PROF_WAITS };
enum { XBOX_PROF_N_LOOPS, XBOX_PROF_SLEEP_OVER, XBOX_PROF_SCHED_RESTART,
       XBOX_PROF_DPCQ_DEPTH, XBOX_PROF_DRAIN_BUSY, XBOX_PROF_SE_FRAME,
       XBOX_PROF_COUNTERS };
extern int xbox_prof_state_;            /* -1 not read yet, 0 off, 1 on */
int       xbox_ProfInit(void);
static inline int xbox_ProfOn(void)
{
    int s = xbox_prof_state_;
    return s > 0 || (s < 0 && xbox_ProfInit());
}
long long xbox_ProfNowUs(void);
void     *xbox_ProfBegin(int kind, uint32_t routine);
void      xbox_ProfEnd(void *prev, long long run_us);
void      xbox_ProfWait(int what, long long us);
void      xbox_ProfCount(int counter, long long v);
void      xbox_ProfReport(void);
void      xbox_ProfKernelCall(unsigned ordinal);  /* kernel thunk dispatch */

#endif
