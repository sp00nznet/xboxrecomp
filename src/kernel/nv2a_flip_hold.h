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
 * CPU flipped as fast as the executor could draw (hundreds per second in a
 * simple menu) while its vblank ran at 60. The walker now holds after a
 * FLIP_STALL until the guest's vblank count allows the flip (nv2a_pb_exec.c
 * arms it, the ack loop in xbox_memory_layout.c holds the next walk, the
 * DMA_GET advance and the PFB flush ack).
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
 * The hold is opt-in: RECOMP_FLIP_PACING=1 turns it on, =edge with D3D's
 * rule for A/B runs; unset, flips complete at once as before.
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
    int      edge;          /* RECOMP_FLIP_PACING=edge: D3D's rule */
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
    /* Statistics, for the [GPU] report; the caller resets them. */
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

/* A frame counter the toolkit bumps: does something else (the title's own
 * vblank DPC) keep moving it? Called once per bump with the value the bump
 * found and the time in ms. Returns 1 while the counter is the title's, and
 * then the caller does not bump it. Never the title's before the first bump
 * has seeded last_written.
 *
 * One move is not enough. Burnout 3's counter moves once on its own at boot
 * (3 after the toolkit's 2) and never again; taken as the title's there, it
 * was left alone, and the title waited on it at its first movie for good.
 * Nor is a slow trickle: something writing the counter once a second would
 * pile up moves over a few seconds. So a counter is the title's once it has
 * moved on its own at XBOX_FRAME_COUNTER_FOREIGN bumps within
 * XBOX_FRAME_COUNTER_WINDOW_MS, which a counter the title's DPC drives at
 * 60 Hz reaches within a few frames at any flip rate above 4 per second.
 * And it is the title's only while it keeps moving: one that has not moved
 * for XBOX_FRAME_COUNTER_IDLE_MS (15 vblanks) goes back to the toolkit, so
 * a wrong hand-over costs a quarter of a second, not the rest of the run. */
#define XBOX_FRAME_COUNTER_FOREIGN    3
#define XBOX_FRAME_COUNTER_WINDOW_MS  500u
#define XBOX_FRAME_COUNTER_IDLE_MS    250u

typedef struct {
    uint32_t foreign;   /* moves on its own in the current window */
    uint32_t first_ms;  /* when the window's first move was seen */
    int      owned;     /* the title's: not bumped */
    uint32_t last_seen; /* owned: the value last read */
    uint32_t moved_ms;  /* owned: when it last moved */
} XboxFrameCounterWatch;

int xbox_FrameCounterOwned(XboxFrameCounterWatch *w, int seeded,
                           uint32_t last_written, uint32_t cur,
                           uint32_t now_ms);

/* One bump of a registered counter word, the part of xbox_memory_layout.c's
 * frame_counter_bump after the guest pointers are resolved. pacing is
 * RECOMP_FLIP_PACING: off, every call adds one, as before the hold, and the
 * watch never runs. On, the watch decides. Returns what happened, for the
 * caller's log line. */
typedef struct {
    uint32_t last_written;
    int      seeded;
    XboxFrameCounterWatch watch;
} XboxFrameCounterBump;

enum {
    XBOX_FRAME_COUNTER_BUMPED,       /* the toolkit's: one added */
    XBOX_FRAME_COUNTER_TITLES,       /* the title's: left alone */
    XBOX_FRAME_COUNTER_HANDED_OVER,  /* became the title's at this call */
    XBOX_FRAME_COUNTER_HANDED_BACK   /* stopped moving: the toolkit's again */
};

int xbox_FrameCounterBump(XboxFrameCounterBump *b, volatile uint32_t *word,
                          int pacing, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_FLIP_HOLD_H */
