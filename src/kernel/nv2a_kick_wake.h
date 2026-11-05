/*
 * nv2a_kick_wake.h - the walker sleeps until the guest has GPU work for it.
 *
 * D3D's KickOff sets the PFB flush bit, spins until the ack loop clears it
 * (xbox_memory_layout.c, NV2A_KICK_REG), then stores DMA_PUT. The guest's
 * stores are plain memory writes, so the loop used to find them by polling
 * both words every ~50 us while the title was kicking: a third of a core on
 * the Deck (Wine's 50 us nap is ~110 us) and a quarter on the Mac, for a
 * title that kicks a thousand times a second.
 *
 * With RECOMP_DEBUG=kick_event=1 (off by default) the loop sleeps
 * instead, and three things end the sleep:
 *
 *   - the KickOff's own spin. Its loop is a lowered spin-wait site
 *     (spin_waits.json), whose RECOMP_SPIN_WAIT calls xbox_SpinWait while
 *     the walker is asleep (g_xbox_kick_armed), at either pacing. The pass
 *     pokes: work pending (the flush bit set, or PUT past what the walker
 *     executed) wakes the walker. Any other lowered loop (a vblank wait, a
 *     wait on GET) pokes the same way;
 *   - the PUT store right after an acknowledged kick, which no guest pass
 *     follows: the walker watches PUT for a short spin after each clear
 *     (xbox_KickWakeWatchPut), the guest being one store away, and polls
 *     when the store does not come, until PUT moves, at most 16 ms
 *     (xbox_KickWakePolling);
 *   - a vblank (a held flip releases on the count), and the loop's own 1 ms
 *     tick, the fallback that bounds any wake that never comes. While a
 *     flip is held the walker sleeps for these two only: the flush bit
 *     stays set, and pokes would wake it for nothing.
 *
 * A title whose KickOff spin is not lowered never pokes: its kicks (or
 * its PUTs) are found only by the timeout. Three such kicks in a row turn the event wait
 * off for the rest of the run and the loop polls as before.
 *
 * No wakeup is lost between the walker's last check and its sleep: the
 * walker raises `armed` and then reads the guest's words, the guest stores
 * its word and then reads `armed`, each with a seq_cst fence between
 * (Dekker), so at least one sees the other; a wake that lands before the
 * sleep is kept by the waker. The guest's fence is in RECOMP_SPIN_WAIT,
 * before it reads `armed`: without it a loop that leaves after one pass
 * (the store and the read reordered past each other) lost about one poke
 * in a thousand in a litmus run. The same holds for a vblank and a held
 * flip: the timer thread counts and then reads `armed`, the walker arms and
 * then reads the count (xbox_KickWakeSleepNudge).
 *
 * The protocol is here on its own, over pointers to the words, so
 * tests/kick_wake drives it with plain memory and threads.
 */
#ifndef NV2A_KICK_WAKE_H
#define NV2A_KICK_WAKE_H

#include <stdint.h>

#include "platform/host_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kicks in a row found by the timeout before the event wait is given up. */
#define XBOX_KICK_WAKE_MISS_LIMIT 3

/* How long a PUT watch that ran out keeps the walker polling when PUT does
 * not move (xbox_KickWakePolling): a frame at 60 Hz. A guest thread late by
 * more than that has missed its frame already, and a KickOff that stores
 * the PUT it found (nothing new) must not hold the loop at the poll. */
#define XBOX_KICK_WAKE_CARRY_NS 16000000ull

typedef struct {
    volatile int *armed;             /* the flag RECOMP_SPIN_WAIT reads */
    XboxHostWaker *waker;
    volatile uint32_t *kick;         /* PFB 0x100410 */
    uint32_t kick_bit;
    volatile uint32_t *put;          /* DMA_PUT */
    volatile uint32_t *done_put;     /* the PUT the walker executed up to */
    uint32_t (*vblank_count)(void);  /* the count a held flip releases on */
    int off;                         /* given up: the loop polls */
    int misses_in_row;
    /* A PUT watch that ran out (xbox_KickWakeWatchPut): the PUT it waited
     * to leave, and when, until PUT moves or the carry ends. */
    int watch_missed;
    uint32_t watch_missed_put;
    uint64_t watch_missed_ns;
    /* Statistics, for the [PACING] line; the reader resets them. */
    volatile long pokes, sleeps, poked, timeouts, early, misses;
    volatile long watch_hits, watch_misses, nudges, held_sleeps;
    volatile long long watch_ns;     /* summed over the hits */
} XboxKickWake;

/* Work the walker has not taken: the flush bit set, or PUT past done_put. */
int  xbox_KickWakePending(const XboxKickWake *k);

/* The walker: sleep up to ns, or until a poke or a nudge. Returns 1 when
 * one ended it (or work was already pending, without sleeping), 0 on the
 * timeout or a spurious wake-up. */
int  xbox_KickWakeSleep(XboxKickWake *k, uint64_t ns);

/* The walker, while a flip is held: the flush bit stays set until the
 * vblank releases the flip, so only a nudge (or the timeout) ends this
 * sleep and the KickOff's passes do not wake it. `seen` is the vblank count
 * the hold was polled at (read before the poll): a count that has moved
 * since returns 1 at once, as a vblank that lands before the arm nudges
 * nobody. 1 when a nudge (or that) ended it. */
int  xbox_KickWakeSleepNudge(XboxKickWake *k, uint64_t ns, uint32_t seen);

/* Any thread, after a store the walker may be waiting for: wake it when it
 * sleeps and work is pending. One load when it does not sleep. */
void xbox_KickWakePoke(XboxKickWake *k);

/* Any thread: wake it when it sleeps, pending work or not (a vblank). */
void xbox_KickWakeNudge(XboxKickWake *k);

/* The walker, right after acknowledging a kick: spin up to ns for PUT to
 * leave put_before. 1 when it did. A watch that runs out (the guest thread
 * preempted between its spin and the store) is remembered for
 * xbox_KickWakePolling: no poke follows that store. */
int  xbox_KickWakeWatchPut(XboxKickWake *k, uint32_t put_before, uint64_t ns);

/* The walker, before it sleeps: 1 while a PUT watch that ran out is still
 * waiting for its store, so the loop polls instead. That holds across
 * ticks: a store that lands during a later tick's sleep would wake nobody
 * and be found only by that tick's timeout, up to 1 ms late. It ends when
 * PUT (as the loop read it) leaves the watched value, a later watch hits,
 * or after XBOX_KICK_WAKE_CARRY_NS. */
int  xbox_KickWakePolling(XboxKickWake *k, uint32_t put);

/* The walker, after a timeout that found work (the flush bit set, or PUT
 * past done_put) with no flip held: the guest stored it without poking. Returns 1 when that turned the
 * event wait off. A kick that a poke found resets the run. */
int  xbox_KickWakeMiss(XboxKickWake *k);
void xbox_KickWakeHit(XboxKickWake *k);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_KICK_WAKE_H */
