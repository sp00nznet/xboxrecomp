/*
 * The guest unwind (recomp_guest_unwind; game-reset D3, save-states phase
 * 3's tasks 3.1 and 3.2).
 *
 * A reset throws the guest away, so every guest thread has to leave guest
 * code first. Each one that runs under a call-in (xbox_GuestUnwindCallIn:
 * a created worker's start, the boot loop) has a jmp_buf there. A request
 * raises g_xbox_reset_pending; at its next unwind point the thread lets go
 * of the guest critical sections it owns and the guest CPU, and longjmps
 * back to its call-in, which returns 1. Lifted frames own no host
 * resources, so nothing between the point and the call-in needs undoing
 * except the IRQL and the one-CPU gate, which the call-in puts back.
 *
 * The points: the back edge (xbox_GuestCpuPreempt), the spin-wait passes,
 * the guest CPU lock's acquire and its queue, the entry and exit of every
 * kernel call, every blocking wait (which also wakes on the thread's
 * unwind event), the delay, a contended critical section, the
 * deterministic ring's turn. A host thread that calls into guest code (a
 * DPC, an ISR) is not a guest thread: it skips the call-in while a reset
 * is pending (xbox_GuestUnwindHostBegin), and the reset waits for one
 * already running to finish.
 *
 * Inert until xbox_GuestUnwindEnable: no thread is tracked, no event made,
 * and every point is one load of g_xbox_reset_pending and a branch. A
 * thread with no call-in (the title's main thread before a boot loop owns
 * it) never unwinds: its points return.
 */
#ifndef KERNEL_GUEST_UNWIND_H
#define KERNEL_GUEST_UNWIND_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

/* Where a thread unwound. */
enum {
    XBOX_UNWIND_BACK_EDGE = 1,  /* xbox_GuestCpuPreempt */
    XBOX_UNWIND_SPIN,           /* xbox_SpinWait */
    XBOX_UNWIND_KICK,           /* xbox_SpinWaitKick */
    XBOX_UNWIND_ACQUIRE,        /* the guest CPU lock: its entry, its queue, the grant */
    XBOX_UNWIND_BRIDGE,         /* a kernel call's entry or exit */
    XBOX_UNWIND_WAIT,           /* a blocking wait, woken or polled */
    XBOX_UNWIND_DELAY,          /* KeDelayExecutionThread */
    XBOX_UNWIND_CS,             /* a contended guest critical section */
    XBOX_UNWIND_DET,            /* the deterministic ring's turn or block */
    XBOX_UNWIND_CALL_IN,        /* the call-in itself: pending before it ran */
    XBOX_UNWIND_KINDS
};

/* Set from the request to the resume. Read by every point. */
extern volatile int32_t g_xbox_reset_pending;
/* Set once by xbox_GuestUnwindEnable, never cleared. */
extern volatile int32_t g_xbox_unwind_on;

/* Track guest threads from now on (the boot loop, a ctest); before any
 * guest thread starts. Off: every call below is a no-op or a plain call. */
void xbox_GuestUnwindEnable(void);
int  xbox_GuestUnwindEnabled(void);

/* The point: unwind now if a reset is pending and this thread can. Returns
 * when it cannot (no call-in, or the held-lock assertion refused it). */
void xbox_GuestUnwindSlow(int kind, uint32_t where);
static inline void xbox_GuestUnwindPoint(int kind, uint32_t where)
{
    if (g_xbox_reset_pending)
        xbox_GuestUnwindSlow(kind, where);
}

/* 1 when this thread would unwind at a point now: pending, and a call-in
 * to go back to. For the loops that must leave a lock before the jump. */
int  xbox_GuestUnwindArmed(void);

/* A host thread's guest call (a DPC, an ISR, on a device thread or on the
 * gate holder): Begin returns 0 to skip it, a reset is pending; otherwise a
 * token that End takes after the call. The reset's WaitStopped waits for
 * the calls in between to end. The count goes up before the flag is read
 * and the request raises the flag before it reads the count (both full
 * barriers), so a call either sees the flag or is waited for. Off: one
 * load and a branch, and the token is 1. */
int  xbox_GuestUnwindHostBeginSlow(void);
void xbox_GuestUnwindHostEndSlow(void);
static inline int xbox_GuestUnwindHostBegin(void)
{
    if (!g_xbox_unwind_on)
        return 1;
    return xbox_GuestUnwindHostBeginSlow();
}
static inline void xbox_GuestUnwindHostEnd(int token)
{
    if (token == 2)
        xbox_GuestUnwindHostEndSlow();
}

/* A guest thread's life. The creator counts a thread it is about to start
 * (SpawnBegin, 1 when counted; SpawnCancel when the host thread could not
 * start), so a request never sees "no live threads" between a create and
 * the new thread's join. Join registers this host thread (counted: the
 * creator's SpawnBegin result); Part, on every exit path, ends it. A
 * thread that cannot be tracked (the registry full, no event) is counted
 * as untracked until its Part, and a request refuses while one lives. */
int  xbox_GuestUnwindSpawnBegin(void);
void xbox_GuestUnwindSpawnCancel(int counted);
void xbox_GuestUnwindJoin(int counted);
void xbox_GuestUnwindPart(void);

/* A guest thread with no call-in to go back to: xbox_PsCreateSystemThreadEx's
 * (the native thunk table's creator, which no current title reaches). Counted
 * while it runs; a request with one live logs it and aborts, since a reset
 * could not stop it. */
void xbox_GuestUnwindForeign(int delta);

/* Run fn(arg) with a jmp_buf: 0 when it returned, 1 when the thread
 * unwound out of it (or a reset was pending before it ran). Nested calls
 * run fn plainly: the outermost call-in is where the thread goes back to.
 * The IRQL and the gate depth are put back as they were at the call. */
int  xbox_GuestUnwindCallIn(void (*fn)(void *), void *arg);

/* A host resource held across guest code on a guest thread: a toolkit
 * lock, the worker stack an ISR or DPC run on the holder borrows. A point
 * never unwinds while the count is not 0; it counts the refusal (and logs
 * the first few) and returns, and the next point past the region unwinds.
 * The assertion of task 1.2: no toolkit lock is held at a point today. */
void xbox_GuestUnwindHold(int delta);

/* A wait for a guest thread that also ends on its unwind event. The
 * results are Win32's; a wake by the reset unwinds instead of returning
 * (or, refused, waits again as asked). Not tracked: the plain wait. */
DWORD xbox_GuestUnwindWait(DWORD n, const HANDLE *h, BOOL all, DWORD ms,
                           BOOL alertable);
/* A sleep that ends on the unwind event: SleepEx's result. */
DWORD xbox_GuestUnwindSleep(DWORD ms, BOOL alertable);

/* The request (the reset thread): raise the flag and the preempt flag,
 * wake every tracked thread's waits and the deterministic ring; 1 when it
 * did. 0, with a log and nothing raised, while an untracked guest thread
 * lives: it would go on running guest code through the reset. Then
 * WaitStopped waits up to timeout_ms for every tracked thread to have
 * parted, except those parked at a boot loop, and for every host call-in
 * to end: 1 when they did, 0 on the timeout (the flag stays up; Resume
 * drops it). */
int  xbox_GuestUnwindRequest(void);
int  xbox_GuestUnwindWaitStopped(uint32_t timeout_ms);

/* The boot loop: after its call-in returned 1, wait here until Resume,
 * then take the guest CPU back if the unwind let go of it. */
void xbox_GuestUnwindPark(void);
/* Drop the flag and let parked threads go on (the reset's end). */
void xbox_GuestUnwindResume(void);

typedef struct {
    int  live;                  /* tracked threads that have not parted */
    int  starting;              /* counted by a creator, not joined yet */
    int  parked;                /* at a boot loop */
    long unwound[XBOX_UNWIND_KINDS];   /* per point kind, since boot */
    long refused;               /* the held-lock assertion */
    long cs_released;           /* critical-section recursions let go */
    int  untracked;             /* guest threads the registry could not take */
    int  host_calls;            /* host call-ins running guest code now */
} XboxGuestUnwindStats;
void xbox_GuestUnwindStatsGet(XboxGuestUnwindStats *out);

/* ── What the other kernel files give the unwind ─────────── */

/* kernel_hal.c: this thread's IRQL and one-CPU gate state at a call-in,
 * and putting it back after a jump past the lowers and leaves. */
typedef struct {
    int irql, gate_depth, gate_held, counted, in_isr, in_dpc;
} XboxIrqlMark;
void xbox_IrqlUnwindMark(XboxIrqlMark *m);
void xbox_IrqlUnwindRestore(const XboxIrqlMark *m);

/* kernel_rtl.c: leave every guest critical section this thread owns; the
 * recursions let go. */
int  xbox_RtlCsReleaseOwned(void);
/* kernel_rtl.c: a thread's part: drop this thread's owner records (not the
 * locks), so a later thread given the same id never leaves them. */
void xbox_RtlCsForgetOwned(void);

/* kernel_guest_cpu.c: let go of the guest CPU for a park (0 when it was not
 * held), and take it back with that result. */
int  xbox_GuestCpuUnwindDrop(void);
void xbox_GuestCpuUnwindRetake(int how);
/* The per-thread stats records in use (the ctest: parted threads free
 * theirs). */
int  xbox_GuestCpuThreadRecsUsed(void);

/* kernel_guest_det.c: wake the threads waiting for the ring's token. */
void xbox_GuestDetUnwindWake(void);

#endif
