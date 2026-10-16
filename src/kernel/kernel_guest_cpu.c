/*
 * kernel_guest_cpu.c - the guest CPU (RECOMP_GUEST_LOCK=1).
 *
 * The Xbox has one CPU: two guest threads never run guest code at the same
 * instant, and titles lean on that without knowing it -- a worker fills a
 * buffer and sets a flag with plain stores, and the reader trusts the order;
 * two threads add to one pool with "slot = count; fill; count = slot + 1"
 * and on two host cores lose each other's entries. Pinning the title's
 * threads to one core (xbox_GuestThreadPin) makes that as rare as a console
 * quantum boundary where the host has thread affinity; macOS has none.
 *
 * The lock: a guest thread holds it while it runs guest code and lets go
 * for the length of every kernel call. Host threads that only run a guest
 * callback (the timer thread's DPCs, the device models' ISRs) never take
 * it: an interrupt ran between any two instructions of any thread on the
 * console, and the one-CPU gate (kernel_hal.c) already orders them against
 * guest code at DISPATCH_LEVEL. Taking it for a DPC would also deadlock a
 * guest thread spinning on the DPC's result.
 *
 * What the lock hands over, and when, is the console's scheduler, and a
 * plain critical section is not it (one title's attract mode: 32 flips/s
 * with the lock off, 28.5 with a critical section, 12.7 with a fair ticket
 * lock that switched at every kernel call):
 *  - a kernel call that returns at once switches nobody. A title makes a
 *    few hundred a frame (IRQL raises and lowers, event sets); a hand-off at
 *    each one is two context switches and a wake, and the frame goes to
 *    scheduling. So a kernel call is a soft release: the CPU is marked free
 *    and the caller takes it straight back on return if nobody took it
 *    meanwhile (it barges past the queue, as a mutex owner re-locking does).
 *  - a call that may block (a wait, a sleep, file I/O) wakes the waiters, so
 *    one takes the CPU if the caller does block; the others' turn comes
 *    through the queue. A waiter also looks again every 2 ms on its own, so
 *    a bridge that blocks without being on the list costs latency, never a
 *    hang.
 *  - a thread that has run for its quantum with another waiting hands over
 *    at its next kernel call or loop back edge (RECOMP_BACK_EDGE, in code
 *    lifted with --back-edge-yield; without it only at kernel calls): the
 *    oldest waiter is granted the CPU, barging is refused until it has
 *    taken it, and the releaser queues behind every waiter.
 *    Without this the unfair critical section let a worker keep the CPU
 *    through a whole decode burst while the main thread lost frames, and a
 *    guest loop spinning on another guest thread's flag with no kernel call
 *    in it deadlocked.
 *  - NtYieldExecution and the thread's end hand over at once.
 *
 * RECOMP_GUEST_QUANTUM (ms, default 4) is the quantum; RECOMP_GUEST_CPU_TRACE
 * (ms) prints each turn held at least that long and where it ended.
 */
#include "kernel.h"
#include "platform/host_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read by RECOMP_BACK_EDGE at every loop back edge of lifted code: a waiter
 * has waited its quantum, so the holder hands over at the edge. Set by the
 * waiter at its poll, cleared by the hand-off; the one relaxed load the
 * check costs is the price of a turn bounded without a kernel call. */
int32_t g_xbox_guest_cpu_preempt;

static int32_t preempt_get(void)
{
    return (int32_t)InterlockedCompareExchange(
        (volatile LONG *)&g_xbox_guest_cpu_preempt, 0, 0);
}

static void preempt_set(int32_t v)
{
    InterlockedExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, (LONG)v);
}

#define GUEST_CPU_POLL_MS 2             /* a waiter's own look at a free CPU */

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static int s_on;
static uint64_t s_trace_ns;                 /* RECOMP_GUEST_CPU_TRACE; 0 = off */
static uint64_t s_quantum_ns;               /* RECOMP_GUEST_QUANTUM */

/* Under s_lock. The queue is tickets: a waiter takes next_ticket and runs
 * when it is head and the CPU is free; granted says a hand-off reserved the
 * free CPU for the head, so a returning caller may not barge. */
static int s_free = 1;
static int s_granted;
static unsigned s_next_ticket, s_head, s_waiting;
static volatile LONG s_owner_tid;

static XBOX_THREAD_LOCAL int t_guest;       /* this host thread runs a guest thread */
static XBOX_THREAD_LOCAL int t_held;        /* and holds the guest CPU */
/* The thread's turn: from the acquire that followed a wait or a hand-off
 * to the hand-off that ends it. A soft release the caller barges back from
 * keeps the turn open; one a waiter took the CPU at ends it there, which
 * is known only when the caller has waited. */
static XBOX_THREAD_LOCAL int t_turn_open;
static XBOX_THREAD_LOCAL uint64_t t_turn_ns, t_soft_ns;
static XBOX_THREAD_LOCAL int t_soft_kind;
static XBOX_THREAD_LOCAL uint32_t t_soft_where;

/* The window's counters (xbox_GuestCpuReport resets them). */
static volatile LONG s_turns, s_releases, s_switches, s_waits, s_preempts;
static volatile LONG s_max_turn_us, s_max_wait_us;
static volatile LONG s_max_turn_kind, s_max_turn_where;
static volatile LONG s_trace_lines;
static uint64_t s_trace_last_ns;

static void guest_cpu_config(void)
{
    const char *e = getenv("RECOMP_GUEST_LOCK");
    const char *t = getenv("RECOMP_GUEST_CPU_TRACE");
    const char *q = getenv("RECOMP_GUEST_QUANTUM");
    long qms = q && *q ? strtol(q, NULL, 0) : 4;

    s_on = e && *e == '1';
    s_trace_ns = t ? (uint64_t)(*t ? strtoul(t, NULL, 0) : 1) * 1000000ull : 0;
    s_quantum_ns = (uint64_t)(qms >= 0 ? qms : 4) * 1000000ull;   /* 0: hand over at every call */
    if (s_on) {
        fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs"
                        " guest code at a time, %lu ms quantum\n",
                (unsigned long)(s_quantum_ns / 1000000ull));
        fflush(stderr);
    }
}

/* The config is read here, under the INIT_ONCE, so no thread sees s_on set
 * before s_quantum_ns: a lazy read outside it let a second thread at boot
 * find the lock on with a 0 quantum. */
static BOOL CALLBACK guest_cpu_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    guest_cpu_config();
    return TRUE;
}

int xbox_GuestCpuOn(void)
{
    InitOnceExecuteOnce(&s_once, guest_cpu_init, NULL, NULL);
    return s_on;
}

void xbox_GuestCpuReloadConfig(void)
{
    InitOnceExecuteOnce(&s_once, guest_cpu_init, NULL, NULL);
    guest_cpu_config();
}

static void raise_max(volatile LONG *slot, LONG v)
{
    LONG cur;
    while (v > (cur = *slot))
        if (InterlockedCompareExchange(slot, v, cur) == cur)
            break;
}

/* Where a turn ended: the kernel ordinal called, the loop back edge's VA,
 * or the end of a callback or of the thread. */
static const char *where_str(int kind, uint32_t where, char *buf, size_t n)
{
    switch (kind) {
    case XBOX_GUEST_CPU_KERNEL:
        snprintf(buf, n, "ordinal %u", where);
        break;
    case XBOX_GUEST_CPU_CALLBACK:
        snprintf(buf, n, "callback end");
        break;
    case XBOX_GUEST_CPU_PREEMPT:
        snprintf(buf, n, "back edge 0x%08X", where);
        break;
    case XBOX_GUEST_CPU_EXIT:
        snprintf(buf, n, "thread end");
        break;
    default:
        snprintf(buf, n, "?");
        break;
    }
    return buf;
}

static void turn_ended(uint64_t end_ns, int kind, uint32_t where)
{
    LONG us = (LONG)((end_ns - t_turn_ns) / 1000u);

    t_turn_open = 0;
    InterlockedIncrement(&s_turns);
    if (us > s_max_turn_us) {
        /* Three words, racy between threads; the summary is a diagnostic,
         * and a torn record names a turn that did happen. */
        raise_max(&s_max_turn_us, us);
        s_max_turn_kind = kind;
        s_max_turn_where = (LONG)where;
    }
    if (s_trace_ns && (uint64_t)us * 1000u >= s_trace_ns) {
        uint64_t now = xbox_HostNowNs();
        /* The first 50, then one a second: a frame loop can cross the
         * threshold on every pass. */
        if (InterlockedIncrement(&s_trace_lines) <= 50
            || now - s_trace_last_ns >= 1000000000ull) {
            char w[32];
            s_trace_last_ns = now;
            fprintf(stderr, "  [GUEST] cpu held %.2f ms by tid %lu, released at"
                    " %s\n", us / 1000.0, (unsigned long)GetCurrentThreadId(),
                    where_str(kind, where, w, sizeof w));
            fflush(stderr);
        }
    }
}

/* Take the CPU. barge: a returning caller (back from a kernel call or into
 * a callback) takes a free, ungranted CPU past the queue, and when the CPU
 * is held it asks for it now: its wait just completed, and the console's
 * dispatcher boosts a thread out of a wait (KeSetEvent's increment, the I/O
 * completion's) above the thread it interrupted, so the woken thread runs
 * at once and the holder waits. A joining or handing-over thread (a
 * preemption) queues behind every waiter and asks only past the quantum,
 * the round robin of equal priorities. */
static void guest_cpu_acquire(int barge)
{
    unsigned my;
    uint64_t t0 = 0;

    EnterCriticalSection(&s_lock);
    if (s_free && !s_granted && (barge || !s_waiting)) {
        s_free = 0;
    } else {
        my = s_next_ticket++;
        s_waiting++;
        t0 = xbox_HostNowNs();
        if (barge && !s_free)
            preempt_set(1);
        while (!(s_free && my == s_head)) {
            SleepConditionVariableCS(&s_cv, &s_lock, GUEST_CPU_POLL_MS);
            /* Ask the holder to hand over at its next loop back edge
             * (RECOMP_BACK_EDGE) or kernel call: a woken thread at once,
             * a queued one past the quantum (the console's timer tick).
             * The holder clears the flag when it cannot (at DISPATCH),
             * and the waiter raises it again at its next poll. */
            if (!preempt_get() && !(s_free && my == s_head)
                && (barge || xbox_HostNowNs() - t0 >= s_quantum_ns))
                preempt_set(1);
        }
        s_free = 0;
        s_granted = 0;
        s_head++;
        s_waiting--;
    }
    InterlockedExchange(&s_owner_tid, (LONG)GetCurrentThreadId());
    LeaveCriticalSection(&s_lock);
    t_held = 1;
    if (t0) {
        uint64_t now = xbox_HostNowNs();
        InterlockedIncrement(&s_waits);
        raise_max(&s_max_wait_us, (LONG)((now - t0) / 1000u));
        /* Someone else ran: the turn that was open ended at its soft
         * release, and a new one starts now. */
        if (t_turn_open) {
            InterlockedIncrement(&s_switches);
            turn_ended(t_soft_ns, t_soft_kind, t_soft_where);
        }
        t_turn_ns = now;
        t_turn_open = 1;
    } else if (!t_turn_open) {
        t_turn_ns = xbox_HostNowNs();
        t_turn_open = 1;
    }
}

/* Hand the CPU over: the oldest waiter is granted it, and nobody barges
 * until it has taken it. The turn ends here. */
static void guest_cpu_handoff(int kind, uint32_t where)
{
    t_held = 0;
    turn_ended(xbox_HostNowNs(), kind, where);
    EnterCriticalSection(&s_lock);
    preempt_set(0);                     /* the waiter's turn comes now */
    s_free = 1;
    InterlockedExchange(&s_owner_tid, 0);
    if (s_waiting) {
        s_granted = 1;
        WakeAllConditionVariable(&s_cv);
    }
    LeaveCriticalSection(&s_lock);
}

/* Let go for a kernel call: free, taken straight back on return unless a
 * waiter got there first. The waiters are told only when the call may
 * block; otherwise their own 2 ms look finds it, should the call take that
 * long. */
static void guest_cpu_soft_release(int kind, uint32_t where, int wake)
{
    t_held = 0;
    InterlockedIncrement(&s_releases);
    t_soft_ns = xbox_HostNowNs();
    t_soft_kind = kind;
    t_soft_where = where;
    EnterCriticalSection(&s_lock);
    s_free = 1;
    InterlockedExchange(&s_owner_tid, 0);
    if (wake && s_waiting)
        WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

/* The kernel calls that can block the caller: the waits and sleeps, file
 * I/O, a thread suspend. NtYieldExecution is a hand-off in its own right. */
static int ordinal_may_block(uint32_t ordinal)
{
    switch (ordinal) {
    case 99:                            /* KeDelayExecutionThread */
    case 158: case 159:                 /* KeWaitFor{Multiple,Single}Object(s) */
    case 190: case 202:                 /* NtCreateFile, NtOpenFile */
    case 196: case 198: case 200:       /* NtDeviceIoControlFile, NtFlushBuffersFile, NtFsControlFile */
    case 207:                           /* NtQueryDirectoryFile */
    case 219: case 220:                 /* NtReadFile, NtReadFileScatter */
    case 223:                           /* NtRemoveIoCompletion */
    case 230:                           /* NtSignalAndWaitForSingleObjectEx */
    case 231:                           /* NtSuspendThread */
    case 233: case 234: case 235:       /* NtWaitFor{Single,SingleEx,MultipleEx} */
    case 236: case 237:                 /* NtWriteFile, NtWriteFileGather */
        return 1;
    default:
        return 0;
    }
}

/* ── The entry points (kernel.h) ─────────────────────────── */

void xbox_GuestCpuJoin(void)
{
    t_guest = 1;
    if (xbox_GuestCpuOn() && !t_held)
        guest_cpu_acquire(0);
}

void xbox_GuestCpuPart(void)
{
    if (t_held)
        guest_cpu_handoff(XBOX_GUEST_CPU_EXIT, 0);
    t_guest = 0;
}

int xbox_GuestCpuHeld(void)
{
    return t_held;
}

int xbox_GuestCpuIsGuestThread(void)
{
    return t_guest;
}

int xbox_GuestCpuRelease(int kind, uint32_t where)
{
    if (!t_held)
        return 0;
    if (kind == XBOX_GUEST_CPU_KERNEL) {
        if (where == 258 /* PsTerminateSystemThread: ExitThread, no return */)
            guest_cpu_handoff(XBOX_GUEST_CPU_EXIT, where);
        /* Not at DISPATCH: the thunk gets here at every ordinal, KfLowerIrql
         * and KeReleaseSpinLock included, while the caller still holds the
         * one-CPU gate; a hand-off there queues the gate holder behind
         * every waiter, and the waiter's own KfRaiseIrql then blocks on the
         * gate. The call soft-releases and barges back, as the back edge
         * refuses to hand over there. */
        else if (where == 238 /* NtYieldExecution */
            || (s_waiting && !xbox_IrqlThisThreadBlocksDpcs()
                && (preempt_get()
                    || xbox_HostNowNs() - t_turn_ns >= s_quantum_ns)))
            guest_cpu_handoff(kind, where);
        else
            guest_cpu_soft_release(kind, where, ordinal_may_block(where));
    } else {
        guest_cpu_soft_release(kind, where, 0);
    }
    return 1;
}

void xbox_GuestCpuAcquire(void)
{
    if (!t_held && xbox_GuestCpuOn())
        guest_cpu_acquire(1);
}

/* A bridge about to block on the host inside a kernel call that usually
 * does not (a contended critical section: the thread that holds it needs
 * the CPU to leave it): the CPU let go of at the call stays free for a
 * while, so the waiters learn now instead of at their next poll. */
void xbox_GuestCpuBlocked(void)
{
    if (t_held || !t_guest || !s_waiting || !xbox_GuestCpuOn())
        return;
    EnterCriticalSection(&s_lock);
    if (s_free && s_waiting)
        WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

/* A loop back edge with the preempt flag set: the holder past its quantum
 * hands over and queues behind the waiters. Not at DISPATCH (the console
 * switched no threads there) and not on a thread that does not hold the CPU
 * (a host thread's callback, the lock off); the flag is cleared when
 * nothing is to be done, and the waiter sets it again at its next poll if
 * it still waits. */
void xbox_GuestCpuPreempt(uint32_t va)
{
    if (!t_held)
        return;
    if (!s_waiting || xbox_IrqlThisThreadBlocksDpcs()) {
        preempt_set(0);
        return;
    }
    InterlockedIncrement(&s_preempts);
    guest_cpu_handoff(XBOX_GUEST_CPU_PREEMPT, va);
    guest_cpu_acquire(0);
}

/* Guest code a bridge runs on a guest thread (an unwind handler, a
 * KeSynchronizeExecution routine, a worker run inline) is thread code on
 * the console too, so it holds the CPU; the bridge let go of it at the
 * call. 1 when this took it, for the leave. */
int xbox_GuestCpuEnterCallback(void)
{
    if (!t_guest || t_held || !xbox_GuestCpuOn())
        return 0;
    guest_cpu_acquire(1);
    return 1;
}

void xbox_GuestCpuLeaveCallback(int took)
{
    if (took && t_held)
        guest_cpu_soft_release(XBOX_GUEST_CPU_CALLBACK, 0, 0);
}

unsigned long xbox_GuestCpuOwner(void)
{
    return (unsigned long)InterlockedCompareExchange(&s_owner_tid, 0, 0);
}

int xbox_GuestCpuWaiting(void)
{
    int n;

    if (!xbox_GuestCpuOn())
        return 0;
    EnterCriticalSection(&s_lock);
    n = (int)s_waiting;
    LeaveCriticalSection(&s_lock);
    return n;
}

void xbox_GuestCpuStatsGet(XboxGuestCpuStats *out, int reset)
{
#define GC_TAKE(f) (reset ? InterlockedExchange(&(f), 0) : (f))
    out->turns = GC_TAKE(s_turns);
    out->releases = GC_TAKE(s_releases);
    out->switches = GC_TAKE(s_switches);
    out->waits = GC_TAKE(s_waits);
    out->preempts = GC_TAKE(s_preempts);
    out->max_turn_us = GC_TAKE(s_max_turn_us);
    out->max_wait_us = GC_TAKE(s_max_wait_us);
    out->max_turn_kind = (int)GC_TAKE(s_max_turn_kind);
    out->max_turn_where = (uint32_t)GC_TAKE(s_max_turn_where);
#undef GC_TAKE
}

/* Beside the [NV2A] vblank line, once per 600 vblanks, when the lock is on. */
void xbox_GuestCpuReport(void)
{
    XboxGuestCpuStats s;
    char w[32];

    if (!xbox_GuestCpuOn())
        return;
    xbox_GuestCpuStatsGet(&s, 1);
    if (!s.turns && !s.releases)
        return;
    fprintf(stderr, "  [GUEST] cpu: turns %ld (preempts %ld), kernel calls %ld"
            " of which switched %ld, waits %ld; longest turn %.2f ms (%s),"
            " longest wait %.2f ms\n",
            s.turns, s.preempts, s.releases, s.switches, s.waits,
            s.max_turn_us / 1000.0,
            where_str(s.max_turn_kind, s.max_turn_where, w, sizeof w),
            s.max_wait_us / 1000.0);
    fflush(stderr);
}
