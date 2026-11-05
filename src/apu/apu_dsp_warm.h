/*
 * The warm-up schedule for one GP or EP core (apu_dsp_run.c).
 *
 * A core's first frames after its bootstrap compile its whole program in
 * the dsp56300 JIT: on the Deck the GP's first frame took 522 ms, and run on
 * the APU thread it starved the audio device for 516 ms. For a while after
 * each bootstrap the core's frames go to a worker thread instead, and the
 * APU thread waits for each one only up to a short deadline. A frame that
 * misses it is late: it is not heard, and the frames that find the worker
 * still busy are skipped and counted, so the APU thread never waits on a
 * compile. The wait matters: the APU thread makes its frames in bursts (on
 * the Mac 8 back to back), and without it only the first frame of a burst
 * found the worker free, so the warm-up skipped 7 frames in 8 for 8 s. Once
 * the warm-up runs out the core runs inline again, as on xemu. Kept apart
 * from the threads so the schedule is testable on its own
 * (tests/apu_dsp_warm).
 */
#ifndef APU_DSP_WARM_H
#define APU_DSP_WARM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ApuDspWarmStep {
    APU_DSP_WARM_INLINE,    /* run the frame on the APU thread */
    APU_DSP_WARM_WORKER,    /* hand the frame to the worker, wait for it
                             * up to the deadline, then apu_dsp_warm_ran */
    APU_DSP_WARM_SKIP,      /* the worker still has the core: skip it */
} ApuDspWarmStep;

typedef struct ApuDspWarm {
    uint32_t left;          /* frames still to hand to the worker */
    uint64_t skipped;       /* frames not run: the worker still had the core */
    uint64_t late;          /* frames that ran but missed the deadline */
} ApuDspWarm;

/* At a bootstrap: the next n frames go to the worker. */
static inline void apu_dsp_warm_start(ApuDspWarm *w, uint32_t n)
{
    w->left = n;
}

/* What to do with this frame. worker_idle: the worker does not have the
 * core (or was never started). */
static inline ApuDspWarmStep apu_dsp_warm_step(ApuDspWarm *w, bool worker_idle)
{
    if (!worker_idle) {
        w->skipped++;
        return APU_DSP_WARM_SKIP;
    }
    if (w->left) {
        w->left--;
        return APU_DSP_WARM_WORKER;
    }
    return APU_DSP_WARM_INLINE;
}

/* After a WORKER frame: in_time, the worker finished it within the deadline
 * and its output is this frame's. */
static inline void apu_dsp_warm_ran(ApuDspWarm *w, bool in_time)
{
    if (!in_time)
        w->late++;
}

#endif
