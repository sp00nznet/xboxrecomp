/*
 * RECOMP_TRACE=apu_lock: who holds the APU model's d->lock, for how long,
 * and what the other side waits for it.
 *
 * The frame thread holds d->lock for its whole loop and drops it only in its
 * pacing wait, around an interrupt post and once per frame. Every
 * NV1BA0_PIO_VOICE_LOCK the title wrote used to take it too, and under
 * Proton a take that landed mid-frame waited out the rest of the VP's ~80 us;
 * voice_lock is now a lock-free bit flip, so the takers left are the frame
 * thread and the others (APU_LOCK_OTHER). Every 5 s an
 * [APU-LOCK] block gives, per holder, the hold lengths, per taker the
 * acquisitions, how many found the lock taken and the wait, and the time in
 * each se_frame stage, as log2 us histograms.
 *
 * Stats are updated with d->lock held (a hold as it ends, a wait once the
 * lock is had), so the lock itself serialises them.
 */
#pragma once

#include <stdint.h>

struct MCPXAPUState;

enum {
    APU_LOCK_FRAME,     /* the frame thread */
    APU_LOCK_OTHER,     /* apu_mixer_play's wake of the frame thread */
    APU_LOCK_HOLDERS
};

extern int apu_lockstat_on_;   /* -1 until looked up */
int apu_lockstat_lookup(void);
static inline int apu_lockstat_on(void)
{
    return apu_lockstat_on_ >= 0 ? apu_lockstat_on_ : apu_lockstat_lookup();
}

/* Take d->lock as `who`, counting a contended take and its wait. */
void apu_lockstat_lock(struct MCPXAPUState *d, int who);
/* Release d->lock held as `who`, ending its hold. */
void apu_lockstat_unlock(struct MCPXAPUState *d, int who);
/* Around a condition wait that drops and retakes d->lock: the hold ends
 * before it and a new one starts after (the wait is not contention). */
void apu_lockstat_hold_end(int who);
void apu_lockstat_hold_begin(int who);
/* Frame-thread counters: one se_frame and its length; one pacing wait. */
void apu_lockstat_se_frame(uint64_t ns);
void apu_lockstat_pace_wait(void);
/* se_frame stages, to find where a long frame-thread hold goes. */
enum { APU_STAGE_VP, APU_STAGE_DSP, APU_STAGE_MON, APU_STAGE_XA2, APU_STAGES };
uint64_t apu_lockstat_now_ns(void);
void apu_lockstat_stage(int stage, uint64_t ns);
/* Print the block when 5 s have passed. Call with d->lock held; the text is
 * formatted then and printed by apu_lockstat_flush after the unlock. */
void apu_lockstat_report(void);
void apu_lockstat_flush(void);
