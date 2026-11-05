/*
 * The stop probe (kernel_reset_probe.h): how long a reset's stop would take.
 *
 * Each guest thread gets a slot at its join. While the key is set the hooks
 * keep the slot's state (in a kernel call and which, blocked, queued on the
 * CPU lock); off, they return at once. At a listed flip the walker wakes
 * the probe thread, which snapshots the slots, opens the sample and waits
 * up to RESET_PROBE_WINDOW_MS for every thread alive at the flip to reach
 * a stop point, then logs and closes it. Nothing is stopped or unwound:
 * the title runs on as before, except that the preempt flag raised during
 * the sample makes the CPU holder hand over at its next back edge if
 * someone waits (as a waiter's own poll would a few ms later).
 */
#include "kernel.h"
#include "kernel_reset_probe.h"
#include "recomp_env.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESET_PROBE_SLOTS     64
#define RESET_PROBE_WINDOW_MS 1000      /* a thread not stopped by then: "not stopped" */
#define RESET_PROBE_ITEMS     64

volatile int32_t g_xbox_reset_probe;
int g_xbox_reset_probe_track;

typedef struct {
    volatile LONG  used;            /* 1 while a guest thread owns it */
    unsigned long  tid;
    volatile LONG  state;           /* XBOX_RESET_* */
    volatile LONG  where;           /* the ordinal, in a kernel call */
    /* The open sample's: */
    volatile LONG  in_sample;       /* alive at the flip */
    volatile LONG  stopped;         /* 0, or the XBOX_RESET_STOP_* kind */
    uint32_t       stop_where;
    uint64_t       stop_ns;
} ResetProbeSlot;

static ResetProbeSlot s_slots[RESET_PROBE_SLOTS];
static XBOX_THREAD_LOCAL ResetProbeSlot *t_slot;

static int s_tracking = -1;         /* -1: the key not read yet */
static uint64_t s_t0_ns;            /* the open sample's start */
static HANDLE s_kick;               /* the flip wakes the probe thread */
static volatile LONG s_thread_up, s_pending_flip, s_busy;
static XboxResetProbeSample s_last;
static volatile LONG s_last_seq;

/* The key: items split on commas, each a flip N or a range A-B/S (S 1 when
 * left out). "300-6000/300" is 20 samples, 300 apart. */
typedef struct { uint32_t a, b, step; } ResetProbeItem;
static ResetProbeItem s_items[RESET_PROBE_ITEMS];
static int s_n_items = -1;

static int parse_items(const char *spec, ResetProbeItem *out, int max)
{
    int n = 0;
    const char *p = spec;

    while (p && *p && n < max) {
        char *end;
        unsigned long a, b, step = 1;
        while (*p == ',' || *p == ' ')
            p++;
        if (!*p)
            break;
        a = strtoul(p, &end, 10);
        if (end == p)
            break;
        b = a;
        p = end;
        if (*p == '-') {
            b = strtoul(p + 1, &end, 10);
            if (end == p + 1)
                break;
            p = end;
            if (*p == '/') {
                step = strtoul(p + 1, &end, 10);
                if (end == p + 1 || step == 0)
                    break;
                p = end;
            }
        }
        if (b < a)
            break;
        out[n].a = (uint32_t)a;
        out[n].b = (uint32_t)b;
        out[n].step = (uint32_t)step;
        n++;
        if (*p && *p != ',')
            break;
    }
    return n;
}

static int items_list(const ResetProbeItem *it, int n, uint32_t flip)
{
    int i;
    for (i = 0; i < n; i++)
        if (flip >= it[i].a && flip <= it[i].b
            && (flip - it[i].a) % it[i].step == 0)
            return 1;
    return 0;
}

int xbox_ResetProbeListed(const char *spec, uint32_t flip)
{
    ResetProbeItem it[RESET_PROBE_ITEMS];
    int n = parse_items(spec, it, RESET_PROBE_ITEMS);
    return items_list(it, n, flip);
}

int xbox_ResetProbeTracking(void)
{
    int t = s_tracking;
    if (t < 0) {
        /* Idempotent: two threads racing here read the same key. */
        t = recomp_env(RENV_RESET_PROBE) != NULL;
        s_tracking = t;
        g_xbox_reset_probe_track = t;
    }
    return t;
}

void xbox_ResetProbeReloadConfig(void)
{
    s_tracking = -1;
    s_n_items = -1;
    xbox_ResetProbeTracking();
}

void xbox_ResetProbeJoin(void)
{
    int i;

    if (!xbox_ResetProbeTracking() || t_slot)
        return;
    for (i = 0; i < RESET_PROBE_SLOTS; i++) {
        ResetProbeSlot *s = &s_slots[i];
        if (s->in_sample)
            continue;
        if (InterlockedCompareExchange(&s->used, 1, 0) == 0) {
            s->tid = (unsigned long)GetCurrentThreadId();
            s->state = XBOX_RESET_RUN;
            s->where = 0;
            s->in_sample = 0;
            s->stopped = 0;
            t_slot = s;
            return;
        }
    }
    fprintf(stderr, "[RESET-PROBE] more than %d guest threads: tid %lu not tracked\n",
            RESET_PROBE_SLOTS, (unsigned long)GetCurrentThreadId());
}

void xbox_ResetProbePart(void)
{
    ResetProbeSlot *s = t_slot;
    if (!s)
        return;
    /* A thread that ends inside a sample has stopped for good: it is not
     * one the reset would wait for. */
    if (g_xbox_reset_probe && s->in_sample && !s->stopped)
        xbox_ResetProbeHitSlow(XBOX_RESET_STOP_BRIDGE, 258);
    /* in_sample stays for the report, which clears it; a join meanwhile
     * takes another slot. */
    InterlockedExchange(&s->used, 0);
    t_slot = NULL;
}

void xbox_ResetProbeStateSlow(int state, uint32_t where)
{
    ResetProbeSlot *s = t_slot;
    if (!s)
        return;
    s->where = (LONG)where;
    InterlockedExchange(&s->state, state);
}

void xbox_ResetProbeHitSlow(int kind, uint32_t where)
{
    ResetProbeSlot *s = t_slot;
    if (!s || !s->in_sample || s->stopped)
        return;
    s->stop_ns = xbox_HostNowNs();
    s->stop_where = where;
    InterlockedExchange(&s->stopped, kind);
}

static const char *stop_name(int kind)
{
    switch (kind) {
    case XBOX_RESET_STOP_BACK_EDGE: return "back_edge";
    case XBOX_RESET_STOP_SPIN:      return "spin";
    case XBOX_RESET_STOP_KICK:      return "spin_kick";
    case XBOX_RESET_STOP_ACQUIRE:   return "cpu_acquire";
    case XBOX_RESET_STOP_BRIDGE:    return "bridge";
    case XBOX_RESET_STOP_BLOCKED:   return "blocked";
    case XBOX_RESET_STOP_WAITING:   return "waiting";
    case XBOX_RESET_STOP_QUEUED:    return "queued";
    default:                        return "?";
    }
}

static const char *state_name(int state)
{
    switch (state) {
    case XBOX_RESET_RUN:       return "running";
    case XBOX_RESET_IN_BRIDGE: return "in_bridge";
    case XBOX_RESET_IN_BLOCK:  return "blocked";
    case XBOX_RESET_IN_QUEUE:  return "queued";
    default:                   return "?";
    }
}

static void run_sample(uint32_t flip)
{
    int i, alive = 0, stopped = 0, late = 0;
    uint64_t max_ns = 0, deadline;
    XboxResetProbeSample out;

    if (xbox_GuestDetOn()) {
        /* The deterministic ring owns the preempt flag and the hand-offs;
         * raising it would change the run being measured. */
        fprintf(stderr, "[RESET-PROBE] flip=%u skipped: in the deterministic mode\n", flip);
        return;
    }
    s_t0_ns = xbox_HostNowNs();
    /* The snapshot: who is alive, and who is already where a reset would
     * find it stopped. A thread can move between the read of its state and
     * the open flag below; it then reaches a stop point at once, which the
     * sample counts as stopped at about 0 too. */
    for (i = 0; i < RESET_PROBE_SLOTS; i++) {
        ResetProbeSlot *s = &s_slots[i];
        LONG st;
        if (!s->used)
            continue;
        s->stopped = 0;
        st = InterlockedCompareExchange(&s->state, 0, 0);
        if (st == XBOX_RESET_IN_QUEUE) {
            s->stop_ns = s_t0_ns;
            s->stop_where = 0;
            s->stopped = XBOX_RESET_STOP_QUEUED;
        } else if (st == XBOX_RESET_IN_BLOCK
                   || (st == XBOX_RESET_IN_BRIDGE
                       && xbox_GuestCpuOrdinalIsWait((uint32_t)s->where))) {
            s->stop_ns = s_t0_ns;
            s->stop_where = (uint32_t)s->where;
            s->stopped = XBOX_RESET_STOP_WAITING;
        }
        InterlockedExchange(&s->in_sample, 1);
        alive++;
    }
    InterlockedExchange((volatile LONG *)&g_xbox_reset_probe, 1);
    deadline = s_t0_ns + (uint64_t)RESET_PROBE_WINDOW_MS * 1000000ull;
    for (;;) {
        int left = 0;
        for (i = 0; i < RESET_PROBE_SLOTS; i++)
            if (s_slots[i].used && s_slots[i].in_sample && !s_slots[i].stopped)
                left++;
        if (!left || xbox_HostNowNs() >= deadline)
            break;
        /* Every pass: the CPU holder clears the flag when it has nobody to
         * hand over to, and a back edge calls in only while it is set. */
        InterlockedExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, 1);
        Sleep(1);
    }
    InterlockedExchange((volatile LONG *)&g_xbox_reset_probe, 0);
    /* With the lock off nothing clears it, and every back edge would call
     * in for the rest of the run. With it on the holder clears it itself. */
    if (!xbox_GuestCpuOn())
        InterlockedExchange((volatile LONG *)&g_xbox_guest_cpu_preempt, 0);

    for (i = 0; i < RESET_PROBE_SLOTS; i++) {
        ResetProbeSlot *s = &s_slots[i];
        if (!s->in_sample)
            continue;
        if (s->stopped) {
            uint64_t d = s->stop_ns > s_t0_ns ? s->stop_ns - s_t0_ns : 0;
            stopped++;
            if (d > max_ns)
                max_ns = d;
            if (d > 100000000ull)
                late++;
            fprintf(stderr, "[RESET-PROBE] flip=%u tid=%lu stop_us=%llu at=%s where=%u\n",
                    flip, s->tid, (unsigned long long)(d / 1000u),
                    stop_name((int)s->stopped), s->stop_where);
        } else {
            fprintf(stderr, "[RESET-PROBE] flip=%u tid=%lu NOT STOPPED in %d ms, last=%s where=%ld\n",
                    flip, s->tid, RESET_PROBE_WINDOW_MS,
                    state_name((int)s->state), (long)s->where);
        }
        s->in_sample = 0;
    }
    fprintf(stderr, "[RESET-PROBE] flip=%u threads=%d stopped=%d max_ms=%.3f late_100ms=%d\n",
            flip, alive, stopped, (double)max_ns / 1e6, late);
    fflush(stderr);
    out.flip = flip;
    out.threads = alive;
    out.stopped = stopped;
    out.max_ns = max_ns;
    out.late_100ms = late;
    s_last = out;
    InterlockedIncrement(&s_last_seq);
}

static DWORD WINAPI probe_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(s_kick, INFINITE);
        run_sample((uint32_t)InterlockedExchange(&s_pending_flip, 0));
        InterlockedExchange(&s_busy, 0);
    }
    return 0;
}

static int probe_start(void)
{
    if (InterlockedCompareExchange(&s_thread_up, 1, 0) == 0) {
        HANDLE t;
        s_kick = CreateEventA(NULL, FALSE, FALSE, NULL);
        t = CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
        if (!s_kick || !t) {
            fprintf(stderr, "[RESET-PROBE] cannot start the probe thread\n");
            InterlockedExchange(&s_thread_up, 2);
            return 0;
        }
        CloseHandle(t);
        InterlockedExchange(&s_thread_up, 3);
    }
    while (InterlockedCompareExchange(&s_thread_up, 0, 0) == 1)
        Sleep(0);
    return s_thread_up == 3;
}

static int open_sample(uint32_t flip)
{
    if (!probe_start())
        return 0;
    /* One at a time: a flip listed while the last is still open is
     * skipped, and said so. */
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) {
        fprintf(stderr, "[RESET-PROBE] flip=%u skipped: the last sample is still open\n", flip);
        return 0;
    }
    InterlockedExchange(&s_pending_flip, (LONG)flip);
    SetEvent(s_kick);
    return 1;
}

void xbox_ResetProbeFlip(uint32_t flip)
{
    const char *spec;

    if (s_n_items < 0) {
        spec = recomp_env(RENV_RESET_PROBE);
        s_n_items = spec ? parse_items(spec, s_items, RESET_PROBE_ITEMS) : 0;
        if (spec && !s_n_items)
            fprintf(stderr, "[RESET-PROBE] reset_probe=%s: no flips in it (N, A-B or A-B/S, comma-separated)\n",
                    spec);
    }
    if (items_list(s_items, s_n_items, flip))
        open_sample(flip);
}

int xbox_ResetProbeSampleNow(uint32_t flip, XboxResetProbeSample *out)
{
    LONG seq = InterlockedCompareExchange(&s_last_seq, 0, 0);
    uint64_t t0 = xbox_HostNowNs();

    if (!open_sample(flip))
        return 0;
    while (InterlockedCompareExchange(&s_last_seq, 0, 0) == seq) {
        if (xbox_HostNowNs() - t0 > (uint64_t)(RESET_PROBE_WINDOW_MS + 5000) * 1000000ull)
            return 0;
        Sleep(1);
    }
    if (out)
        *out = s_last;
    return 1;
}
