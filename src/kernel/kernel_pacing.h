/*
 * kernel_pacing.h - frame pacing: the vblank schedule's arithmetic, the
 * sleeping wait that lowered guest spin loops call, and RECOMP_TRACE=pacing.
 *
 * A title that waits for its next frame in a busy loop on a counter the
 * vblank DPC moves (a title's main loop, typically) holds a host core for
 * nothing. With `tools.recomp --spin-waits`, the generator lowers such a
 * loop's back edge to RECOMP_SPIN_WAIT(va) (templates/runtime/recomp_types.h),
 * which calls xbox_SpinWait while the pacing mode is "sleep". The kernel
 * signals xbox_SpinWake whenever guest-visible state may have changed (a
 * vblank pass, any host-run ISR or DPC, a fence write), and every wait is
 * bounded at 1 ms, so a wake the kernel never sends costs a millisecond and
 * never hangs the title. Mode "spin" (the default) returns at once: the loop
 * busy-waits exactly as the console does.
 */
#ifndef XBOX_KERNEL_PACING_H
#define XBOX_KERNEL_PACING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── The vblank schedule (pure; tests/vblank_schedule drives it) ──
 *
 * Vblank N is due at t0 + N * 10^9 / 60 ns, so the rate is exact whatever
 * the caller's sleep granularity. A caller more than 100 ms behind (a
 * debugger stop, a stalled timer thread) restarts the grid at now instead of
 * firing the missed vblanks in a burst. */
typedef struct {
    int      started;
    uint64_t t0_ns;      /* the grid's epoch */
    uint64_t count;      /* vblanks fired since t0 */
} XboxVblankSchedule;

#define XBOX_VBLANK_HZ          60u
#define XBOX_VBLANK_RESTART_NS  100000000ull   /* 100 ms behind: restart */

/* Advance the schedule to now_ns. Returns 1 when a vblank is due now (and
 * counts it), 0 when not. *next_due_ns is the next edge on the grid (in the
 * past while catching up); *restarted_late_ns is how far behind a restart
 * was, 0 when none. */
int xbox_VblankSchedule(XboxVblankSchedule *s, uint64_t now_ns,
                        uint64_t *next_due_ns, uint64_t *restarted_late_ns);

/* ── The pacing mode ── */
enum { XBOX_PACING_SPIN = 0, XBOX_PACING_SLEEP = 1 };

/* Set before the guest starts (the enhancements layer's present.pacing).
 * Without a call the mode is spin. */
void xbox_PacingSetMode(int mode);
int  xbox_PacingMode(void);

/* Read by RECOMP_SPIN_WAIT on every pass of a lowered loop, with
 * g_xbox_irq_pending (kernel_hal.c): a pass with an interrupt posted calls
 * the wait at any pacing, as the gate holder's safe point. */
extern volatile int g_xbox_spin_sleep;
extern volatile int g_xbox_irq_pending;

/* One pass of a lowered loop whose condition was still true. First a safe
 * point (xbox_IrqSafePoint): the gate holder runs what was posted. Mode spin
 * returns there. Mode sleep:
 * return at once if the wake generation moved since this thread last looked,
 * else block until it moves or 1 ms passes. Never sleeps at DISPATCH level
 * or above, or with the one-CPU gate held: the DPC the loop waits for could
 * not run. */
void xbox_SpinWait(uint32_t va);

/* Guest-visible state may have changed: wake every xbox_SpinWait. One load
 * when the mode is spin. */
void xbox_SpinWake(void);

/* Per-site counters, for the trace and the tests. */
typedef struct {
    uint32_t va;
    long waits, immediate, wakes, timeouts, dispatch;
    /* Loops that left on a signal (or at once) or on the timeout; a loop's
     * exit is counted when its thread next waits, more than 0.5 ms later. */
    long exit_wake, exit_timeout;
} XboxSpinSiteStats;
/* Copy the site's counters since the last reset; 0 if the site never ran. */
int  xbox_SpinSiteStats(uint32_t va, XboxSpinSiteStats *out);
void xbox_SpinStatsReset(void);

/* ── RECOMP_TRACE=pacing ── */

/* The executor's flip, on every backend (nv2a_pb_exec.c). */
void xbox_PacingFlip(uint32_t flip, uint32_t batches);
/* A vblank raised at now_ns (the timer thread), for the gap range. */
void xbox_PacingVblank(uint64_t now_ns);

/* CPU time of the calling thread, and of the process, in ns. */
uint64_t xbox_ThreadCpuNs(void);
int      xbox_PacingTraceOn(void);  /* RECOMP_PACING_TRACE set: summaries print */
uint64_t xbox_ProcessCpuNs(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_PACING_H */
