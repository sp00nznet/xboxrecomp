/**
 * A virtual audio device clocked by the host's monotonic time.
 *
 * SDL's "dummy" driver (headless runs) pulls one buffer per sleep and never
 * corrects the sleep's overshoot, so it drains at 0.93-1.01 of the rate it
 * was opened at, moving with load. The APU paces itself by the device queue,
 * so that error became the guest's audio clock. This plays the submitted
 * frames at exactly `rate` frames a second instead; on running dry it plays
 * silence, as a device does, and resumes at the next submit. Only the dummy
 * driver gets it; "disk" and the real drivers keep SDL's own queue.
 *
 * Every call takes the time, so tests can drive it with a fake clock. Not
 * thread safe: the caller serialises.
 */
#ifndef APU_VCLOCK_H
#define APU_VCLOCK_H

#include <stdint.h>

typedef struct apu_vclock {
    uint32_t rate;           /* frames per second */
    uint64_t submitted;      /* frames ever handed over */
    uint64_t anchor_played;  /* play position at anchor_ns */
    int64_t  anchor_ns;
    int      started;        /* a first submit has anchored the clock */
} apu_vclock;

void     apu_vclock_init(apu_vclock *c, uint32_t rate);
/* Hand n frames over at now_ns. */
void     apu_vclock_submit(apu_vclock *c, uint32_t n, int64_t now_ns);
/* Frames handed over and not yet played at now_ns. */
uint64_t apu_vclock_queued(apu_vclock *c, int64_t now_ns);
/* Frames played by now_ns (silence not counted). */
uint64_t apu_vclock_played(apu_vclock *c, int64_t now_ns);

#endif
