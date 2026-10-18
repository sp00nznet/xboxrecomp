/*
 * nv2a_flip_hold.h - when a flip may complete, on the guest's vblank count.
 *
 * On the console a flip completes at a vblank. The XDK's Swap pushes a
 * NO_OPERATION whose parameter is software method 1 (the swap: framebuffer
 * address, presentation interval, IMMEDIATE), then FLIP_INCREMENT_WRITE and
 * FLIP_STALL. PGRAPH traps the NOP to D3D's interrupt handler, which queues
 * the flip for a vblank; the vblank DPC flips and writes PGRAPH_INCREMENT,
 * and only then does the GPU get past FLIP_STALL. The title feels that as
 * a KickOff or a fence that waits.
 *
 * The toolkit raises no GPU interrupts, so none of that runs, and a
 * FLIP_STALL used to end at once: a title that does not pace itself on the
 * CPU flipped as fast as the backend presented (hundreds per second on
 * Metal, the display's 90 Hz on a 90 Hz panel) while its vblank ran at 60. The walker now holds after a FLIP_STALL until
 * the guest's vblank count allows the flip (nv2a_pb_exec.c arms it, the ack
 * loop in xbox_memory_layout.c holds the next walk and the KickOff ack).
 *
 * The rule here is pure: counts and times come in as arguments, so
 * tests/flip_hold drives it without a kernel.
 *
 *   target  = last_flip + interval
 *   release when (int32)(count - target) >= 0
 *
 * Not D3D's max(last + interval, now + 1): a title that already takes its
 * interval per frame (one that waits for two vblanks itself) is never held,
 * and a late frame on a slow backend is not pushed to the next vblank.
 * flip_pacing=edge selects D3D's rule for A/B runs.
 */
#ifndef NV2A_FLIP_HOLD_H
#define NV2A_FLIP_HOLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No hold lasts longer than this, whatever the count says. */
#define XBOX_FLIP_HOLD_MAX_NS    100000000ull
/* A vblank count that has not moved for this long is not a clock. */
#define XBOX_FLIP_CLOCK_STALE_NS 100000000ull

typedef struct {
    int      edge;          /* flip_pacing=edge: D3D's quantising rule */
    /* The latest swap NOP since the previous FLIP_STALL. */
    int      have_swap;
    unsigned swap_interval;
    int      swap_immediate;
    /* The vblank the previous flip was released on. */
    int      have_last;
    uint32_t last_flip;
    /* A pending hold. */
    int      held;
    uint32_t target;
    uint32_t stall_count;
    uint64_t held_since_ns;
    /* Statistics, for RECOMP_TRACE=pacing; the caller resets them. */
    uint32_t holds, timeouts, clock_stops, nonop, iv[4];
    uint64_t held_ns;
} XboxFlipHold;

/* A NO_OPERATION parameter. Returns 1 when it is the swap (software method
 * 1) and was recorded, 0 for anything else, which changes nothing. */
int xbox_FlipHoldSwap(XboxFlipHold *h, uint32_t param);

/* A FLIP_STALL, with the vblank count sampled when the walk that reached it
 * began (a stall reached late in a walk is not taken for a new frame).
 * clock_ok: the count is moving (vblanks on, advanced recently). Returns 1
 * when the flip is now held, 0 when it completes at once. */
int xbox_FlipHoldStall(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                       int clock_ok);

/* Once per pass of the walker's loop. Returns 1 while the flip is still
 * held, 0 once it is released (or none was pending). */
int xbox_FlipHoldPoll(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                      int clock_ok);

/* Drop a pending hold without waiting (a walk that lost its place). */
void xbox_FlipHoldCancel(XboxFlipHold *h, uint64_t now_ns);

/* A frame counter the toolkit bumps: has something else (the title's own
 * vblank DPC) moved it forward since the toolkit last wrote it? Never
 * before the first bump has seeded last_written. */
int xbox_FrameCounterOwned(int seeded, uint32_t last_written, uint32_t cur);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_FLIP_HOLD_H */
