/*
 * The NV2A's notify interrupt for D3D's software methods, and the walker's
 * bounded run-ahead that needs it (RECOMP_DEBUG=run_ahead, on by default
 * at depth 64; see nv2a_run_ahead_depth).
 *
 * On the console a NO_OPERATION with a non-zero parameter is trapped by
 * PGRAPH: it latches the method and parameter in TRAPPED_ADDR and
 * TRAPPED_DATA_LOW, sets NSOURCE to NOTIFICATION and raises PGRAPH_INTR's
 * ERROR bit, and PFIFO stops pulling until the interrupt is cleared (xemu
 * pgraph.c NV097_NO_OPERATION, pfifo.c waiting_for_nop; Cxbx-Reloaded
 * EmuNV2A_PGRAPH.cpp). D3D's ServiceGrInterrupt reads those registers and
 * runs software method (param & 0x1F) with data (param >> 5): 1 is the swap,
 * 5 sets the event BlockOnTime sleeps on when the GPU is more than 8 KB
 * behind the fence it waits for.
 *
 * Here nothing raised it, which was harmless only while the KickOff flush
 * ack kept the title within one segment of the walker: BlockOnTime never
 * found the GPU that far behind. A run-ahead walker acks the flush before
 * it has caught up, the far path is taken, and without this the title
 * sleeps forever on an event nothing sets.
 *
 * Where this model differs from the console:
 *   - Only the methods in the mask are trapped (5 by default); the rest of
 *     the non-zero parameters pass as before, where PGRAPH traps them all.
 *     The method numbers are XDK D3D's, checked against XDK 5849's D3D8;
 *     another build may number them differently.
 *   - TRAPPED_ADDR's channel field is left 0 and PGRAPH_INTR_EN is not
 *     consulted (D3D enables ERROR).
 *   - The registers are plain RAM, so the write-1-to-clear the handler does
 *     is a plain store and the trap's end is inferred: ServiceGrInterrupt
 *     writes 0 to PGRAPH_FIFO (0x400720) on entry and 1 on its way out, the
 *     raise leaves a value it never writes, and the handler has run once
 *     PGRAPH_FIFO reads 1 again. That is a level between passes, not the
 *     edge the console sees, and the handler's DPC calls it again while
 *     the latched bits stay up, so a sampler can miss every exit. The ack
 *     thread makes it an edge: over a pass in flight (0) it leaves a mark
 *     only the handler's writes erase, and a 0 or 1 after its mark is an
 *     exit. It must watch for as long as a notify is pending: only a clear
 *     between the handler's write-back and its re-read ends the loop.
 *   - PFIFO stops at a trap until it is cleared; the walk here stalls at
 *     most a bounded time and then goes on, and a notify that finds one
 *     still pending is queued behind it rather than raised over it.
 *
 * Pure functions over the register block (a pointer to the NV2A's MMIO
 * window, byte offsets), the pending/queued state and the run-ahead's ack
 * rule, so tests/gr_notify runs them on every host.
 */
#ifndef NV2A_GR_NOTIFY_H
#define NV2A_GR_NOTIFY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NV2A_GR_PMC_INTR_0          0x000100u
#define NV2A_GR_PMC_INTR_EN_0       0x000140u
#define NV2A_GR_PMC_INTR_PGRAPH     (1u << 12)
#define NV2A_GR_PGRAPH_INTR         0x400100u
#define NV2A_GR_PGRAPH_INTR_ERROR   (1u << 20)
#define NV2A_GR_PGRAPH_NSOURCE      0x400108u
#define NV2A_GR_NSOURCE_NOTIFICATION 1u
#define NV2A_GR_PGRAPH_TRAPPED_ADDR 0x400704u
#define NV2A_GR_PGRAPH_TRAPPED_DATA 0x400708u
#define NV2A_GR_PGRAPH_FIFO         0x400720u
/* What the raise leaves in PGRAPH_FIFO, and what the ack thread leaves
 * over a pass in flight (nv2a_gr_poll): the handler writes only 0 and 1,
 * and the title never reads it. */
#define NV2A_GR_FIFO_RAISED         0x2u
#define NV2A_GR_FIFO_MARK           0x3u
#define NV2A_GR_NOP_METHOD          0x100u

/* Software methods delivered by default: 5, BlockOnTime's event. Never 1:
 * the swap is the flip hold's (nv2a_flip_hold.h), and its handler would
 * program a scanout this runtime does not have. */
#define NV2A_GR_NOTIFY_DEFAULT_MASK (1u << 5)

#define NV2A_GR_REG(regs, off) \
    (*(volatile uint32_t *)((volatile char *)(regs) + (off)))

/* Whether a NO_OPERATION parameter is one to deliver. */
static inline int nv2a_gr_notify_wanted(uint32_t param, uint32_t mask)
{
    uint32_t m = param & 0x1Fu;
    return param != 0 && m != 1u && ((mask >> m) & 1u);
}

/* Latch the trap and raise the line's status bits. The interrupt itself is
 * the timer thread's to deliver (kernel_bridge.c). */
static inline void nv2a_gr_notify_raise(volatile void *regs, uint32_t subch,
                                        uint32_t param)
{
    NV2A_GR_REG(regs, NV2A_GR_PGRAPH_TRAPPED_ADDR) =
        (subch << 16) | NV2A_GR_NOP_METHOD;
    NV2A_GR_REG(regs, NV2A_GR_PGRAPH_TRAPPED_DATA) = param;
    NV2A_GR_REG(regs, NV2A_GR_PGRAPH_NSOURCE) = NV2A_GR_NSOURCE_NOTIFICATION;
    NV2A_GR_REG(regs, NV2A_GR_PGRAPH_FIFO) = NV2A_GR_FIFO_RAISED;
    /* The trap before the status bits: the handler reads them in that
     * order. Atomic, since the vblank's raise and the guest change other
     * bits of PMC_INTR_0 meanwhile. */
    __atomic_fetch_or((volatile uint32_t *)((volatile char *)regs
                                            + NV2A_GR_PGRAPH_INTR),
                      NV2A_GR_PGRAPH_INTR_ERROR, __ATOMIC_SEQ_CST);
    __atomic_fetch_or((volatile uint32_t *)((volatile char *)regs
                                            + NV2A_GR_PMC_INTR_0),
                      NV2A_GR_PMC_INTR_PGRAPH, __ATOMIC_SEQ_CST);
}

/* The guest's ServiceGrInterrupt has run to its end since the raise. */
static inline int nv2a_gr_notify_serviced(volatile void *regs)
{
    return __atomic_load_n((volatile uint32_t *)((volatile char *)regs
                                                 + NV2A_GR_PGRAPH_FIFO),
                           __ATOMIC_ACQUIRE) == 1u;
}

/* The write-1-to-clear the handler did. Also what a handler pass that
 * re-latched the bits (its read-back write landing after this) needs. */
static inline void nv2a_gr_notify_clear(volatile void *regs)
{
    NV2A_GR_REG(regs, NV2A_GR_PGRAPH_NSOURCE) = 0;
    __atomic_fetch_and((volatile uint32_t *)((volatile char *)regs
                                             + NV2A_GR_PGRAPH_INTR),
                       ~NV2A_GR_PGRAPH_INTR_ERROR, __ATOMIC_SEQ_CST);
    __atomic_fetch_and((volatile uint32_t *)((volatile char *)regs
                                             + NV2A_GR_PMC_INTR_0),
                       ~NV2A_GR_PMC_INTR_PGRAPH, __ATOMIC_SEQ_CST);
}

/* The line is asserted only while PMC_INTR_EN_0 is on: the XDK ISR clears
 * it before queueing its DPC and the DPC sets it again last. Off, the bit
 * stays latched and the DPC's own loop, or the next delivery, finds it. */
static inline int nv2a_gr_notify_deliverable(volatile void *regs)
{
    return __atomic_load_n((volatile uint32_t *)((volatile char *)regs
                                                 + NV2A_GR_PMC_INTR_EN_0),
                           __ATOMIC_ACQUIRE) != 0;
}

/* A notify raised and not yet serviced, and the ones queued behind it.
 * `pending` is any thread's to read and take down (nv2a_gr_poll); the
 * queue is the walker's thread's alone, which posts to it and raises from
 * it (the two counts are a report's to read and reset). The swap never
 * comes here, so an entry is an event method: running it late wakes its
 * waiter late, dropping it never wakes it. */
#define NV2A_GR_QUEUE_LEN 16u

typedef struct Nv2aGrState {
    int pending;
    int marked;                         /* the ack thread's, per raise */
    uint32_t head, tail;
    uint32_t subch[NV2A_GR_QUEUE_LEN], param[NV2A_GR_QUEUE_LEN];
    long queued, dropped;
} Nv2aGrState;

enum { NV2A_GR_DROPPED = -1, NV2A_GR_QUEUED = 0, NV2A_GR_RAISED = 1 };

static inline int nv2a_gr_pending(Nv2aGrState *s)
{
    return __atomic_load_n(&s->pending, __ATOMIC_ACQUIRE) != 0;
}

/* The ack thread's test: serviced, seen as an edge. A pass reading 0 is in
 * flight: mark it. The handler's next write after the mark is its exit's 1
 * (and then the next pass's 0), so a 0 or 1 once marked is an exit, even
 * when no sample ever lands between passes. The compare-exchange leaves an
 * exit that landed after the read alone. One thread only: the mark is
 * reset by the raise, on the same thread. */
static inline int nv2a_gr_serviced_watch(Nv2aGrState *s, volatile void *regs)
{
    uint32_t *f = (uint32_t *)((char *)regs + NV2A_GR_PGRAPH_FIFO);
    uint32_t v = __atomic_load_n(f, __ATOMIC_ACQUIRE), zero = 0;

    if (v == 1u || (v == 0u && s->marked))
        return 1;
    if (v == 0u && __atomic_compare_exchange_n(f, &zero, NV2A_GR_FIFO_MARK, 0,
                                               __ATOMIC_SEQ_CST,
                                               __ATOMIC_SEQ_CST))
        s->marked = 1;
    return 0;
}

/* Take a serviced notify down: 1 when this call did. Any thread with
 * watch 0 (the plain level); the ack thread with watch 1, and it must call
 * it for as long as one is pending (see above).
 *
 * pending goes 1, then NV2A_GR_CLEARING, and only after the clear to 0:
 * the next raise waits for 0, so it cannot land between this call taking
 * the line and clearing the bits, and have its own bits cleared. That
 * leaves it pending with nothing raised, and the event's waiter asleep. */
#define NV2A_GR_CLEARING 2

static inline int nv2a_gr_take(Nv2aGrState *s)
{
    int one = 1;
    return __atomic_compare_exchange_n(&s->pending, &one, NV2A_GR_CLEARING,
                                       0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void nv2a_gr_settle(Nv2aGrState *s, volatile void *regs)
{
    nv2a_gr_notify_clear(regs);
    __atomic_store_n(&s->pending, 0, __ATOMIC_SEQ_CST);
}

static inline int nv2a_gr_poll(Nv2aGrState *s, volatile void *regs, int watch)
{
    if (__atomic_load_n(&s->pending, __ATOMIC_ACQUIRE) != 1)
        return 0;
    if (!(watch ? nv2a_gr_serviced_watch(s, regs)
                : nv2a_gr_notify_serviced(regs)))
        return 0;
    if (!nv2a_gr_take(s))
        return 0;
    nv2a_gr_settle(s, regs);
    return 1;
}

static inline void nv2a_gr_raise_now(Nv2aGrState *s, volatile void *regs,
                                     uint32_t subch, uint32_t param)
{
    /* The raise before pending: PGRAPH_FIFO still reads the last service's
     * 1 until the raise overwrites it. */
    s->marked = 0;
    nv2a_gr_notify_raise(regs, subch, param);
    __atomic_store_n(&s->pending, 1, __ATOMIC_SEQ_CST);
}

/* Raise it, or queue it behind the one pending (or those already queued,
 * so they are raised in order). */
static inline int nv2a_gr_post(Nv2aGrState *s, volatile void *regs,
                               uint32_t subch, uint32_t param)
{
    if (nv2a_gr_pending(s) || s->head != s->tail) {
        if (s->tail - s->head >= NV2A_GR_QUEUE_LEN) {
            __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED);
            return NV2A_GR_DROPPED;
        }
        s->subch[s->tail % NV2A_GR_QUEUE_LEN] = subch;
        s->param[s->tail % NV2A_GR_QUEUE_LEN] = param;
        s->tail++;
        __atomic_fetch_add(&s->queued, 1, __ATOMIC_RELAXED);
        return NV2A_GR_QUEUED;
    }
    nv2a_gr_raise_now(s, regs, subch, param);
    return NV2A_GR_RAISED;
}

/* The line is free and something is queued: the walker's thread raises it
 * with nv2a_gr_raise_next. */
static inline int nv2a_gr_due(Nv2aGrState *s)
{
    return s->head != s->tail && !nv2a_gr_pending(s);
}

static inline uint32_t nv2a_gr_raise_next(Nv2aGrState *s, volatile void *regs)
{
    uint32_t i = s->head++ % NV2A_GR_QUEUE_LEN;
    nv2a_gr_raise_now(s, regs, s->subch[i], s->param[i]);
    return s->param[i];
}

/* The run-ahead's ack rule, at a checkpoint of a walk.
 *
 * The KickOff sets the flush bit, spins until it clears and only then
 * stores PUT, so every ack the walk gives lets the title submit one more
 * segment past the end of the walk in progress, and a set bit is a title
 * waiting to submit the next. depth caps how many it may submit before the
 * walker starts on them: 1 lets it fill one segment while the walker draws
 * this one. A title that kicks a few dozen times a frame hides only a
 * segment's worth at 1; a larger depth leaves D3D's own MakeSpace (GET)
 * and BlockOnTime (the fence, the notify) as the bound, as on the console.
 * Never while a flip is held: the hold is how the title feels FLIP_STALL. */
static inline int nv2a_run_ahead_may_ack(int kick_set, int flip_held,
                                         uint32_t acks_this_walk,
                                         uint32_t depth)
{
    return kick_set && !flip_held && acks_this_walk < depth;
}

/* The run-ahead checkpoint's defer rule (RECOMP_DEBUG=occ_ckpt_defer),
 * in two steps so the backend is asked only when it can matter.
 *
 * A checkpoint settles the walk's visibility reports before it moves GET or
 * acks, as the walk end does, and that waits out the GPU's latency mid-walk.
 * With the rule on, a checkpoint that owes reports asks the backend whether
 * any is still on the GPU; one that is holds the ack (and GET at the oldest
 * owed report) until a later checkpoint, the notify raise, a semaphore
 * release, the walk end or the flip settles it. A notify due raises now and
 * its raise settles everything, so it never defers. */
static inline int nv2a_ckpt_defer_asks(int enabled, int report_owed,
                                       int notify_due)
{
    return enabled && report_owed && !notify_due;
}

static inline int nv2a_ckpt_defers(int enabled, int report_owed,
                                   int notify_due, int pending)
{
    return nv2a_ckpt_defer_asks(enabled, report_owed, notify_due)
        && pending > 0;
}

/* The GET a deferring checkpoint at va stores: the walk's place before its
 * oldest owed report (owed, 0 for none), or 0 to leave GET where it is
 * (last_get, the GET last stored). GET only moves forward: never past va
 * (an owed report after a ring wrap the walk has not reached), and never
 * back to or behind last_get (a latch from before it, or a wrap between
 * them, where address order is not ring order: holding is the safe side).
 * All three in GET's form (va & 0x0FFFFFFF). */
static inline uint32_t nv2a_ckpt_held_get(uint32_t owed, uint32_t va,
                                          uint32_t last_get)
{
    if (!owed || owed > va || owed <= last_get)
        return 0;
    return owed;
}

/* The depth when nothing sets one: deep enough that D3D's own bounds
 * (MakeSpace on GET, BlockOnTime on the fence and its notify) hold the
 * title, as on the console, where the flush is acked at once. BLiNX 1's
 * stage 1-3 on the Deck: 26-28 fps at the old one-segment lockstep, 29-30
 * at depth 1, 30.0 at 64. */
#define NV2A_RUN_AHEAD_DEFAULT_DEPTH 64u
#define NV2A_RUN_AHEAD_MAX_DEPTH     1024u

/* The run-ahead depth for RECOMP_DEBUG=run_ahead's value (NULL when
 * neither it nor a game default is set), 0 for off.
 *   - unset or empty: NV2A_RUN_AHEAD_DEFAULT_DEPTH, except in the
 *     deterministic mode, whose walker passes are granted by the guest ring
 *     and whose runs were recorded with the lockstep: off there unless asked;
 *   - "0" or "off": off (a game that breaks under it says
 *     D(RUN_AHEAD, "0") in RECOMP_ENV_GAME_DEFAULTS);
 *   - N > 1: N, at most NV2A_RUN_AHEAD_MAX_DEPTH; any other value 1. */
static inline uint32_t nv2a_run_ahead_depth(const char *e, int deterministic)
{
    unsigned long n;

    if (!e || !*e)
        return deterministic ? 0u : NV2A_RUN_AHEAD_DEFAULT_DEPTH;
    if (!strcmp(e, "0") || !strcmp(e, "off"))
        return 0u;
    n = strtoul(e, NULL, 10);
    if (n <= 1)
        return 1u;
    return n > NV2A_RUN_AHEAD_MAX_DEPTH ? NV2A_RUN_AHEAD_MAX_DEPTH : (uint32_t)n;
}

#endif /* NV2A_GR_NOTIFY_H */
