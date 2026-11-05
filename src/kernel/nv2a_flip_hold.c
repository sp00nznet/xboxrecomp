/*
 * nv2a_flip_hold.c - when a flip may complete (see nv2a_flip_hold.h).
 */
#include "nv2a_flip_hold.h"

#include <string.h>

int xbox_FlipPacingMode(const char *env, int virtual_clock)
{
    if (!env || !*env)
        return virtual_clock ? XBOX_FLIP_PACING_ON : XBOX_FLIP_PACING_OFF;
    if (!strcmp(env, "0"))
        return XBOX_FLIP_PACING_OFF;
    if (!strcmp(env, "edge"))
        return XBOX_FLIP_PACING_EDGE;
    return XBOX_FLIP_PACING_ON;
}

int xbox_FlipHoldSwap(XboxFlipHold *h, uint32_t param)
{
    uint32_t payload;

    /* D3D's software methods: the low five bits say which (1 = the swap),
     * the rest is its payload. The swap's payload is the framebuffer
     * address with the interval in bits 0-2 (DEFAULT and ONE are 1, TWO 2,
     * THREE 3) and IMMEDIATE in bit 3, as the XDK's Swap encodes it and
     * D3D's interrupt handler decodes it. */
    if ((param & 0x1Fu) != 1u)
        return 0;
    payload = param >> 5;
    h->have_swap = 1;
    h->swap_interval = payload & 7u;
    h->swap_immediate = (payload & 8u) != 0;
    return 1;
}

static void release(XboxFlipHold *h, uint64_t now_ns, int waited)
{
    /* last_flip = max(target, count at the stall); a hold only exists
     * while the stall's count was below the target. */
    h->last_flip = h->target;
    h->held = 0;
    if (waited && now_ns > h->held_since_ns)
        h->held_ns += now_ns - h->held_since_ns;
}

void xbox_FlipHoldPutRead(XboxFlipHold *h, uint32_t vbl, uint32_t put,
                          uint64_t now_ns)
{
    /* A new PUT: the previous read saw the old one, so the kick that moved
     * it was stored after that read's count. */
    if (h->have_pass && put != h->pass_put) {
        h->have_bound = 1;
        h->bound_vbl = h->pass_vbl;
        h->bound_ns = h->pass_ns;
    }
    h->have_pass = 1;
    h->pass_put = put;
    h->pass_vbl = vbl;
    h->pass_ns = now_ns;
}

void xbox_FlipHoldWalk(XboxFlipHold *h, uint64_t behind_ns)
{
    h->have_kick = h->have_bound && behind_ns
                && (int64_t)(behind_ns - h->bound_ns) > 0;
    h->kick_count = h->bound_vbl;
}

/* A flip recorded before the walk's count: a late walk, for the trace. */
static void late_walk(XboxFlipHold *h, uint32_t count)
{
    if ((int32_t)(count - h->last_flip) > 0) {
        uint32_t moved = count - h->last_flip;
        h->late_walks++;
        if (moved > h->late_max)
            h->late_max = moved;
    }
}

int xbox_FlipHoldStall(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                       int clock_ok)
{
    unsigned interval;
    int immediate = 0;
    uint32_t kick = count;

    /* The earliest vblank the title can have kicked this frame in, when a
     * catch-up made the walk late (nv2a_flip_hold.h). Not bounded to one
     * vblank: in the catch-up a walk a few milliseconds late is several
     * vblanks late, and a one-vblank bound left those flips late and the
     * grid shifted. A bound newer than the count (it cannot be) is
     * ignored. A second stall in the walk uses the count. */
    if (h->have_kick && (int32_t)(count - h->kick_count) > 0)
        kick = h->kick_count;
    h->have_kick = 0;

    /* A second stall in one walk: the first was never polled, so nothing
     * waited on it. Its target still counts as that flip's vblank. */
    if (h->held)
        release(h, now_ns, 0);
    if (h->have_swap) {
        interval = h->swap_interval;
        immediate = h->swap_immediate;
        /* IMMEDIATE with no interval flips at once. The encoder never
         * makes interval 0 without IMMEDIATE; read it as ONE. */
        if (!interval && !immediate)
            interval = 1;
    } else {
        /* No swap NOP: the triple-buffer Swap, or another XDK. ONE. */
        interval = 1;
        h->nonop++;
    }
    h->have_swap = 0;
    h->swap_immediate = 0;
    h->iv[interval < 3 ? interval : 3]++;

    /* No target to hold to: it flips now, so at the kick's vblank when the
     * walk was late (a swap kicked inside a CALL shifted the grid too). */
    if (!interval || !h->have_last || !clock_ok) {
        h->have_last = 1;
        h->last_flip = kick;
        late_walk(h, count);
        return 0;
    }
    h->target = h->last_flip + interval;
    /* D3D pushes a late frame to the next vblank, except ONE_OR_IMMEDIATE
     * (interval 1 with IMMEDIATE), which flips at once when it is late. */
    if (h->edge && !immediate && (int32_t)(kick + 1u - h->target) > 0)
        h->target = kick + 1u;
    if ((int32_t)(count - h->target) >= 0) {
        /* Not held: it flipped in the vblank it was kicked in, or on its
         * target when the kick may have come in time for it. Taking the
         * walk's count here is what shifted the grid after a late walk. */
        h->last_flip = (int32_t)(kick - h->target) > 0 ? kick : h->target;
        late_walk(h, count);
        return 0;
    }
    h->held = 1;
    h->stall_count = count;
    h->held_since_ns = now_ns;
    h->holds++;
    return 1;
}

int xbox_FlipHoldPoll(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                      int clock_ok)
{
    if (!h->held)
        return 0;
    if ((int32_t)(count - h->target) >= 0) {
        release(h, now_ns, 1);
        return 0;
    }
    if (!clock_ok) {
        h->clock_stops++;
        release(h, now_ns, 1);
        return 0;
    }
    if (now_ns - h->held_since_ns >= XBOX_FLIP_HOLD_MAX_NS) {
        /* Vblanks came and the target still did not: the rule is wrong
         * somewhere, and the gate runs expect none of these. Without a
         * vblank at all it is the clock that stopped (a host stall). */
        if (count != h->stall_count)
            h->timeouts++;
        else
            h->clock_stops++;
        release(h, now_ns, 1);
        return 0;
    }
    return 1;
}

void xbox_FlipHoldCancel(XboxFlipHold *h, uint64_t now_ns)
{
    if (h->held)
        release(h, now_ns, 1);
    h->have_swap = 0;
    h->swap_immediate = 0;
}

int xbox_FrameCounterOwned(XboxFrameCounterWatch *w, int seeded,
                           uint32_t last_written, uint32_t cur,
                           uint32_t now_ms)
{
    if (w->owned) {
        if (cur != w->last_seen) {
            w->last_seen = cur;
            w->moved_ms = now_ms;
            return 1;
        }
        if (now_ms - w->moved_ms < XBOX_FRAME_COUNTER_IDLE_MS)
            return 1;
        /* Stopped: the toolkit bumps it again, from where it is. */
        w->owned = 0;
        w->foreign = 0;
        return 0;
    }
    if (!seeded)
        return 0;
    /* Backwards is a device reset, not the title counting: still ours. */
    if ((int32_t)(cur - last_written) <= 0)
        return 0;
    if (!w->foreign || now_ms - w->first_ms > XBOX_FRAME_COUNTER_WINDOW_MS) {
        w->foreign = 1;
        w->first_ms = now_ms;
    } else {
        w->foreign++;
    }
    if (w->foreign < XBOX_FRAME_COUNTER_FOREIGN)
        return 0;
    w->owned = 1;
    w->foreign = 0;
    w->last_seen = cur;
    w->moved_ms = now_ms;
    return 1;
}

int xbox_FrameCounterBump(XboxFrameCounterBump *b, volatile uint32_t *word,
                          int pacing, uint32_t now_ms)
{
    uint32_t cur = *word;
    int was = b->watch.owned;

    if (pacing && xbox_FrameCounterOwned(&b->watch, b->seeded,
                                         b->last_written, cur, now_ms))
        return was ? XBOX_FRAME_COUNTER_TITLES : XBOX_FRAME_COUNTER_HANDED_OVER;
    *word = cur + 1;
    b->last_written = cur + 1;
    b->seeded = 1;
    return was ? XBOX_FRAME_COUNTER_HANDED_BACK : XBOX_FRAME_COUNTER_BUMPED;
}
