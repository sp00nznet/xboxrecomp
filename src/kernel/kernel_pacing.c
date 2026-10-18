/*
 * kernel_pacing.c - see kernel_pacing.h.
 *
 * The wait is a generation counter under a lock with a condition variable.
 * A waker takes the lock, bumps the generation, drops the lock and
 * broadcasts. A waiter takes the lock and sleeps only while the generation
 * still equals the one it last saw; the condition variable releases the lock
 * atomically. The generation is never read outside the lock, so:
 *  - a wake between the lifted loop's memory read and the wait has already
 *    moved the generation, and the wait returns without sleeping (no lost
 *    wake-up);
 *  - a guest store that precedes a wake in program order (the vblank DPC's
 *    bump of the frame counter, then xbox_IrqlLeaveInterrupt) is visible to
 *    the loop's next MEM32 read after the wait returns, on x86-64 and arm64,
 *    because the lock pairs the waker's release with the waiter's acquire.
 * Only a site whose writer the kernel never signals times out, and then by
 * at most SPIN_TIMEOUT_MS.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "recomp_env.h"
#include "platform/host_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <time.h>
#endif

/* ── The vblank schedule ─────────────────────────────────── */

static uint64_t vblank_due(const XboxVblankSchedule *s, uint64_t n)
{
    /* n * 1e9 overflows 64 bits only after ~9.7 years of vblanks; the
     * 100 ms restart rule resets n long before a host runs that long. */
    return s->t0_ns + n * 1000000000ull / XBOX_VBLANK_HZ;
}

int xbox_VblankSchedule(XboxVblankSchedule *s, uint64_t now_ns,
                        uint64_t *next_due_ns, uint64_t *restarted_late_ns)
{
    uint64_t due;

    *restarted_late_ns = 0;
    if (!s->started) {
        s->started = 1;
        s->t0_ns = now_ns;
        s->count = 0;
    }
    due = vblank_due(s, s->count);
    if (now_ns < due) {
        *next_due_ns = due;
        return 0;
    }
    if (now_ns - due > XBOX_VBLANK_RESTART_NS) {
        *restarted_late_ns = now_ns - due;
        s->t0_ns = now_ns;
        s->count = 0;
    }
    s->count++;
    *next_due_ns = vblank_due(s, s->count);
    return 1;
}

/* ── Mode and the wait ───────────────────────────────────── */

#define SPIN_TIMEOUT_MS 1
/* A thread that comes back to a wait sooner than this after its last one is
 * still in the same loop (a pass is a few guest instructions; the game's
 * frame work between two loops takes milliseconds). Longer, and its last
 * wait was the loop's final pass: the outcome that let the loop exit. */
#define SPIN_EXIT_GAP_NS 500000ull
/* The waiting thread's CPU clock is read on one call in this many. */
#define SPIN_CPU_SAMPLE 64

volatile int g_xbox_spin_sleep;

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static uint32_t s_gen;                       /* under s_lock only */
static XBOX_THREAD_LOCAL uint32_t t_seen_gen;

static BOOL CALLBACK pacing_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    return TRUE;
}

static void pacing_once(void)
{
    InitOnceExecuteOnce(&s_once, pacing_init, NULL, NULL);
}

void xbox_PacingSetMode(int mode)
{
    pacing_once();
    g_xbox_spin_sleep = (mode == XBOX_PACING_SLEEP);
}

int xbox_PacingMode(void)
{
    return g_xbox_spin_sleep ? XBOX_PACING_SLEEP : XBOX_PACING_SPIN;
}

/* Per-site counters: an open-addressed table keyed by the loop's VA. A
 * title has a handful of lowered sites; past the table's size they share
 * the last slot, reported as "other". */
#define SITE_SLOTS 31
typedef struct {
    volatile LONG va;          /* 0 = free */
    volatile LONG waits, immediate, wakes, timeouts, dispatch;
    /* How loops left: their last pass returned on a signal (or at once),
     * or on the 1 ms timeout. Timeouts mid-loop are normal (a vblank wait
     * spans up to 16 of them); a loop that keeps leaving on a timeout has a
     * writer the kernel never signals. */
    volatile LONG exit_wake, exit_timeout;
    /* At spin pacing the macro calls only with a flag up: the passes that
     * came here for a waiting guest thread, and how many handed over. */
    volatile LONG spin_passes, spin_yields;
    volatile LONG inwait_us;   /* sampled CPU inside the wait, x SAMPLE */
    volatile LONG calls;       /* for the sampling stride */
    /* The waiting thread's CPU clock against wall time, first and last
     * sample in the window: its whole CPU share, game work included. */
    volatile LONGLONG cpu0_ns, wall0_ns, cpu1_ns, wall1_ns;
} SpinSite;
static SpinSite s_sites[SITE_SLOTS + 1];

/* The calling thread's last wait, for the exit count (mode sleep only). */
enum { LAST_NONE, LAST_WAKE, LAST_TIMEOUT };
static XBOX_THREAD_LOCAL SpinSite *t_last_site;
static XBOX_THREAD_LOCAL int t_last_outcome;
static XBOX_THREAD_LOCAL uint64_t t_last_ret_ns;

static SpinSite *site_for(uint32_t va)
{
    unsigned h = (va * 2654435761u) % SITE_SLOTS, i;

    for (i = 0; i < SITE_SLOTS; i++) {
        SpinSite *s = &s_sites[(h + i) % SITE_SLOTS];
        LONG cur = s->va;
        if (cur == (LONG)va)
            return s;
        if (!cur && InterlockedCompareExchange(&s->va, (LONG)va, 0) == 0)
            return s;
        if (s->va == (LONG)va)
            return s;
    }
    return &s_sites[SITE_SLOTS];
}

void xbox_SpinWait(uint32_t va)
{
    SpinSite *site;
    int sample, woke = 0, immediate = 0, held;
    uint64_t c0 = 0, now;

    /* A lowered loop's pass is between two guest instructions: a holder
     * spinning here at DISPATCH takes a posted interrupt, as a CPU would.
     * At "spin" pacing the macro calls only for that, and for a guest
     * thread waiting for the guest CPU: the pass hands it over and queues
     * behind it, the console's quantum expiry (kernel_guest_cpu.c). The
     * interrupt first, holding the CPU, as an ISR ran with no thread in
     * between on the console. */
    xbox_IrqSafePoint();
    if (!g_xbox_spin_sleep) {
        /* Not at DISPATCH: there the console's scheduler could not switch
         * threads either, and the loop's DPC is held off by this thread's
         * own IRQL, not by the CPU. */
        if (g_xbox_guest_cpu_waiters && !xbox_IrqlThisThreadBlocksDpcs()) {
            site = site_for(va);
            InterlockedIncrement(&site->spin_passes);
            if (xbox_GuestCpuYield(va))
                InterlockedIncrement(&site->spin_yields);
        }
        return;
    }
    site = site_for(va);
    now = xbox_HostNowNs();

    /* The previous wait on this thread ended its loop if the thread was
     * away long enough: count how that loop left. */
    if (t_last_site && t_last_outcome != LAST_NONE
        && now - t_last_ret_ns > SPIN_EXIT_GAP_NS)
        InterlockedIncrement(t_last_outcome == LAST_WAKE
                             ? &t_last_site->exit_wake
                             : &t_last_site->exit_timeout);
    t_last_site = site;
    t_last_outcome = LAST_NONE;
    t_last_ret_ns = now;

    InterlockedIncrement(&site->waits);
    /* At DISPATCH or with the gate held the DPC that ends this loop cannot
     * run until the thread leaves; sleeping would only add latency, and the
     * pass does not yield the guest CPU either: at DISPATCH the console
     * switched no threads. */
    if (xbox_IrqlThisThreadBlocksDpcs()) {
        InterlockedIncrement(&site->dispatch);
        return;
    }
    sample = (InterlockedIncrement(&site->calls) % SPIN_CPU_SAMPLE) == 0;
    if (sample)
        c0 = xbox_ThreadCpuNs();

    pacing_once();
    /* The sleep is without the guest CPU: a waiting guest thread runs for
     * its length, and this thread queues behind the waiters on the way
     * back, as at a spin-mode yield. */
    held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_SPIN, va);
    EnterCriticalSection(&s_lock);
    if (s_gen != t_seen_gen) {
        immediate = 1;
    } else {
        SleepConditionVariableCS(&s_cv, &s_lock, SPIN_TIMEOUT_MS);
        woke = (s_gen != t_seen_gen);
    }
    t_seen_gen = s_gen;
    LeaveCriticalSection(&s_lock);
    if (held)
        xbox_GuestCpuAcquire();

    if (immediate)
        InterlockedIncrement(&site->immediate);
    else if (woke)
        InterlockedIncrement(&site->wakes);
    else
        InterlockedIncrement(&site->timeouts);
    t_last_outcome = immediate || woke ? LAST_WAKE : LAST_TIMEOUT;
    t_last_ret_ns = xbox_HostNowNs();

    if (sample) {
        uint64_t c1 = xbox_ThreadCpuNs(), w = xbox_HostNowNs();
        InterlockedExchangeAdd(&site->inwait_us,
                               (LONG)((c1 - c0) / 1000u * SPIN_CPU_SAMPLE));
        if (!site->wall0_ns) {
            site->cpu0_ns = (LONGLONG)c1;
            site->wall0_ns = (LONGLONG)w;
        }
        site->cpu1_ns = (LONGLONG)c1;
        site->wall1_ns = (LONGLONG)w;
    }
}

void xbox_SpinWake(void)
{
    if (!g_xbox_spin_sleep)
        return;
    pacing_once();
    EnterCriticalSection(&s_lock);
    s_gen++;
    LeaveCriticalSection(&s_lock);
    WakeAllConditionVariable(&s_cv);
}

int xbox_SpinSiteStats(uint32_t va, XboxSpinSiteStats *out)
{
    unsigned i;

    for (i = 0; i <= SITE_SLOTS; i++) {
        SpinSite *s = &s_sites[i];
        if (s->va != (LONG)va || !s->waits)
            continue;
        out->va = va;
        out->waits = s->waits;
        out->immediate = s->immediate;
        out->wakes = s->wakes;
        out->timeouts = s->timeouts;
        out->dispatch = s->dispatch;
        out->exit_wake = s->exit_wake;
        out->exit_timeout = s->exit_timeout;
        return 1;
    }
    return 0;
}

static void site_reset(SpinSite *s)
{
    InterlockedExchange(&s->waits, 0);
    InterlockedExchange(&s->immediate, 0);
    InterlockedExchange(&s->wakes, 0);
    InterlockedExchange(&s->timeouts, 0);
    InterlockedExchange(&s->dispatch, 0);
    InterlockedExchange(&s->exit_wake, 0);
    InterlockedExchange(&s->exit_timeout, 0);
    InterlockedExchange(&s->spin_passes, 0);
    InterlockedExchange(&s->spin_yields, 0);
    InterlockedExchange(&s->inwait_us, 0);
    /* Not atomic with a waiter's sample: a sample landing between these
     * stores and the next window's first one can pair an old cpu0 with a
     * new wall0, or count a wait in the window it did not finish in. Only
     * the trace's thread CPU share for one window can be off; nothing
     * reads these counters for pacing itself. */
    s->wall0_ns = 0;
}

void xbox_SpinStatsReset(void)
{
    unsigned i;

    for (i = 0; i <= SITE_SLOTS; i++)
        site_reset(&s_sites[i]);
}

/* ── CPU clocks ──────────────────────────────────────────── */

uint64_t xbox_ThreadCpuNs(void)
{
#ifdef _WIN32
    /* Wine advances these in system-tick steps (15.6 ms): unbiased over a
     * trace window, coarse within one. */
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t xbox_ProcessCpuNs(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000ull
           + (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000ull;
#endif
}

/* ── RECOMP_TRACE=pacing ─────────────────────────────────── */

#define TRACE_WINDOW 600

static int s_trace = -1;          /* 0 off, 1 summaries, 2 also every flip */
/* The vblank gap range since the last summary, in µs (the timer thread
 * writes, the walker reads and resets). */
static volatile LONG s_gap_min_us = 0x7FFFFFFF, s_gap_max_us;
static uint64_t s_last_vblank_ns;   /* timer thread only */

static int trace_mode(void)
{
    if (s_trace < 0) {
        const char *v = recomp_env(RENV_PACING_TRACE);
        s_trace = !v || !*v || *v == '0' ? 0 : (strcmp(v, "all") == 0 ? 2 : 1);
    }
    return s_trace;
}

int xbox_PacingTraceOn(void)
{
    return trace_mode() != 0;
}

void xbox_PacingVblank(uint64_t now_ns)
{
    if (!trace_mode())
        return;
    if (s_last_vblank_ns) {
        LONG g = (LONG)((now_ns - s_last_vblank_ns) / 1000u), cur;
        while ((cur = s_gap_min_us) > g &&
               InterlockedCompareExchange(&s_gap_min_us, g, cur) != cur)
            ;
        while ((cur = s_gap_max_us) < g &&
               InterlockedCompareExchange(&s_gap_max_us, g, cur) != cur)
            ;
    }
    s_last_vblank_ns = now_ns;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static double pct(const uint32_t *sorted, int n, double p)
{
    int i = (int)(p * (n - 1) + 0.5);
    return sorted[i] / 1000.0;
}

static void site_line(const SpinSite *s, double window_s, int other)
{
    double inwait = window_s > 0 ? s->inwait_us / 1e6 / window_s * 100.0 : 0;
    double thread = 0;
    if (s->wall1_ns > s->wall0_ns)
        thread = (double)(s->cpu1_ns - s->cpu0_ns)
                 / (double)(s->wall1_ns - s->wall0_ns) * 100.0;
    if (other)
        fprintf(stderr, "[PACING]   site other:");
    else
        fprintf(stderr, "[PACING]   site 0x%08X:", (uint32_t)s->va);
    fprintf(stderr, " waits %ld immediate %ld wakes %ld timeouts %ld"
            " dispatch %ld; exits wake %ld timeout %ld;"
            " cpu in-wait %.1f%% thread %.0f%%",
            (long)s->waits, (long)s->immediate, (long)s->wakes,
            (long)s->timeouts, (long)s->dispatch, (long)s->exit_wake,
            (long)s->exit_timeout, inwait, thread);
    if (s->spin_passes)
        fprintf(stderr, "; spin passes with a waiter %ld, handed over %ld",
                (long)s->spin_passes, (long)s->spin_yields);
    fputc('\n', stderr);
}

void xbox_PacingFlip(uint32_t flip, uint32_t batches)
{
    /* Walker thread only. */
    static uint64_t last_ns, win_ns, win_cpu;
    static uint32_t iv_us[TRACE_WINDOW];
    static int n;
    uint64_t now;
    int mode = trace_mode();

    if (!mode)
        return;
    now = xbox_HostNowNs();
    if (mode == 2)
        fprintf(stderr, "[PACING] flip %u t_us %llu batches %u\n", flip,
                (unsigned long long)(now / 1000u), batches);
    if (!win_ns) {
        win_ns = now;
        win_cpu = xbox_ProcessCpuNs();
    }
    if (last_ns)
        iv_us[n++] = (uint32_t)((now - last_ns) / 1000u);
    last_ns = now;
    if (n < TRACE_WINDOW)
        return;

    {
        uint32_t s[TRACE_WINDOW];
        uint64_t cpu = xbox_ProcessCpuNs();
        double window_s = (double)(now - win_ns) / 1e9;
        LONG gmin = InterlockedExchange(&s_gap_min_us, 0x7FFFFFFF);
        LONG gmax = InterlockedExchange(&s_gap_max_us, 0);
        unsigned i;

        memcpy(s, iv_us, sizeof s);
        qsort(s, TRACE_WINDOW, sizeof s[0], cmp_u32);
        fprintf(stderr, "[PACING] flips %d: interval p5 %.1f p50 %.1f p95 %.1f"
                " max %.1f ms; ", TRACE_WINDOW, pct(s, TRACE_WINDOW, 0.05),
                pct(s, TRACE_WINDOW, 0.50), pct(s, TRACE_WINDOW, 0.95),
                s[TRACE_WINDOW - 1] / 1000.0);
        if (gmax)
            fprintf(stderr, "vblank gap %.1f-%.1f ms; ", gmin / 1000.0,
                    gmax / 1000.0);
        else
            fprintf(stderr, "vblank gap -; ");
        fprintf(stderr, "cpu process %.0f%%; mode %s\n",
                window_s > 0 ? (double)(cpu - win_cpu) / 1e9 / window_s * 100.0 : 0,
                g_xbox_spin_sleep ? "sleep" : "spin");
        for (i = 0; i <= SITE_SLOTS; i++) {
            SpinSite *st = &s_sites[i];
            if ((st->va || i == SITE_SLOTS) && (st->waits || st->spin_passes))
                site_line(st, window_s, i == SITE_SLOTS);
            site_reset(st);
        }
        {
            /* The flip hold's window (nv2a_pb_exec.c). */
            extern void nv2a_pb_exec_flip_report(void);
            nv2a_pb_exec_flip_report();
        }
        xbox_GuestCpuReport();
        fflush(stderr);
        n = 0;
        win_ns = now;
        win_cpu = cpu;
    }
}
