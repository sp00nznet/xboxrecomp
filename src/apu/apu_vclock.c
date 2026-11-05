/**
 * Virtual audio device clock: see apu_vclock.h.
 *
 * The play position is anchor_played + elapsed * rate, computed from a single
 * anchor rather than summed per call, so rounding never accumulates into
 * drift. The anchor moves only when the device runs dry: it stops at what was
 * submitted and plays silence until the next submit, which starts at once.
 */
#include "apu_vclock.h"

void apu_vclock_init(apu_vclock *c, uint32_t rate)
{
    c->rate = rate;
    c->submitted = 0;
    c->anchor_played = 0;
    c->anchor_ns = 0;
    c->started = 0;
}

/* Play position at now_ns; re-anchors at the moment the queue ran dry. */
static uint64_t position(apu_vclock *c, int64_t now_ns)
{
    int64_t dt;
    uint64_t pos, left;

    if (!c->started)
        return c->submitted;
    dt = now_ns - c->anchor_ns;
    if (dt <= 0)
        return c->anchor_played;
    left = c->submitted - c->anchor_played;
    /* Split the product so dt * rate cannot overflow on a long run. */
    pos = (uint64_t)(dt / 1000000000) * c->rate +
          (uint64_t)(dt % 1000000000) * c->rate / 1000000000;
    if (pos >= left) {
        /* Dry: the last frame played at anchor + left / rate. */
        c->anchor_ns += (int64_t)(left / c->rate) * 1000000000 +
                        (int64_t)((left % c->rate) * 1000000000 / c->rate);
        c->anchor_played = c->submitted;
        return c->submitted;
    }
    return c->anchor_played + pos;
}

void apu_vclock_submit(apu_vclock *c, uint32_t n, int64_t now_ns)
{
    uint64_t pos = position(c, now_ns);

    if (!c->started || pos == c->submitted) {
        /* Nothing queued: the device has been playing silence, and these
         * frames start now. */
        c->started = 1;
        c->anchor_ns = now_ns;
        c->anchor_played = pos;
    }
    c->submitted += n;
}

uint64_t apu_vclock_queued(apu_vclock *c, int64_t now_ns)
{
    return c->submitted - position(c, now_ns);
}

uint64_t apu_vclock_played(apu_vclock *c, int64_t now_ns)
{
    return position(c, now_ns);
}
