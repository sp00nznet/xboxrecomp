/*
 * nv2a_flip_hold.c - when a flip may complete (see nv2a_flip_hold.h).
 */
#include "nv2a_flip_hold.h"

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

int xbox_FlipHoldStall(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                       int clock_ok)
{
    unsigned interval;
    int immediate = 0;

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

    if (!interval || !h->have_last || !clock_ok) {
        h->have_last = 1;
        h->last_flip = count;
        return 0;
    }
    h->target = h->last_flip + interval;
    /* D3D pushes a late frame to the next vblank, except ONE_OR_IMMEDIATE
     * (interval 1 with IMMEDIATE), which flips at once when it is late. */
    if (h->edge && !immediate && (int32_t)(count + 1u - h->target) > 0)
        h->target = count + 1u;
    if ((int32_t)(count - h->target) >= 0) {
        h->last_flip = count;
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

int xbox_FrameCounterOwned(int seeded, uint32_t last_written, uint32_t cur)
{
    /* Backwards is a device reset, not the title counting: still ours. */
    return seeded && (int32_t)(cur - last_written) > 0;
}
