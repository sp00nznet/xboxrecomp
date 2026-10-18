/*
 * kernel_guest_cpu.c - the guest CPU (RECOMP_GUEST_LOCK=1).
 *
 * The Xbox has one CPU: two guest threads never run guest code at the same
 * instant, and titles lean on that without knowing it. BLiNX 2's stage
 * loader and its loader thread add lights to one global pool with
 * "slot = count; fill; count = slot + 1" (sub_00037CE0); on two host cores
 * the two lost each other's entries and the terrain baked black (the dark
 * water of 2026-10-07). Pinning the title's threads to one core
 * (xbox_GuestThreadPin) makes that as rare as a console quantum boundary
 * where the host has thread affinity; macOS has none.
 *
 * Upstream's answer (v0.13.1, #160) is this lock: a guest thread holds it
 * while it runs guest code and lets go for the length of every kernel call.
 * Host threads that only run a guest callback (the timer thread's DPCs, the
 * device models' ISRs) never take it: an interrupt ran between any two
 * instructions of any thread on the console, and the one-CPU gate
 * (kernel_hal.c) already orders them against guest code at DISPATCH_LEVEL.
 * Taking it for a DPC would also deadlock a guest thread spinning on the
 * DPC's result.
 *
 * What the lock hands over, and when, is the console's scheduler, and a
 * plain critical section is not it (BLiNX 2's attract: 32 flips/s with the
 * lock off, 28.5 with upstream's critical section, 12.7 with a fair ticket
 * lock that switched at every kernel call):
 *  - a kernel call that returns at once switches nobody. The title makes a
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
 *    at its next kernel call, whatever the call: the oldest waiter is
 *    granted the CPU, barging is refused until it has taken it, and the
 *    releaser queues behind every waiter. Without this the unfair critical
 *    section let a worker keep the CPU through a whole decode burst while
 *    the main thread lost frames (p95 70 ms).
 *  - a guest thread with nothing to do hands over at once: a lowered
 *    spin-wait's pass (templates/runtime/recomp_types.h, the macro's
 *    g_xbox_guest_cpu_waiters flag), NtYieldExecution, the thread's end.
 *    That is the console's behaviour, and the end of upstream's deadlock
 *    note (a guest loop spinning on another guest thread's flag) for every
 *    loop in spin_waits.json.
 *
 * ponytail: a guest thread that computes for long with no kernel call and no
 * lowered loop holds the others off for that long; the console switched at
 * its quantum wherever the thread was. The pacing summary's longest turn
 * (xbox_GuestCpuReport) and RECOMP_TRACE=guest_cpu name such a hold and
 * where it ended; a guest loop goes in spin_waits.json, a long computation
 * is the case for a yield at the lifted code's loop back edges, behind its
 * own switch, not paid for here.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "platform/host_time.h"
#include "kernel_pacing.h"
#include <stdio.h>
#include <string.h>

/* Read by RECOMP_SPIN_WAIT on every pass of a lowered loop: threads blocked
 * in the queue. An int, not a LONG, for the generated code. */
volatile int g_xbox_guest_cpu_waiters;
/* Read by RECOMP_BACK_EDGE at every loop back edge of lifted code: a waiter
 * has waited its quantum, so the holder hands over at the edge. Set by the
 * waiter at its poll, cleared by the hand-off; the one relaxed load the
 * check costs is the price of a turn bounded without a kernel call. */
volatile int g_xbox_guest_cpu_preempt;

#define GUEST_CPU_POLL_MS 2             /* a waiter's own look at a free CPU */

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static int s_on = -1;
static uint64_t s_trace_ns;                 /* guest_cpu=ms; 0 = off */
static uint64_t s_quantum_ns;               /* guest_quantum=ms */
static int s_account;                       /* per-thread table and CPU clock: pacing or guest_cpu trace on */

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
static XBOX_THREAD_LOCAL uint64_t t_turn_cpu0;  /* the thread's CPU clock at the turn's start */

/* Per-thread totals for the window's report: a turn's wall time against
 * the CPU time the thread burnt in it tells a thread that computes from one
 * that holds the CPU while it waits for the host (a trap, a spin that is
 * not lowered), and the waits tell who is kept off the CPU and how long. */
#define GUEST_CPU_THREADS 16
typedef struct {
    volatile LONG tid;                  /* 0: free slot */
    volatile LONG turns, held_us, cpu_us, max_turn_us, max_turn_cpu_us;
    volatile LONG max_turn_kind, max_turn_where;
    volatile LONG waits, wait_us, max_wait_us, yields, preempts;
    volatile LONG releases, handoffs;   /* kernel calls that let go; of the turns, ended by this thread's own hand-off */
    /* Inside a turn: the longest kernel call taken straight back (nobody
     * took the CPU while it ran) and the longest run of guest code between
     * two lets-go, with the ordinal that ended it. A long turn with little
     * CPU is one or the other: a call that blocked with no taker, or guest
     * code off the CPU (a page fault, a host wait no release covers). */
    volatile LONG max_call_us, max_call_where, max_run_us, max_run_where;
} GuestCpuThread;
static XBOX_THREAD_LOCAL uint64_t t_run_ns;    /* when this thread last got the CPU back */
static GuestCpuThread s_threads[GUEST_CPU_THREADS];
static XBOX_THREAD_LOCAL GuestCpuThread *t_rec;

/* This thread's slot, claimed by tid on first use and never freed: a
 * title that creates and ends more than 16 guest threads leaves the later
 * ones uncounted (NULL), which is a diagnostic's limit, not the lock's. */
static GuestCpuThread *thread_rec(void)
{
    LONG tid = (LONG)GetCurrentThreadId();
    int i;

    if (t_rec)
        return t_rec;
    if (!s_account)
        return NULL;
    for (i = 0; i < GUEST_CPU_THREADS; i++) {
        LONG seen = InterlockedCompareExchange(&s_threads[i].tid, tid, 0);
        if (seen == 0 || seen == tid) {
            t_rec = &s_threads[i];
            return t_rec;
        }
    }
    return NULL;                        /* more threads than slots: uncounted */
}

static void thread_add(volatile LONG *sum, LONG v)
{
    InterlockedExchangeAdd(sum, v);
}

/* The run of guest code since the thread last got the CPU back ends at a
 * let-go (where: the ordinal or site that ends it). */
static void run_ended(GuestCpuThread *r, uint64_t now, uint32_t where)
{
    LONG us = (LONG)((now - t_run_ns) / 1000u);
    if (r && t_run_ns && us > r->max_run_us) {
        r->max_run_us = us;
        r->max_run_where = (LONG)where;
    }
}

/* The window's counters (xbox_GuestCpuReport resets them). */
static volatile LONG s_turns, s_releases, s_switches, s_waits, s_yields, s_preempts;
static volatile LONG s_max_turn_us, s_max_wait_us;
static volatile LONG s_max_turn_kind, s_max_turn_where;
static volatile LONG s_trace_lines;
static uint64_t s_trace_last_ns;

static BOOL CALLBACK guest_cpu_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    return TRUE;
}

static void guest_cpu_config(void)
{
    const char *e = recomp_env(RENV_GUEST_LOCK);

    s_on = e && *e == '1';
    s_trace_ns = recomp_env(RENV_GUEST_CPU_TRACE)
                 ? (uint64_t)recomp_env_int(RENV_GUEST_CPU_TRACE, 1) * 1000000ull : 0;
    s_quantum_ns = (uint64_t)recomp_env_int(RENV_GUEST_QUANTUM, 4) * 1000000ull;
    /* The thread CPU clock is GetThreadTimes under Wine, a wineserver
     * round trip, and a turn ends thousands of times a second: read it,
     * and keep the per-thread table, only for a trace that prints them. */
    s_account = s_trace_ns != 0 || xbox_PacingTraceOn();
    if (s_on) {
        fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs"
                        " guest code at a time, %lu ms quantum"
                        " (RECOMP_GUEST_LOCK=0: every thread at once)\n",
                (unsigned long)(s_quantum_ns / 1000000ull));
        fflush(stderr);
    }
}

int xbox_GuestCpuOn(void)
{
    InitOnceExecuteOnce(&s_once, guest_cpu_init, NULL, NULL);
    if (s_on < 0)
        guest_cpu_config();
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

/* Where a turn ended: the kernel ordinal called, the lowered loop's VA, or
 * the end of a callback or of the thread. */
static const char *where_str(int kind, uint32_t where, char *buf, size_t n)
{
    switch (kind) {
    case XBOX_GUEST_CPU_KERNEL:
        snprintf(buf, n, "ordinal %u", where);
        break;
    case XBOX_GUEST_CPU_SPIN:
        snprintf(buf, n, "spin 0x%08X", where);
        break;
    case XBOX_GUEST_CPU_CALLBACK:
        snprintf(buf, n, "callback end");
        break;
    case XBOX_GUEST_CPU_HOST:
        snprintf(buf, n, "host wait 0x%08X", where);
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

/* A turn ended at end_ns, at kind/where: the record is this thread's own,
 * read from no other thread. */
static void turn_ended(uint64_t end_ns, int kind, uint32_t where)
{
    LONG us = (LONG)((end_ns - t_turn_ns) / 1000u);
    GuestCpuThread *r = thread_rec();
    LONG cpu_us = r ? (LONG)((xbox_ThreadCpuNs() - t_turn_cpu0) / 1000u) : 0;

    t_turn_open = 0;
    InterlockedIncrement(&s_turns);
    if (r) {
        InterlockedIncrement(&r->turns);
        thread_add(&r->held_us, us);
        thread_add(&r->cpu_us, cpu_us);
        if (us > r->max_turn_us) {
            r->max_turn_us = us;
            r->max_turn_cpu_us = cpu_us;
            r->max_turn_kind = kind;
            r->max_turn_where = (LONG)where;
        }
    }
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

/* Take the CPU. barge: a returning caller (back from a kernel call, a host
 * wait or into a callback) takes a free, ungranted CPU past the queue, and
 * when the CPU is held it asks for it now: its wait just completed, and
 * the console's dispatcher boosts a thread out of a wait (KeSetEvent's
 * increment, the I/O completion's) above the thread it interrupted, so
 * the woken thread runs at once and the holder waits. A joining or
 * handing-over thread (a yield, a preemption) queues behind every waiter
 * and asks only past the quantum, the round robin of equal priorities. */
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
        g_xbox_guest_cpu_waiters = (int)s_waiting;
        t0 = xbox_HostNowNs();
        if (barge && !s_free)
            g_xbox_guest_cpu_preempt = 1;
        while (!(s_free && my == s_head)) {
            SleepConditionVariableCS(&s_cv, &s_lock, GUEST_CPU_POLL_MS);
            /* Ask the holder to hand over at its next loop back edge
             * (RECOMP_BACK_EDGE) or kernel call: a woken thread at once,
             * a queued one past the quantum (the console's timer tick).
             * The holder clears the flag when it cannot (at DISPATCH),
             * and the waiter raises it again at its next poll. */
            if (!g_xbox_guest_cpu_preempt && !(s_free && my == s_head)
                && (barge || xbox_HostNowNs() - t0 >= s_quantum_ns))
                g_xbox_guest_cpu_preempt = 1;
        }
        s_free = 0;
        s_granted = 0;
        s_head++;
        s_waiting--;
        g_xbox_guest_cpu_waiters = (int)s_waiting;
    }
    InterlockedExchange(&s_owner_tid, (LONG)GetCurrentThreadId());
    LeaveCriticalSection(&s_lock);
    t_held = 1;
    if (t0) {
        uint64_t now = xbox_HostNowNs();
        LONG wait_us = (LONG)((now - t0) / 1000u);
        GuestCpuThread *r = thread_rec();
        InterlockedIncrement(&s_waits);
        raise_max(&s_max_wait_us, wait_us);
        if (r) {
            InterlockedIncrement(&r->waits);
            thread_add(&r->wait_us, wait_us);
            raise_max(&r->max_wait_us, wait_us);
        }
        /* Someone else ran: the turn that was open ended at its soft
         * release, and a new one starts now. */
        if (t_turn_open) {
            InterlockedIncrement(&s_switches);
            turn_ended(t_soft_ns, t_soft_kind, t_soft_where);
        }
        t_turn_ns = now;
        if (r)
            t_turn_cpu0 = xbox_ThreadCpuNs();
        t_turn_open = 1;
    } else if (!t_turn_open) {
        t_turn_ns = xbox_HostNowNs();
        if (s_account)
            t_turn_cpu0 = xbox_ThreadCpuNs();
        t_turn_open = 1;
    } else {
        /* Taken straight back: the call ran inside the turn. */
        GuestCpuThread *r = thread_rec();
        uint64_t now = xbox_HostNowNs();
        LONG us = (LONG)((now - t_soft_ns) / 1000u);
        if (r && us > r->max_call_us) {
            r->max_call_us = us;
            r->max_call_where = (LONG)t_soft_where;
        }
    }
    t_run_ns = xbox_HostNowNs();
}

/* Hand the CPU over: the oldest waiter is granted it, and nobody barges
 * until it has taken it. The turn ends here. */
static void guest_cpu_handoff(int kind, uint32_t where)
{
    GuestCpuThread *r = thread_rec();
    uint64_t now = xbox_HostNowNs();

    t_held = 0;
    if (r)
        InterlockedIncrement(&r->handoffs);
    run_ended(r, now, where);
    turn_ended(now, kind, where);
    EnterCriticalSection(&s_lock);
    g_xbox_guest_cpu_preempt = 0;       /* the waiter's turn comes now */
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
    GuestCpuThread *r = thread_rec();
    uint64_t now = xbox_HostNowNs();

    t_held = 0;
    run_ended(r, now, where);
    InterlockedIncrement(&s_releases);
    if (r)
        InterlockedIncrement(&r->releases);
    t_soft_ns = now;
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
        /* Not at DISPATCH (D3): the thunk gets here at every ordinal,
         * KfLowerIrql and KeReleaseSpinLock included, while the caller
         * still holds the DISPATCH gate; a hand-off there queues the gate
         * holder behind every waiter, and the waiter's own KfRaiseIrql then
         * blocks on the gate. The call soft-releases and barges back, as
         * the back edge and the spin-wait pass refuse to hand over there. */
        else if (where == 238 /* NtYieldExecution */
            || (s_waiting && !xbox_IrqlThisThreadBlocksDpcs()
                && (g_xbox_guest_cpu_preempt
                    || xbox_HostNowNs() - t_turn_ns >= s_quantum_ns)))
            guest_cpu_handoff(kind, where);
        else
            guest_cpu_soft_release(kind, where, ordinal_may_block(where));
    } else if (kind == XBOX_GUEST_CPU_HOST) {
        /* About to block on the host for a while (a vblank, say) with no
         * kernel call to let go in: the thread is off the CPU as a waiting
         * thread is on the console, so the oldest waiter gets it now. */
        if (s_waiting)
            guest_cpu_handoff(kind, where);
        else
            guest_cpu_soft_release(kind, where, 1);
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

/* A lowered loop's pass with another guest thread waiting: hand the CPU to
 * it and queue behind every waiter. Nothing when this thread does not hold
 * the CPU (the lock is off, or a host thread runs the loop). */
int xbox_GuestCpuYield(uint32_t va)
{
    GuestCpuThread *r;

    if (!t_held || !s_waiting)
        return 0;
    InterlockedIncrement(&s_yields);
    if ((r = thread_rec()) != NULL)
        InterlockedIncrement(&r->yields);
    guest_cpu_handoff(XBOX_GUEST_CPU_SPIN, va);
    guest_cpu_acquire(0);
    return 1;
}

/* A loop back edge with the preempt flag set: the holder past its quantum
 * hands over, as at a lowered loop's pass, and queues behind the waiters.
 * Not at DISPATCH (D3: the console switched no threads there) and not on a
 * thread that does not hold the CPU (a host thread's callback, the lock
 * off); the flag is cleared when nothing is to be done, and the waiter sets
 * it again at its next poll if it still waits. */
void xbox_GuestCpuPreempt(uint32_t va)
{
    GuestCpuThread *r;

    if (!t_held)
        return;
    if (!s_waiting || xbox_IrqlThisThreadBlocksDpcs()) {
        g_xbox_guest_cpu_preempt = 0;
        return;
    }
    InterlockedIncrement(&s_preempts);
    if ((r = thread_rec()) != NULL)
        InterlockedIncrement(&r->preempts);
    guest_cpu_handoff(XBOX_GUEST_CPU_PREEMPT, va);
    guest_cpu_acquire(0);
}

/* Guest code a bridge runs on a guest thread (an APC routine, an unwind
 * handler, a KeSynchronizeExecution routine, a worker run inline) is thread
 * code on the console too, so it holds the CPU; the bridge let go of it at
 * the call. 1 when this took it, for the leave. */
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

void xbox_GuestCpuStatsGet(XboxGuestCpuStats *out, int reset)
{
#define GC_TAKE(f) (reset ? InterlockedExchange(&(f), 0) : (f))
    out->turns = GC_TAKE(s_turns);
    out->releases = GC_TAKE(s_releases);
    out->switches = GC_TAKE(s_switches);
    out->waits = GC_TAKE(s_waits);
    out->yields = GC_TAKE(s_yields);
    out->preempts = GC_TAKE(s_preempts);
    out->max_turn_us = GC_TAKE(s_max_turn_us);
    out->max_wait_us = GC_TAKE(s_max_wait_us);
    out->max_turn_kind = (int)GC_TAKE(s_max_turn_kind);
    out->max_turn_where = (uint32_t)GC_TAKE(s_max_turn_where);
#undef GC_TAKE
}

/* Under the [PACING] summary, every 600 flips, when the lock is on. */
void xbox_GuestCpuReport(void)
{
    XboxGuestCpuStats s;
    char w[32];

    if (!xbox_GuestCpuOn())
        return;
    xbox_GuestCpuStatsGet(&s, 1);
    if (!s.turns && !s.releases)
        return;
    fprintf(stderr, "[PACING]   guest cpu: turns %ld (yields %ld, preempts %ld),"
            " kernel calls %ld of which switched %ld, waits %ld; longest turn"
            " %.2f ms (%s), longest wait %.2f ms\n",
            s.turns, s.yields, s.preempts, s.releases, s.switches, s.waits,
            s.max_turn_us / 1000.0,
            where_str(s.max_turn_kind, s.max_turn_where, w, sizeof w),
            s.max_wait_us / 1000.0);
    for (int i = 0; i < GUEST_CPU_THREADS; i++) {
        GuestCpuThread *r = &s_threads[i];
        LONG tid = r->tid;
        if (!tid || (!r->turns && !r->waits && !r->releases))
            continue;
        fprintf(stderr, "[PACING]     tid %ld: calls %ld, turns %ld (%ld own hand-offs)"
                " held %.1f ms cpu %.1f ms"
                " (longest %.2f ms, cpu %.2f, %s), yields %ld, preempts %ld, waits %ld"
                " %.1f ms (longest %.2f); longest call %.2f ms (%ld),"
                " longest run %.2f ms (to %ld)\n",
                (long)tid, (long)r->releases, (long)r->turns, (long)r->handoffs,
                r->held_us / 1000.0, r->cpu_us / 1000.0,
                r->max_turn_us / 1000.0, r->max_turn_cpu_us / 1000.0,
                where_str((int)r->max_turn_kind, (uint32_t)r->max_turn_where, w, sizeof w),
                (long)r->yields, (long)r->preempts, (long)r->waits, r->wait_us / 1000.0,
                r->max_wait_us / 1000.0, r->max_call_us / 1000.0,
                (long)r->max_call_where, r->max_run_us / 1000.0,
                (long)r->max_run_where);
        r->turns = r->held_us = r->cpu_us = r->max_turn_us = r->max_turn_cpu_us = 0;
        r->waits = r->wait_us = r->max_wait_us = r->yields = r->preempts = 0;
        r->releases = r->handoffs = 0;
        r->max_call_us = r->max_call_where = r->max_run_us = r->max_run_where = 0;
    }
    fflush(stderr);
}
