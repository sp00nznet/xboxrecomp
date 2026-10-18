/* apu_vclock: the virtual device that stands in for SDL's dummy driver
 * plays at exactly its rate, plays silence when dry, and never drifts.
 *
 *   apu_vclock_test rate    pace like mcpx_apu_pace_step for an hour
 *   apu_vclock_test dry     run dry, wait, submit again
 *   apu_vclock_test edges   sub-frame steps, dt <= 0, a day-long dt
 */
#include "apu_vclock.h"

#include <stdio.h>
#include <string.h>

#define RATE   48000
#define PERIOD 256          /* one EP period */
#define NS     1000000000LL

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* The queue pacing of apu_core.c: render ahead to the high mark, then wait
 * one period's worth of time; jitter the wake-ups so rounding is exercised. */
static void test_rate(void)
{
    apu_vclock c;
    int64_t t = 1000 * NS, t0, step = (int64_t)PERIOD * NS / RATE;
    uint64_t sub0, played, i = 0;
    double ratio;

    apu_vclock_init(&c, RATE);
    while (apu_vclock_queued(&c, t) < 2048)
        apu_vclock_submit(&c, PERIOD, t);
    t0 = t;
    sub0 = c.submitted;
    while (t - t0 < 3600 * NS) {
        t += step + (int64_t)((i++ * 7919) % 3000000) - 1500000;
        while (apu_vclock_queued(&c, t) < 1024)
            apu_vclock_submit(&c, PERIOD, t);
        CHECK("rate: never dry while paced", apu_vclock_queued(&c, t) > 0);
        if (failures) return;
    }
    played = apu_vclock_played(&c, t) - (sub0 - 2048);
    ratio = (double)played / ((double)(t - t0) / NS * RATE);
    printf("rate: %.7f over an hour, %llu submitted\n", ratio,
           (unsigned long long)(c.submitted - sub0));
    /* One frame of rounding at most, over 172.8 M frames. */
    CHECK("rate: exact", played + 1 >= (uint64_t)((t - t0) / 1000) * RATE / 1000000 &&
                         played <= (uint64_t)((t - t0) / 1000) * RATE / 1000000 + 1);
}

static void test_dry(void)
{
    apu_vclock c;
    int64_t t = 5 * NS;

    apu_vclock_init(&c, RATE);
    CHECK("dry: empty before any submit", apu_vclock_queued(&c, t) == 0);
    CHECK("dry: nothing played before any submit", apu_vclock_played(&c, t) == 0);

    /* Waiting before the first submit must not bank time. */
    t += 2 * NS;
    apu_vclock_submit(&c, 4800, t);                        /* 100 ms */
    CHECK("dry: first submit fully queued", apu_vclock_queued(&c, t) == 4800);
    CHECK("dry: half after 50 ms", apu_vclock_queued(&c, t + NS / 20) == 2400);

    /* Dry after 100 ms; a second of silence follows. */
    CHECK("dry: empty at 100 ms", apu_vclock_queued(&c, t + NS / 10) == 0);
    CHECK("dry: empty after 1 s", apu_vclock_queued(&c, t + NS) == 0);
    CHECK("dry: played all of it", apu_vclock_played(&c, t + NS) == 4800);

    /* The next submit plays from the moment it arrives, not from where the
     * clock would have been: the silence is not made up. */
    t += 2 * NS;
    apu_vclock_submit(&c, 480, t);
    CHECK("dry: restart fully queued", apu_vclock_queued(&c, t) == 480);
    CHECK("dry: restart drains at the rate", apu_vclock_queued(&c, t + NS / 200) == 240);
    CHECK("dry: restart played count", apu_vclock_played(&c, t + NS / 200) == 4800 + 240);

    /* A submit while frames are queued does not move the clock. */
    apu_vclock_submit(&c, 480, t + NS / 200);
    CHECK("dry: queued submit appends", apu_vclock_queued(&c, t + NS / 200) == 720);
    CHECK("dry: and drains on", apu_vclock_queued(&c, t + NS / 100) == 480);
}

static void test_edges(void)
{
    apu_vclock c;
    int64_t t = 0;

    apu_vclock_init(&c, RATE);
    apu_vclock_submit(&c, 1000, t);
    /* 1/48000 s is 20833.3 ns: no frame at 20833, one at 20834. */
    CHECK("edges: no frame before its time", apu_vclock_queued(&c, 20833) == 1000);
    CHECK("edges: one frame at its time", apu_vclock_queued(&c, 20834) == 999);
    CHECK("edges: time going back holds", apu_vclock_queued(&c, -5) == 1000);

    /* A day-long dt into a deep queue must not overflow. */
    apu_vclock_init(&c, RATE);
    apu_vclock_submit(&c, 0xFFFFFFFFu, 0);
    apu_vclock_submit(&c, 0xFFFFFFFFu, 0);
    CHECK("edges: a day of play", apu_vclock_played(&c, 86400 * NS) == 86400ULL * RATE);
}

int main(int argc, char **argv)
{
    const char *t = argc > 1 ? argv[1] : "";

    if (!strcmp(t, "rate")) test_rate();
    else if (!strcmp(t, "dry")) test_dry();
    else if (!strcmp(t, "edges")) test_edges();
    else { printf("usage: apu_vclock_test rate|dry|edges\n"); return 2; }
    if (failures) return 1;
    printf("PASS %s\n", t);
    return 0;
}
