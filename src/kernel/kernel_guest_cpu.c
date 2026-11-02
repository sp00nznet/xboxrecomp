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
 *
 * guest_sched=1 adds the console's priorities (the GUEST SCHED section
 * below): the queue is ordered by priority, then by ticket, and every
 * hand-off above asks first whether the waiter outranks or equals the
 * holder. Off, none of it runs and the guest CPU is the FIFO described here.
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
static int s_on = -1;
static uint64_t s_trace_ns;                 /* guest_cpu=ms; 0 = off */
static uint64_t s_quantum_ns;               /* guest_quantum=ms */
static int s_account;                       /* per-thread table and CPU clock: pacing or guest_cpu trace on */
static int s_sched;                         /* guest_sched: priorities order the guest CPU */

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
static volatile LONG s_max_wait_run_us;     /* never reset: the run's longest */
static volatile LONG s_max_turn_kind, s_max_turn_where;
static volatile LONG s_trace_lines;
static uint64_t s_trace_last_ns;

/* ── GUEST SCHED (guest_sched=1) ─────────────────────────────
 *
 * The console ran the highest-priority ready thread; a thread made ready
 * above the running one preempted it at once, and equal priorities took
 * turns at the quantum. Titles lock by it: CRI ADX (JSRF sub_0013B0A0,
 * BLiNX 2 sub_002ACBA0) raises itself to time critical and so keeps every
 * title thread off the CPU until it drops back, and JSRF's wait helper
 * sub_001437B0 asks "GetThreadPriority(self) == 15" to learn it is inside.
 * Host priorities cannot give that under the lock: the host runs whichever
 * thread holds the guest CPU, so the guest CPU itself has to know them.
 *
 * A thread is known by its guest thread object (kernel_bridge.c's thobj):
 * the bridges set and read priorities and suspend threads by it, and a
 * guest thread binds itself to it before its first instruction. Everything
 * here is under s_lock. A thread with no record (the table full, a host
 * thread) runs at the process base, 8.
 *
 * A lower waiter takes the CPU a holder let go of at a kernel call only
 * when the call blocks: a sleep or file I/O at once, a wait or a contended
 * critical section when its bridge finds it must block
 * (xbox_GuestCpuBlocked), and any other call once it has run past 2 ms.
 * The last is a guess at a block nobody reported, and inside a
 * time-critical holder it opens the lock, so those admissions are counted
 * for the pacing summary and, at 15, named. */
#define GUEST_SCHED_THREADS 64
#define GUEST_SCHED_SOFT_NS 2000000ull     /* a kernel call this long counts as blocked */
#define GUEST_SCHED_STARVE_NS 1000000000ull /* a wait this long is named */
/* The hang guard: a waiter kept off this long gets one turn (D7). Above
 * every wait seen on a title so far (BLiNX 2's ADX worker, about 1 s), so
 * it fires only where the model has a gap: a higher thread spinning with no
 * kernel call on work the console did in an ISR, which no quantum rescues
 * once priorities order the CPU. Every firing is logged; goldens want 0. */
#define GUEST_SCHED_VALVE_NS 4000000000ull
#define GUEST_SCHED_SUSPENDED_JOIN_NS 5000000000ull /* a created-suspended thread still held */

typedef struct {
    uint32_t obj;                       /* 0: free */
    LONG inc;                           /* base priority increment, NT-normalised */
    LONG suspend;                       /* guest-side suspend count */
    int gone;                           /* the object was freed while its thread was queued or held */
} GuestSchedThread;

typedef struct GuestSchedWaiter {
    unsigned ticket;
    GuestSchedThread *t;
    int barge;
    uint64_t t0;
    struct GuestSchedWaiter *next;
} GuestSchedWaiter;

static GuestSchedThread s_sched_threads[GUEST_SCHED_THREADS];
static XBOX_THREAD_LOCAL GuestSchedThread *t_sched;
static GuestSchedWaiter *s_wait_head, *s_wait_tail;
static unsigned s_sched_ticket;
static GuestSchedThread *s_holder;          /* the holder's record; NULL: free, or none */
/* The last soft release: who let go for a kernel call, at what level, and
 * whether the call may block. A lower waiter waits for it to come back
 * while the call is short; a higher or equal one may take the CPU. */
static unsigned long s_soft_tid;            /* 0: none outstanding */
static int s_soft_level, s_soft_blocking, s_soft_kind;
static unsigned long s_last_tid;             /* who let go of the CPU last (lines) */
static int s_last_level;
static uint32_t s_soft_where;
static uint64_t s_soft_at_ns;
static volatile LONG s_refused;
static volatile LONG s_starve_lines;
static uint64_t s_starve_last_ns;
/* For the starvation line: turns taken, and soft releases that let any
 * level in (a blocking call), counted under s_lock. */
static unsigned long s_sched_takes, s_sched_blocking;
/* Where the holder last let go or kept the CPU at a hand-off point: the
 * starvation line says where a long hold is. */
static int s_site_kind;
static uint32_t s_site_where;
/* The soft window and the valve, wider in the ctests (xbox_GuestSchedTestWindows). */
static uint64_t s_soft_ns = GUEST_SCHED_SOFT_NS, s_valve_ns = GUEST_SCHED_VALVE_NS;

/* Admissions past the soft window: a lower thread took the CPU only because
 * a kernel call that is not on the may-block list ran past 2 ms. Inside a
 * time-critical holder that breaks its lock (a slow bridge under Wine, a
 * host wait that does not say it blocks), so they are counted by ordinal
 * and level for the pacing summary, and named at once when the holder was
 * at 15. Under s_lock; a full table drops the rest into the last row. */
#define GUEST_SCHED_LATE_ROWS 8
typedef struct { int kind, level; uint32_t where; long n; } GuestSchedLate;
static GuestSchedLate s_late[GUEST_SCHED_LATE_ROWS];
static long s_late_lines;
static uint64_t s_late_last_ns;

/* The valve: the waiter it chose, the turn it gave and how often it fired
 * (the window's count for the summary, the run's for the ctests). */
static struct GuestSchedWaiter *s_valve;
static unsigned long s_valve_tid;
static uint64_t s_valve_until_ns;
static volatile LONG s_valves, s_valves_total;
static int s_table_full_said;

/* A line formatted under s_lock, printed by the caller once it has let go:
 * stderr can be slow (a wineserver round trip under Wine), and every guest
 * CPU transition would queue behind it. */
static XBOX_THREAD_LOCAL char t_note[400];

static void note_flush(void)
{
    if (t_note[0]) {
        fputs(t_note, stderr);
        fflush(stderr);
        t_note[0] = 0;
    }
}

/* KeSetBasePriorityThread's increment as the kernel keeps it: +-16 (half
 * of HIGH_PRIORITY + 1) saturates to time critical (15) or idle (1) and
 * reads back as +-16; anything else moves the base from the process's 8
 * and is clamped to 1..15. */
static LONG sched_normalise(LONG inc)
{
    if (inc >= 16)
        return 16;
    if (inc <= -16)
        return -16;
    if (8 + inc > 15)
        return 7;
    if (8 + inc < 1)
        return -7;
    return inc;
}

static int sched_level(const GuestSchedThread *t)
{
    if (!t)
        return 8;
    if (t->inc >= 16)
        return 15;
    if (t->inc <= -16)
        return 1;
    return 8 + (int)t->inc;
}

static int sched_suspended(const GuestSchedThread *t)
{
    return t && t->suspend > 0;
}

static GuestSchedThread *sched_find(uint32_t obj, int make)
{
    GuestSchedThread *free_slot = NULL;
    int i;

    if (!obj)
        return NULL;
    for (i = 0; i < GUEST_SCHED_THREADS; i++) {
        if (s_sched_threads[i].obj == obj)
            return &s_sched_threads[i];
        if (!free_slot && !s_sched_threads[i].obj)
            free_slot = &s_sched_threads[i];
    }
    if (make && free_slot) {
        free_slot->obj = obj;
        free_slot->inc = 0;
        free_slot->suspend = 0;
        free_slot->gone = 0;
    } else if (make && !s_table_full_said) {
        /* The thread runs at 8, its SetBase is dropped and a suspend of it
         * goes to the host: said once, as that is a different scheduler. */
        s_table_full_said = 1;
        snprintf(t_note, sizeof t_note, "  [GUEST] guest_sched: the thread table"
                 " (%d) is full; thread object 0x%08X runs at 8 and its suspend"
                 " goes to the host\n", GUEST_SCHED_THREADS, obj);
    }
    return make ? free_slot : NULL;
}

static void sched_forget(GuestSchedThread *t)
{
    t->obj = 0;
    t->inc = 0;
    t->suspend = 0;
    t->gone = 0;
}

static const char *where_str(int kind, uint32_t where, char *buf, size_t n);

/* The waiter that runs next: the highest level, the oldest among equals,
 * never a suspended one. NULL when nobody can run. */
static GuestSchedWaiter *sched_best(void)
{
    GuestSchedWaiter *w, *best = NULL;
    int bl = -1;

    if (s_valve && !sched_suspended(s_valve->t))
        return s_valve;                 /* the hang guard's turn comes first */
    for (w = s_wait_head; w; w = w->next) {
        int l;
        if (sched_suspended(w->t))
            continue;
        l = sched_level(w->t);
        if (l > bl) {                   /* the list is in ticket order */
            best = w;
            bl = l;
        }
    }
    return best;
}

static int sched_best_level(void)
{
    GuestSchedWaiter *b = sched_best();
    return b ? sched_level(b->t) : -1;
}

/* May a thread at lvl take a free CPU the last soft release left? Not
 * while the valve's turn runs, unless that thread blocked: the starved
 * thread gets its quantum, not one kernel call. */
static int sched_soft_ok(int lvl, uint64_t now)
{
    unsigned long me = (unsigned long)GetCurrentThreadId();

    if (s_soft_tid && s_soft_tid == s_valve_tid && now < s_valve_until_ns
        && s_soft_tid != me && !s_soft_blocking)
        return 0;
    return !s_soft_tid || s_soft_blocking
        || s_soft_tid == me
        || lvl >= s_soft_level
        || now - s_soft_at_ns >= s_soft_ns;
}

/* Only the soft window let lvl in: the releaser's call is not on the
 * may-block list and ran past it (the priority lock's class of breach). */
static int sched_soft_late(int lvl, uint64_t now)
{
    return s_soft_tid && !s_soft_blocking
        && s_soft_tid != (unsigned long)GetCurrentThreadId()
        && lvl < s_soft_level && now - s_soft_at_ns >= s_soft_ns;
}

/* Count a late admission (under s_lock); a time-critical releaser's is
 * also named, the first 20 then one per 10 s. */
static void sched_late_note(int lvl, uint64_t now)
{
    GuestSchedLate *row = NULL;
    int i;

    for (i = 0; i < GUEST_SCHED_LATE_ROWS && !row; i++)
        if (!s_late[i].n
            || (s_late[i].kind == s_soft_kind && s_late[i].where == s_soft_where
                && s_late[i].level == s_soft_level))
            row = &s_late[i];
    if (!row)
        row = &s_late[GUEST_SCHED_LATE_ROWS - 1];
    if (!row->n) {
        row->kind = s_soft_kind;
        row->where = s_soft_where;
        row->level = s_soft_level;
    }
    row->n++;
    if (s_soft_level == 15 && (++s_late_lines <= 20 || now - s_late_last_ns >= 10000000000ull)) {
        char w[32];
        s_late_last_ns = now;
        snprintf(t_note, sizeof t_note, "  [GUEST] tid %lu (level %d) took the CPU"
                 " from a time-critical holder (tid %lu) whose %s ran %.2f ms:"
                 " the lock is open for its length\n",
                 (unsigned long)GetCurrentThreadId(), lvl, s_soft_tid,
                 where_str(s_soft_kind, s_soft_where, w, sizeof w),
                 (now - s_soft_at_ns) / 1e6);
    }
}

/* Should the holder give way to waiter w now (raise the preempt flag)? A
 * higher level at once; a woken thread (a barger) with the console's +1
 * wake boost over an equal holder at once; an equal one past the quantum,
 * the round robin. */
static int sched_wants_cpu(const GuestSchedWaiter *w, uint64_t now)
{
    int mp = sched_level(w->t), hp = sched_level(s_holder);

    if (sched_suspended(w->t))
        return 0;
    return mp > hp || (w->barge && (mp < 15 ? mp + 1 : 15) > hp)
        || (mp == hp && now - w->t0 >= s_quantum_ns);
}

/* After a priority or a suspend count changed: the holder may now be
 * outranked or suspended, and a waiter may now be the one to run. */
static void sched_kick(void)
{
    if (!s_free) {
        GuestSchedWaiter *b = sched_best();
        if (sched_suspended(s_holder)
            || (b && sched_level(b->t) > sched_level(s_holder)))
            preempt_set(1);
    }
    if (s_waiting)
        WakeAllConditionVariable(&s_cv);
}

/* The holder's question at a hand-off point: give way? Yes when it is
 * suspended or a waiter is at its level or above; otherwise it keeps the
 * CPU, as the console's scheduler kept a higher-priority thread running,
 * and the preempt flag goes down with the refusal. */
static int sched_should_yield(int kind, uint32_t where)
{
    int yield;
    uint64_t now = xbox_HostNowNs();

    EnterCriticalSection(&s_lock);
    if (s_valve_tid == (unsigned long)GetCurrentThreadId() && now < s_valve_until_ns)
        yield = sched_suspended(t_sched);   /* the valve's turn runs its quantum */
    else
        yield = sched_suspended(t_sched) || (s_valve && !sched_suspended(s_valve->t))
                || sched_best_level() >= sched_level(t_sched);
    if (!yield) {
        preempt_set(0);
        s_site_kind = kind;
        s_site_where = where;
        if (s_waiting)
            InterlockedIncrement(&s_refused);
    }
    LeaveCriticalSection(&s_lock);
    return yield;
}

static void sched_took(uint64_t now)
{
    if (sched_soft_late(sched_level(t_sched), now))
        sched_late_note(sched_level(t_sched), now);
    s_free = 0;
    s_granted = 0;
    s_holder = t_sched;
    s_soft_tid = 0;
    s_sched_takes++;
}

/* guest_cpu_acquire's body under s_lock with guest_sched on; the start of
 * the wait, 0 when the CPU was taken at once. A barger takes a free CPU
 * unless a waiter outranks it (or equals it, when a hand-off granted the
 * CPU to the queue); a joining thread only when every waiter is below it.
 * Waiters take it in level then ticket order. */
static XBOX_THREAD_LOCAL int t_joining;     /* the thread's first take (xbox_GuestCpuJoin) */

/* Print a line formatted under s_lock without holding it: the waiter's
 * record stays in the queue, and the loop looks at everything again. */
static void sched_say_unlocked(void)
{
    LeaveCriticalSection(&s_lock);
    note_flush();
    EnterCriticalSection(&s_lock);
}

/* Who the CPU is with, for the lines above: while it is free (a soft
 * release outstanding, a hand-off in flight) the owner is 0 and the
 * holder NULL, so name the thread that let go of it. */
static const char *sched_holder_says(void)
{
    return s_free ? "free; last let go by" : "held by";
}

static unsigned long sched_holder_tid(void)
{
    return s_free ? s_last_tid : (unsigned long)s_owner_tid;
}

static int sched_holder_level(void)
{
    return s_free ? s_last_level : sched_level(s_holder);
}

static uint64_t sched_take(int barge)
{
    GuestSchedWaiter w;
    uint64_t now = xbox_HostNowNs(), joined = now;
    int lvl = sched_level(t_sched), bl = sched_best_level();
    int warned = 0, held_said = 0;
    unsigned long takes0 = s_sched_takes, blocking0 = s_sched_blocking;

    /* While the valve is open the free CPU is the starved waiter's: the
     * holder that just handed over at a back edge or spin pass would
     * otherwise take it straight back, as it reaches s_lock first. */
    if (s_free && !sched_suspended(t_sched) && sched_soft_ok(lvl, now)
        && (!s_valve || sched_suspended(s_valve->t))
        && (barge ? (s_granted ? lvl > bl : lvl >= bl) : lvl > bl)) {
        sched_took(now);
        return 0;
    }
    w.ticket = s_sched_ticket++;
    w.t = t_sched;
    w.barge = barge;
    w.t0 = now;
    w.next = NULL;
    if (s_wait_tail)
        s_wait_tail->next = &w;
    else
        s_wait_head = &w;
    s_wait_tail = &w;
    s_waiting++;
    g_xbox_guest_cpu_waiters = (int)s_waiting;
    if (!s_free && sched_wants_cpu(&w, now))
        preempt_set(1);
    while (!(s_free && sched_best() == &w
             && (s_valve == &w || sched_soft_ok(sched_level(t_sched), now)))) {
        SleepConditionVariableCS(&s_cv, &s_lock, GUEST_CPU_POLL_MS);
        now = xbox_HostNowNs();
        if (!s_free && !preempt_get()
            && (s_valve == &w || sched_wants_cpu(&w, now)))
            preempt_set(1);  /* a refusal or hand-off cleared it */
        /* Suspended time is not waiting for the CPU: the starvation line,
         * the quantum and the wait statistics count from the resume. */
        if (sched_suspended(t_sched)) {
            w.t0 = now;
            /* A thread created suspended and never resumed: a resume
             * through a handle the bridges cannot resolve to its object
             * (bridge_sched_target) leaves it here, a hang with no line. */
            if (t_joining && !held_said && now - joined >= GUEST_SCHED_SUSPENDED_JOIN_NS) {
                held_said = 1;
                snprintf(t_note, sizeof t_note, "  [GUEST] tid %lu (thread object"
                         " 0x%08X) has been held suspended at its start for %.0f s;"
                         " created suspended and not resumed through its handle or"
                         " object?\n", (unsigned long)GetCurrentThreadId(),
                         t_sched ? t_sched->obj : 0, (now - joined) / 1e9);
                sched_say_unlocked();
                now = xbox_HostNowNs();
            }
            continue;
        }
        /* On the console a thread below a running one that never waits
         * does not run either, so a long wait is named, not cut short; the
         * valve gives it one turn only past every wait a title has shown. */
        if (!warned && now - w.t0 >= GUEST_SCHED_STARVE_NS) {
            warned = 1;
            if (InterlockedIncrement(&s_starve_lines) <= 20
                || now - s_starve_last_ns >= 10000000000ull) {
                char site[32];
                s_starve_last_ns = now;
                snprintf(t_note, sizeof t_note, "  [GUEST] tid %lu (level %d) has"
                         " waited %.0f ms for the guest CPU; %s tid %lu (level"
                         " %d), last at %s; meanwhile %lu turns, %lu blocking"
                         " releases, best waiter level %d\n",
                         (unsigned long)GetCurrentThreadId(), sched_level(t_sched),
                         (now - w.t0) / 1e6, sched_holder_says(),
                         sched_holder_tid(), sched_holder_level(),
                         where_str(s_site_kind, s_site_where, site, sizeof site),
                         s_sched_takes - takes0, s_sched_blocking - blocking0,
                         sched_best_level());
                sched_say_unlocked();
                now = xbox_HostNowNs();
            }
        }
        if (!s_valve && now - w.t0 >= s_valve_ns) {
            char site[32];
            s_valve = &w;
            InterlockedIncrement(&s_valves);
            InterlockedIncrement(&s_valves_total);
            preempt_set(1);
            if (s_waiting)
                WakeAllConditionVariable(&s_cv);
            snprintf(t_note, sizeof t_note, "  [GUEST] valve: tid %lu (level %d)"
                     " has waited %.0f ms for the guest CPU and gets one turn; %s"
                     " tid %lu (level %d), last at %s. A higher thread spins on"
                     " work this one does: a model gap, see D7\n",
                     (unsigned long)GetCurrentThreadId(), sched_level(t_sched),
                     (now - w.t0) / 1e6, sched_holder_says(), sched_holder_tid(),
                     sched_holder_level(),
                     where_str(s_site_kind, s_site_where, site, sizeof site));
            sched_say_unlocked();
            now = xbox_HostNowNs();
        }
    }
    {
        GuestSchedWaiter **pp = &s_wait_head, *prev = NULL;
        while (*pp != &w) {
            prev = *pp;
            pp = &(*pp)->next;
        }
        *pp = w.next;
        if (s_wait_tail == &w)
            s_wait_tail = prev;
    }
    s_waiting--;
    g_xbox_guest_cpu_waiters = (int)s_waiting;
    if (s_valve == &w) {
        s_valve = NULL;
        s_valve_tid = (unsigned long)GetCurrentThreadId();
        s_valve_until_ns = now + (s_quantum_ns > 4000000ull ? s_quantum_ns : 4000000ull);
        s_soft_tid = 0;                 /* the turn is given, whoever let go */
    }
    sched_took(now);
    return w.t0;
}

int xbox_GuestSchedOn(void)
{
    return xbox_GuestCpuOn() && s_sched;
}

void xbox_GuestSchedThreadNew(uint32_t obj)
{
    GuestSchedThread *t;

    if (!xbox_GuestSchedOn())
        return;
    EnterCriticalSection(&s_lock);
    if ((t = sched_find(obj, 1)) != NULL) {
        t->inc = 0;                     /* fresh: base 0, not suspended */
        t->suspend = 0;
    }
    LeaveCriticalSection(&s_lock);
    note_flush();
}

void xbox_GuestSchedThreadGone(uint32_t obj)
{
    GuestSchedThread *t;

    if (!xbox_GuestSchedOn())
        return;
    EnterCriticalSection(&s_lock);
    if ((t = sched_find(obj, 0)) != NULL) {
        GuestSchedWaiter *w;
        int queued = t == s_holder;
        for (w = s_wait_head; w; w = w->next)
            queued |= w->t == t;
        /* Queued or holding: its thread still reads the record, so it is
         * freed when that thread parts (xbox_GuestCpuPart). */
        if (queued)
            t->gone = 1;
        else
            sched_forget(t);
    }
    LeaveCriticalSection(&s_lock);
}

void xbox_GuestSchedBind(uint32_t obj)
{
    xbox_GuestDetBind(obj);
    if (!xbox_GuestSchedOn())
        return;
    EnterCriticalSection(&s_lock);
    t_sched = sched_find(obj, 1);
    if (t_held)
        s_holder = t_sched;
    LeaveCriticalSection(&s_lock);
    note_flush();
}

LONG xbox_GuestSchedSetBase(uint32_t obj, LONG increment)
{
    GuestSchedThread *t;
    LONG prev = 0;

    if (!xbox_GuestSchedOn())
        return 0;
    EnterCriticalSection(&s_lock);
    if ((t = sched_find(obj, 1)) != NULL) {
        prev = t->inc;
        t->inc = sched_normalise(increment);
        sched_kick();
    }
    LeaveCriticalSection(&s_lock);
    note_flush();
    return prev;
}

LONG xbox_GuestSchedQueryBase(uint32_t obj)
{
    GuestSchedThread *t;
    LONG v = 0;

    if (!xbox_GuestSchedOn())
        return 0;
    EnterCriticalSection(&s_lock);
    if ((t = sched_find(obj, 0)) != NULL)
        v = t->inc;
    LeaveCriticalSection(&s_lock);
    return v;
}

/* Another guest thread's suspend and resume, counted here and not on the
 * host: the POSIX shim cannot stop another thread at all, and a host
 * suspend on Windows could catch the target inside s_lock or a bridge's
 * own lock. A suspended thread is skipped in the queue and gives the CPU
 * up at its next back edge or kernel call, which is where the console's
 * suspend APC took effect too (at PASSIVE). */
int xbox_GuestSchedSuspend(uint32_t obj, int delta, ULONG *prev)
{
    GuestSchedThread *t;
    int handled = 0;

    if (xbox_GuestDetOn())
        return xbox_GuestDetSuspend(obj, delta, prev);
    if (!xbox_GuestSchedOn())
        return 0;
    EnterCriticalSection(&s_lock);
    t = sched_find(obj, delta > 0);
    if (t && (delta > 0 || t->suspend > 0)) {
        if (prev)
            *prev = (ULONG)t->suspend;
        t->suspend += delta > 0 ? 1 : -1;
        handled = 1;
        sched_kick();
    }
    LeaveCriticalSection(&s_lock);
    note_flush();
    return handled;
}

void xbox_GuestSchedTestWindows(uint64_t soft_ns, uint64_t valve_ns)
{
    s_soft_ns = soft_ns ? soft_ns : GUEST_SCHED_SOFT_NS;
    s_valve_ns = valve_ns ? valve_ns : GUEST_SCHED_VALVE_NS;
}

long xbox_GuestSchedValves(void)
{
    return (long)InterlockedCompareExchange(&s_valves_total, 0, 0);
}

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
    /* guest_priority=0 asks for priorities to be ignored everywhere, so it
     * wins over guest_sched. */
    s_sched = s_on && recomp_env_int(RENV_GUEST_SCHED, 0) == 1
              && recomp_env_int(RENV_GUEST_PRIORITY, 1) != 0;
    if (s_on) {
        fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs"
                        " guest code at a time, %lu ms quantum%s"
                        " (RECOMP_GUEST_LOCK=0: every thread at once)\n",
                (unsigned long)(s_quantum_ns / 1000000ull),
                s_sched ? ", by guest priority (guest_sched)" : "");
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
    if (s_sched) {
        t0 = sched_take(barge);
    } else if (s_free && !s_granted && (barge || !s_waiting)) {
        s_free = 0;
    } else {
        my = s_next_ticket++;
        s_waiting++;
        g_xbox_guest_cpu_waiters = (int)s_waiting;
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
        g_xbox_guest_cpu_waiters = (int)s_waiting;
    }
    InterlockedExchange(&s_owner_tid, (LONG)GetCurrentThreadId());
    LeaveCriticalSection(&s_lock);
    note_flush();
    t_held = 1;
    t_joining = 0;
    if (t0) {
        uint64_t now = xbox_HostNowNs();
        LONG wait_us = (LONG)((now - t0) / 1000u);
        GuestCpuThread *r = thread_rec();
        InterlockedIncrement(&s_waits);
        raise_max(&s_max_wait_us, wait_us);
        /* The run's longest wait, in both modes and without the pacing
         * trace: one line each time it grows past 50 ms, so a log says how
         * long a thread was kept off (the guest_sched A/B listens). */
        if (wait_us >= 50000 && wait_us > s_max_wait_run_us) {
            raise_max(&s_max_wait_run_us, wait_us);
            if (s_max_wait_run_us == wait_us) {
                fprintf(stderr, "  [GUEST] longest guest CPU wait so far: %.1f ms"
                        " (tid %lu, level %d)\n", wait_us / 1000.0,
                        (unsigned long)GetCurrentThreadId(),
                        s_sched ? sched_level(t_sched) : -1);
                fflush(stderr);
            }
        }
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
    preempt_set(0);       /* the waiter's turn comes now */
    s_free = 1;
    s_last_tid = (unsigned long)GetCurrentThreadId();
    s_last_level = sched_level(t_sched);
    s_holder = NULL;
    s_soft_tid = 0;
    if (s_valve_tid == (unsigned long)GetCurrentThreadId())
        s_valve_tid = 0;                /* the valve's turn ends with the hand-off */
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
static void guest_cpu_soft_release(int kind, uint32_t where, int wake,
                                   int blocking)
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
    if (s_sched) {
        s_holder = NULL;
        s_soft_tid = (unsigned long)GetCurrentThreadId();
        s_soft_level = s_last_level = sched_level(t_sched);
        s_last_tid = s_soft_tid;
        s_soft_blocking = blocking;
        s_soft_kind = s_site_kind = kind;
        s_soft_where = s_site_where = where;
        s_soft_at_ns = now;
        s_sched_blocking += blocking != 0;
    }
    InterlockedExchange(&s_owner_tid, 0);
    if (wake && s_waiting)
        WakeAllConditionVariable(&s_cv);
    LeaveCriticalSection(&s_lock);
}

/* The kernel calls that can block the caller: the waits and sleeps, file
 * I/O, a thread suspend. NtYieldExecution is a hand-off in its own right.
 * With guest_sched the waits and NtSuspendThread are not taken as blocked
 * at the call (ordinal_is_wait); see xbox_GuestCpuRelease. */
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

/* The waits: on the console one whose object is already signalled returns
 * without a context switch, so with guest_sched it is a short call until
 * the wait really blocks (kernel_sync.c calls xbox_GuestCpuBlocked then). */
static int ordinal_is_wait(uint32_t ordinal)
{
    switch (ordinal) {
    case 158: case 159:                 /* KeWaitFor{Multiple,Single}Object(s) */
    case 223:                           /* NtRemoveIoCompletion */
    case 230:                           /* NtSignalAndWaitForSingleObjectEx */
    case 233: case 234: case 235:       /* NtWaitFor{Single,SingleEx,MultipleEx} */
        return 1;
    default:
        return 0;
    }
}

/* ── The entry points (kernel.h) ─────────────────────────── */

void xbox_GuestCpuJoin(void)
{
    t_guest = 1;
    xbox_GuestDetArm();
    if (xbox_GuestDetOn()) {
        /* The deterministic ring (kernel_guest_det.c) is the guest CPU. */
        if (!t_held)
            xbox_GuestDetJoin();
        t_held = 1;
        return;
    }
    if (xbox_GuestCpuOn() && !t_held) {
        t_joining = 1;              /* a created-suspended thread waits here */
        guest_cpu_acquire(0);
    }
}

void xbox_GuestCpuPart(void)
{
    if (xbox_GuestDetOn()) {
        if (t_held)
            xbox_GuestDetPart();
        t_held = 0;
        t_guest = 0;
        return;
    }
    if (t_held)
        guest_cpu_handoff(XBOX_GUEST_CPU_EXIT, 0);
    if (t_sched && s_sched) {
        EnterCriticalSection(&s_lock);
        if (t_sched->gone)              /* its object was freed meanwhile */
            sched_forget(t_sched);
        LeaveCriticalSection(&s_lock);
    }
    t_guest = 0;
    t_sched = NULL;
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
    if (xbox_GuestDetOn()) {
        /* The holder keeps the token through the call; the calls that
         * block give it up themselves, and so does a yield. The thread's
         * end (258) keeps it too: PsTerminateSystemThread parts only after
         * it has published the end in the thread object, or a thread
         * polling GetExitCodeThread would see the end at whatever pass the
         * host got round to writing it. */
        if (kind == XBOX_GUEST_CPU_KERNEL && where == 238)
            xbox_GuestDetYield(238);
        return 0;
    }
    if (kind == XBOX_GUEST_CPU_KERNEL) {
        if (where == 258 /* PsTerminateSystemThread: ExitThread, no return */)
            guest_cpu_handoff(XBOX_GUEST_CPU_EXIT, where);
        /* guest_sched: the same points, but only to a waiter at the
         * holder's level or above. A sleep or file I/O counts as blocked at
         * once, so anyone may take the CPU for its length. A wait does not:
         * one on a signalled object returns at once on the console, and a
         * lower thread let in there would run inside a time-critical
         * holder's lock (CRI ADX) until the holder preempted it back; the
         * wait says so itself when it really blocks (xbox_GuestCpuBlocked,
         * kernel_sync.c). NtSuspendThread does not block either: another
         * thread's suspend is only counted, and a self-suspend says it
         * blocks in its bridge. KeDelayExecutionThread with a zero
         * interval readies only equal priorities on the console, so the
         * delay says it blocks only when it sleeps (kernel_thread.c). */
        else if (s_sched) {
            int blocking = ordinal_may_block(where) && !ordinal_is_wait(where)
                           && where != 231 && where != 99;
            if ((where == 238
                 || (s_waiting && !xbox_IrqlThisThreadBlocksDpcs()
                     && (preempt_get()
                         || xbox_HostNowNs() - t_turn_ns >= s_quantum_ns)))
                && sched_should_yield(kind, where))
                guest_cpu_handoff(kind, where);
            else
                guest_cpu_soft_release(kind, where, blocking, blocking);
        }
        /* Not at DISPATCH (D3): the thunk gets here at every ordinal,
         * KfLowerIrql and KeReleaseSpinLock included, while the caller
         * still holds the DISPATCH gate; a hand-off there queues the gate
         * holder behind every waiter, and the waiter's own KfRaiseIrql then
         * blocks on the gate. The call soft-releases and barges back, as
         * the back edge and the spin-wait pass refuse to hand over there. */
        else if (where == 238 /* NtYieldExecution */
            || (s_waiting && !xbox_IrqlThisThreadBlocksDpcs()
                && (preempt_get()
                    || xbox_HostNowNs() - t_turn_ns >= s_quantum_ns)))
            guest_cpu_handoff(kind, where);
        else
            guest_cpu_soft_release(kind, where, ordinal_may_block(where), 0);
    } else if (kind == XBOX_GUEST_CPU_HOST) {
        /* About to block on the host for a while (a vblank, say) with no
         * kernel call to let go in: the thread is off the CPU as a waiting
         * thread is on the console, so the oldest waiter gets it now. */
        if (s_waiting)
            guest_cpu_handoff(kind, where);
        else
            guest_cpu_soft_release(kind, where, 1, 1);
    } else {
        /* A spin-sleep pass sleeps on the host: off the CPU, as blocked. */
        guest_cpu_soft_release(kind, where, 0, kind == XBOX_GUEST_CPU_SPIN);
    }
    return 1;
}

void xbox_GuestCpuAcquire(void)
{
    if (!t_held && xbox_GuestCpuOn() && !xbox_GuestDetOn())
        guest_cpu_acquire(1);
}

/* A bridge about to block on the host inside a kernel call that usually
 * does not (a contended critical section: the thread that holds it needs
 * the CPU to leave it), a wait whose object is not signalled, a
 * self-suspend: the CPU let go of at the call stays free for a while, so
 * the waiters learn now instead of at their next poll, and with guest_sched
 * any level may take it. */
void xbox_GuestCpuBlocked(void)
{
    if (t_held || !t_guest || !xbox_GuestCpuOn() || xbox_GuestDetOn())
        return;
    if (!s_sched && !s_waiting)
        return;
    EnterCriticalSection(&s_lock);
    if (s_sched && s_soft_tid == (unsigned long)GetCurrentThreadId()
        && !s_soft_blocking) {
        s_soft_blocking = 1;
        s_sched_blocking++;
    }
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

    if (!t_held || !s_waiting || xbox_GuestDetOn())
        return 0;
    if (s_sched && !sched_should_yield(XBOX_GUEST_CPU_SPIN, va))
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

    if (xbox_GuestDetOn()) {
        xbox_GuestDetBackEdge(va, xbox_IrqlThisThreadBlocksDpcs());
        return;
    }
    if (!t_held)
        return;
    if (s_sched ? xbox_IrqlThisThreadBlocksDpcs()
                : (!s_waiting || xbox_IrqlThisThreadBlocksDpcs())) {
        preempt_set(0);
        return;
    }
    /* guest_sched: a suspended holder stops here even with nobody waiting. */
    if (s_sched && !sched_should_yield(XBOX_GUEST_CPU_PREEMPT, va))
        return;
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
        guest_cpu_soft_release(XBOX_GUEST_CPU_CALLBACK, 0, 0, 0);
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
    out->yields = GC_TAKE(s_yields);
    out->preempts = GC_TAKE(s_preempts);
    out->max_turn_us = GC_TAKE(s_max_turn_us);
    out->max_wait_us = GC_TAKE(s_max_wait_us);
    out->max_turn_kind = (int)GC_TAKE(s_max_turn_kind);
    out->max_turn_where = (uint32_t)GC_TAKE(s_max_turn_where);
    out->refused = GC_TAKE(s_refused);
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
    if (s_sched) {
        GuestSchedLate late[GUEST_SCHED_LATE_ROWS];
        LONG valves = InterlockedExchange(&s_valves, 0);
        fprintf(stderr, "[PACING]   guest sched: %ld hand-offs kept by a holder"
                " above every waiter, valve %ld\n", s.refused, (long)valves);
        EnterCriticalSection(&s_lock);
        memcpy(late, s_late, sizeof late);
        memset(s_late, 0, sizeof s_late);
        LeaveCriticalSection(&s_lock);
        for (int i = 0; i < GUEST_SCHED_LATE_ROWS && late[i].n; i++)
            fprintf(stderr, "[PACING]     soft admissions past %.0f ms: %s at level %d x%ld\n",
                    s_soft_ns / 1e6, where_str(late[i].kind, late[i].where, w, sizeof w),
                    late[i].level, late[i].n);
    }
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
