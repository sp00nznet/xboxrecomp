/*
 * The vblank schedule (xbox_VblankSchedule, kernel_pacing.c).
 *
 * The timer thread raises vblank N at t0 + N * 10^9 / 60 ns. Driven here with
 * made-up clocks: a steady grid, a pass 50 ms late (caught up one vblank per
 * call, no restart), a 150 ms stall (the grid restarts at now instead of
 * firing a burst), 10^6 vblanks fired at jittered times with no drift, and an
 * epoch near 2^62 ns, a host that has been up for a very long time.
 */
#include "kernel_pacing.h"

#include <stdio.h>
#include <stdlib.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define NS_PER_S 1000000000ull
#define MS       1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Edge n of a grid that starts at t0. */
static uint64_t edge(uint64_t t0, uint64_t n)
{
    return t0 + n * NS_PER_S / 60u;
}

static void steady_grid(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 5 * NS_PER_S, next, late, n;
    int ok = 1;

    /* The first call starts the grid and fires vblank 0. */
    ok &= xbox_VblankSchedule(&s, t0, &next, &late) == 1;
    ok &= next == edge(t0, 1) && late == 0;
    for (n = 1; n < 600 && ok; n++) {
        /* A hair early: nothing yet, and told the same edge. */
        ok &= xbox_VblankSchedule(&s, edge(t0, n) - 1, &next, &late) == 0;
        ok &= next == edge(t0, n);
        ok &= xbox_VblankSchedule(&s, edge(t0, n), &next, &late) == 1;
        ok &= next == edge(t0, n + 1) && late == 0;
    }
    check(ok, "steady grid: one vblank per edge, none early");
    check(s.count == 600 && edge(t0, 600) - t0 == 10 * NS_PER_S,
          "600 vblanks span exactly 10 s");
}

static void late_tick(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 1 * NS_PER_S, now, next, late;
    int fired = 0, restarted = 0;

    xbox_VblankSchedule(&s, t0, &next, &late);
    /* The next pass comes 50 ms late: edges 1, 2 and 3 (16.7, 33.3 and
     * 50 ms) are all due. Each call fires one and reports the next edge,
     * in the past while behind. */
    now = t0 + 50 * MS;
    while (xbox_VblankSchedule(&s, now, &next, &late)) {
        fired++;
        restarted |= late != 0;
        if (fired > 10)
            break;
    }
    check(fired == 3, "50 ms late: the three missed edges fire one per call");
    check(!restarted && s.t0_ns == t0, "50 ms late: the grid keeps its epoch");
    check(next == edge(t0, 4), "50 ms late: then waits for the next edge");
}

static void stall_restart(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 2 * NS_PER_S, now, next, late;
    int r;

    xbox_VblankSchedule(&s, t0, &next, &late);
    /* 150 ms with no pass: edge 1 is 133 ms overdue, past the 100 ms rule. */
    now = t0 + 150 * MS;
    r = xbox_VblankSchedule(&s, now, &next, &late);
    check(r == 1, "150 ms stall: one vblank fires");
    check(late == now - edge(t0, 1), "150 ms stall: reports how late it was");
    check(s.t0_ns == now && next == edge(now, 1),
          "150 ms stall: the grid restarts at now, no burst");
    check(xbox_VblankSchedule(&s, now + 1, &next, &late) == 0,
          "150 ms stall: nothing more until the new edge");
}

static void no_drift(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 3 * NS_PER_S, now = t0, next = 0, late, fired = 0;
    uint32_t rng = 12345;
    const uint64_t N = 1000000;
    int ok = 1;

    /* The caller wakes up to 900 us after each edge, as a host sleep does. */
    while (fired < N) {
        if (xbox_VblankSchedule(&s, now, &next, &late)) {
            fired++;
            ok &= late == 0;
        }
        rng = rng * 1103515245u + 12345u;
        now = next > now ? next + (rng >> 8) % (900 * 1000) : now;
    }
    check(ok, "10^6 jittered vblanks: never restarted");
    check(s.t0_ns == t0 && next == edge(t0, N),
          "10^6 jittered vblanks: the next edge is still t0 + N/60 s");
    /* 10^6 vblanks are 16666.67 s: the rate is exact to the nanosecond. */
    check(next - t0 == 16666666666666ull, "no drift over 10^6 vblanks");
}

static void large_epoch(void)
{
    XboxVblankSchedule s = {0};
    uint64_t t0 = 1ull << 62, next, late, prev, n;
    int ok = 1;

    xbox_VblankSchedule(&s, t0, &next, &late);
    prev = t0;
    for (n = 1; n <= 6000; n++) {
        uint64_t due = next, gap;
        ok &= xbox_VblankSchedule(&s, due, &next, &late) == 1 && late == 0;
        gap = due - prev;
        ok &= gap == 16666666u || gap == 16666667u;
        prev = due;
    }
    check(ok, "epoch near 2^62 ns: every gap is 16.666667 ms");
    check(next == t0 + 6001ull * NS_PER_S / 60u,
          "epoch near 2^62 ns: no wrap, 100 s on the grid");
}

int main(void)
{
    printf("vblank_schedule:\n");
    steady_grid();
    late_tick();
    stall_restart();
    no_drift();
    large_epoch();
    printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
