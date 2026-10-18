/*
 * The flip hold's rule (nv2a_flip_hold.c).
 *
 * A FLIP_STALL may complete only once the guest's vblank count has reached
 * the previous flip's vblank plus the swap's interval. Driven here with
 * made-up counts and clocks: interval 1 frames that finish 5 ms into a
 * vblank (held to the next edge), interval 2 (held to every second edge),
 * IMMEDIATE (never held), a Swap without its NOP (interval 1), a 20 ms frame
 * (not held, but held to the next edge under edge), a stopped vblank clock,
 * the 100 ms bound with and without vblanks, ONE_OR_IMMEDIATE under edge,
 * two stalls in one walk, last_flip after a timeout, a count that wraps,
 * and the frame counter ownership rule with its seeding.
 */
#include "nv2a_flip_hold.h"

#include <stdio.h>
#include <string.h>

#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* D3D's swap NOP parameter: (framebuffer | interval | IMMEDIATE) << 5 | 1. */
static uint32_t swap(unsigned interval, int immediate)
{
    return ((0x005E8000u | interval | (immediate ? 8u : 0u)) << 5) | 1u;
}

static void fresh(XboxFlipHold *h, int edge, uint32_t first)
{
    memset(h, 0, sizeof *h);
    h->edge = edge;
    /* The first flip is never held; it sets the count the next is measured
     * from. */
    xbox_FlipHoldSwap(h, swap(1, 0));
    xbox_FlipHoldStall(h, first, 0, 1);
}

int main(void)
{
    XboxFlipHold h;
    uint64_t t = 0;
    int held;

    printf("flip hold: interval 1, frames done 5 ms into a vblank\n");
    fresh(&h, 0, 100);
    check(!h.held && h.last_flip == 100, "first flip completes at once");
    xbox_FlipHoldSwap(&h, swap(1, 0));
    held = xbox_FlipHoldStall(&h, 100, t += 5 * MS, 1);
    check(held && h.target == 101, "second flip in the same vblank is held to 101");
    check(xbox_FlipHoldPoll(&h, 100, t += 5 * MS, 1), "still held at 100");
    check(!xbox_FlipHoldPoll(&h, 101, t += 7 * MS, 1), "released at 101");
    check(h.last_flip == 101 && h.holds == 1, "last flip 101, one hold");
    check(h.held_ns == 12 * MS, "held 12 ms");
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 101, t += 5 * MS, 1) && h.target == 102,
          "next frame held to 102: 60 Hz");

    printf("flip hold: interval 2\n");
    fresh(&h, 0, 200);
    xbox_FlipHoldSwap(&h, swap(2, 0));
    check(xbox_FlipHoldStall(&h, 201, t, 1) && h.target == 202,
          "a frame one vblank later is held to 202");
    check(!xbox_FlipHoldPoll(&h, 202, t + MS, 1), "released at 202");
    xbox_FlipHoldSwap(&h, swap(2, 0));
    check(!xbox_FlipHoldStall(&h, 204, t, 1) && h.last_flip == 204,
          "a frame two vblanks later is not held (a title's own pacing)");
    check(h.iv[2] == 2, "iv2 counted twice");

    printf("flip hold: IMMEDIATE, a missing NOP, other software methods\n");
    fresh(&h, 0, 300);
    xbox_FlipHoldSwap(&h, swap(0, 1));
    check(!xbox_FlipHoldStall(&h, 300, t, 1), "IMMEDIATE is never held");
    check(h.iv[0] == 1, "iv0 counted");
    check(!xbox_FlipHoldSwap(&h, (0x1234u << 5) | 5u), "software method 5 is not a swap");
    check(!xbox_FlipHoldSwap(&h, 0), "NOP 0 is not a swap");
    check(xbox_FlipHoldStall(&h, 300, t, 1) && h.target == 301 && h.nonop == 1,
          "a FLIP_STALL without its NOP is interval 1");
    xbox_FlipHoldCancel(&h, t);
    check(!h.held, "cancel drops the hold");
    xbox_FlipHoldSwap(&h, swap(0, 0));
    check(xbox_FlipHoldStall(&h, 301, t, 1) && h.target == 302,
          "interval 0 without IMMEDIATE reads as ONE");

    printf("flip hold: a 20 ms frame, default and edge\n");
    fresh(&h, 0, 400);
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(!xbox_FlipHoldStall(&h, 401, t, 1) && h.last_flip == 401,
          "default: not held, the frame flips at 401");
    fresh(&h, 1, 400);
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 401, t, 1) && h.target == 402,
          "edge: held to the next vblank, 402");
    xbox_FlipHoldSwap(&h, swap(0, 1));
    xbox_FlipHoldPoll(&h, 402, t, 1);
    check(!xbox_FlipHoldStall(&h, 402, t, 1), "edge: IMMEDIATE still never held");

    printf("flip hold: a stopped clock and the 100 ms bound\n");
    fresh(&h, 0, 500);
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(!xbox_FlipHoldStall(&h, 500, t, 0), "no running clock: not held");
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 500, t, 1), "held with the clock running");
    check(!xbox_FlipHoldPoll(&h, 500, t + MS, 0) && h.clock_stops == 1,
          "the clock stops: released, counted as a clock stop");
    fresh(&h, 0, 600);
    xbox_FlipHoldSwap(&h, swap(1, 0));
    t = 1000 * MS;
    xbox_FlipHoldStall(&h, 600, t, 1);
    check(xbox_FlipHoldPoll(&h, 600, t + 99 * MS, 1), "held at 99 ms");
    check(!xbox_FlipHoldPoll(&h, 600, t + 100 * MS, 1)
          && h.clock_stops == 1 && h.timeouts == 0,
          "100 ms with no vblank: released as a clock stop");
    fresh(&h, 0, 700);
    xbox_FlipHoldSwap(&h, swap(7, 0));
    xbox_FlipHoldStall(&h, 701, t, 1);
    check(!xbox_FlipHoldPoll(&h, 703, t + 100 * MS, 1) && h.timeouts == 1,
          "100 ms with vblanks short of the target: a timeout");

    printf("flip hold: ONE_OR_IMMEDIATE under edge, and IMMEDIATE does not linger\n");
    fresh(&h, 1, 450);
    xbox_FlipHoldSwap(&h, swap(1, 1));
    check(!xbox_FlipHoldStall(&h, 451, t, 1) && h.last_flip == 451,
          "edge: a late ONE_OR_IMMEDIATE flips at once");
    xbox_FlipHoldSwap(&h, swap(1, 1));
    check(xbox_FlipHoldStall(&h, 451, t, 1) && h.target == 452,
          "edge: an early ONE_OR_IMMEDIATE still waits for its vblank");
    xbox_FlipHoldPoll(&h, 452, t, 1);
    check(xbox_FlipHoldStall(&h, 453, t, 1) && h.target == 454,
          "edge: the next stall without a NOP is not taken as IMMEDIATE");

    printf("flip hold: a second stall in one walk, last_flip after a timeout\n");
    fresh(&h, 0, 800);
    t = 2000 * MS;
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 800, t, 1) && h.target == 801,
          "first stall of the walk held to 801");
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 800, t + 2 * MS, 1) && h.target == 802,
          "second stall in the same walk: measured from 801, held to 802");
    check(h.holds == 2 && h.held_ns == 0,
          "two holds, no held time for the one nothing waited on");
    check(!xbox_FlipHoldPoll(&h, 802, t + 30 * MS, 1) && h.held_ns == 28 * MS,
          "released at 802 after 28 ms");
    fresh(&h, 0, 900);
    xbox_FlipHoldSwap(&h, swap(3, 0));
    xbox_FlipHoldStall(&h, 900, t, 1);
    check(!xbox_FlipHoldPoll(&h, 901, t + 100 * MS, 1) && h.timeouts == 1
          && h.last_flip == 903,
          "a timeout releases with last_flip at the target, 903");
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 901, t, 1) && h.target == 904,
          "the next flip is measured from 903");

    printf("flip hold: the count wraps\n");
    fresh(&h, 0, 0xFFFFFFFFu);
    xbox_FlipHoldSwap(&h, swap(1, 0));
    check(xbox_FlipHoldStall(&h, 0xFFFFFFFFu, t, 1) && h.target == 0,
          "held to 0 across the wrap");
    check(xbox_FlipHoldPoll(&h, 0xFFFFFFFFu, t, 1), "still held at 0xFFFFFFFF");
    check(!xbox_FlipHoldPoll(&h, 0, t, 1), "released at 0");

    printf("frame counter ownership\n");
    check(!xbox_FrameCounterOwned(0, 0, 7),
          "not seeded: a counter that starts non-zero is not the title's");
    check(!xbox_FrameCounterOwned(1, 8, 8), "unchanged since the bump: ours");
    check(xbox_FrameCounterOwned(1, 8, 9), "moved on by something else: the title's");
    check(!xbox_FrameCounterOwned(1, 8, 3), "went backwards (device reset): ours");
    check(xbox_FrameCounterOwned(1, 0xFFFFFFFFu, 0), "moved across the wrap: the title's");

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
