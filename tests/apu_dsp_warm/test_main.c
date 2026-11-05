/*
 * apu_dsp_warm: the warm-up schedule (src/apu/apu_dsp_warm.h), driven the
 * way apu_dsp_run.c drives it, with a fake worker and a fake clock. The APU
 * thread makes its frames in bursts (8 back to back on the Mac, then a wait
 * for the device), and waits for a worker frame up to a 3 ms deadline. The
 * GP's first frame compiles its program: 522 ms on the Deck. The APU thread
 * must never run a frame inline while the worker has the core, must not wait
 * on it past the deadline, and must hear every frame that fits in it.
 */
#include "apu_dsp_warm.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define DEADLINE_US 3000u
#define BURST       8u
#define BURST_US    (BURST * 667u)      /* 8 APU frames of real time */

typedef struct Fake {
    unsigned long now, busy_until;     /* microseconds */
    unsigned handed;
    unsigned first, rest;           /* us the first and later frames take */
} Fake;

typedef struct Tally {
    unsigned inline_, heard, late, skip, inline_while_busy;
    unsigned long waited_max;
} Tally;

/* APU frame i: step the schedule as gp_frame does, then let time pass. */
static ApuDspWarmStep frame(ApuDspWarm *w, Fake *f, Tally *t, unsigned i)
{
    bool busy = f->now < f->busy_until;
    ApuDspWarmStep step = apu_dsp_warm_step(w, !busy);
    switch (step) {
    case APU_DSP_WARM_INLINE:
        t->inline_++;
        if (busy)
            t->inline_while_busy++;
        break;
    case APU_DSP_WARM_WORKER: {
        unsigned cost = f->handed++ ? f->rest : f->first;
        bool in_time = cost <= DEADLINE_US;
        unsigned long waited = in_time ? cost : DEADLINE_US;
        f->busy_until = f->now + cost;
        f->now += waited;
        if (waited > t->waited_max)
            t->waited_max = waited;
        apu_dsp_warm_ran(w, in_time);
        if (in_time)
            t->heard++;
        else
            t->late++;
        break;
    }
    case APU_DSP_WARM_SKIP:
        t->skip++;
        break;
    }
    /* The burst's frames are back to back; then the device drains. */
    if (i % BURST == BURST - 1)
        f->now += BURST_US;
    return step;
}

/* The GP boot: a 522 ms compile, then 200 us frames in bursts. Only the
 * frames during the compile are lost, never most of every burst. */
static void gp_boot(void)
{
    ApuDspWarm w = { 0 };
    Fake f = { .first = 522000, .rest = 200 };
    Tally t = { 0 };

    /* Before the bootstrap nothing is warming: inline (the caller does not
     * step a core that is not running, but the schedule must say inline). */
    CHECK(apu_dsp_warm_step(&w, true) == APU_DSP_WARM_INLINE, "cold inline");
    t.inline_++;

    apu_dsp_warm_start(&w, 1500);
    for (unsigned i = 0; i < 4000; i++)
        frame(&w, &f, &t, i);

    CHECK(t.inline_while_busy == 0, "%u inline frames while busy",
          t.inline_while_busy);
    CHECK(t.heard + t.late == 1500, "worker frames %u", t.heard + t.late);
    CHECK(t.late == 1 && w.late == 1, "late %u/%llu", t.late,
          (unsigned long long)w.late);
    CHECK(t.waited_max <= DEADLINE_US, "waited %lu us", t.waited_max);
    /* 522 ms is about 98 bursts after the 3 ms wait: 8 frames each. */
    CHECK(t.skip > 700 && t.skip < 820 && w.skipped == t.skip, "skips %u/%llu",
          t.skip, (unsigned long long)w.skipped);
    CHECK(t.inline_ == 4001 - 1500 - t.skip, "inline %u", t.inline_);
    CHECK(w.left == 0, "left %u", w.left);
}

/* Frames slower than the deadline through the whole warm-up (a loaded
 * host): each is late and its successors skipped until it is done, and the
 * APU thread never waits past the deadline or runs a frame inline under the
 * worker. */
static void slow_worker(void)
{
    ApuDspWarm w = { 0 };
    Fake f = { .first = 4000, .rest = 4000 };
    Tally t = { 0 };

    apu_dsp_warm_start(&w, 10);
    for (unsigned i = 0; i < 200; i++)
        frame(&w, &f, &t, i);
    CHECK(t.inline_while_busy == 0, "%u inline while busy",
          t.inline_while_busy);
    CHECK(t.late == 10 && t.heard == 0, "late %u heard %u", t.late, t.heard);
    CHECK(t.skip > 0 && w.skipped == t.skip, "skip %u", t.skip);
    CHECK(t.waited_max == DEADLINE_US, "waited %lu us", t.waited_max);
}

/* A second bootstrap (the title resets the GP) warms again; one during a
 * warm-up starts the count over. */
static void restart(void)
{
    ApuDspWarm w = { 0 };
    apu_dsp_warm_start(&w, 3);
    CHECK(apu_dsp_warm_step(&w, true) == APU_DSP_WARM_WORKER, "1");
    apu_dsp_warm_start(&w, 3);
    for (int i = 0; i < 3; i++)
        CHECK(apu_dsp_warm_step(&w, true) == APU_DSP_WARM_WORKER, "restart %d",
              i);
    CHECK(apu_dsp_warm_step(&w, true) == APU_DSP_WARM_INLINE, "after");
    CHECK(apu_dsp_warm_step(&w, false) == APU_DSP_WARM_SKIP, "busy skips");
    CHECK(w.skipped == 1, "skipped %llu", (unsigned long long)w.skipped);
}

int main(void)
{
    gp_boot();
    slow_worker();
    restart();
    if (failures)
        return 1;
    printf("apu_dsp_warm: ok\n");
    return 0;
}
