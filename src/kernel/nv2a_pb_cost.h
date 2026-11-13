/* Per-batch cost by stage (RECOMP_TRACE=pb_cost, the [PB-COST] line).
 *
 * The [PB-PERF] line splits the walk into decode and draw; this splits the
 * draw into the stages of a backend's on_draw, so a change to one of them
 * is measured in µs per batch rather than guessed from a sampler. A stage
 * mark is one cycle-counter read (rdtsc, cntvct_el0) where the host has one:
 * the walker sees ~10k batches a frame, and two clock_gettime or
 * RtlGetSystemTimePrecise calls per stage would be the cost it measures.
 * The counts are scaled to ns at each flip against the walker's clock.
 * Off, each mark is one load and a branch. */
#ifndef NV2A_PB_COST_H
#define NV2A_PB_COST_H

#include <stdint.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

enum nv2a_cost_stage {
    NV2A_COST_PRE,      /* draw_primitive before on_draw: attributes, notes */
    NV2A_COST_SETUP,    /* surface, vertex shader, ao trigger, indices, raster state */
    NV2A_COST_AO,       /* render.ao depth samples */
    NV2A_COST_STREAMS,  /* vertex streams into the ring */
    NV2A_COST_IB,       /* index buffer into the ring */
    NV2A_COST_ASPECT,   /* hor+ / snap_2d 2D test */
    NV2A_COST_CONSTS,   /* program constants upload */
    NV2A_COST_BIND,     /* viewport, OM, IA and VS state */
    NV2A_COST_PX_STAGES,  /* setup_pixel: the stages' textures and samplers */
    NV2A_COST_PX_ASPECT,  /* setup_pixel: hor+ placement, snap_2d, target reads */
    NV2A_COST_PX_BIND,    /* setup_pixel: pixel constants, views, samplers */
    NV2A_COST_PX_SHADER,  /* setup_pixel: the combiner key and shader */
    NV2A_COST_PIXEL,    /* the pixel shader and raster state binds */
    NV2A_COST_PLACE,    /* flat layers, hud lift, viewport, dump and mag hooks */
    NV2A_COST_DRAW,     /* the API draw call */
    /* The decode side, each timed on its own rather than chained: a method
     * includes whatever it triggers (a merged batch's draw, a flip). */
    NV2A_COST_D_EXEC,   /* nv2a_pb_exec_method, inclusive, from the scan */
    NV2A_COST_D_CKPT,   /* run-ahead checkpoints, inclusive */
    NV2A_COST_D_WALK,   /* walk begin and end, inclusive */
    NV2A_COST_D_CLEAR,  /* the backend's clears */
    NV2A_COST_D_POLL,   /* the backend's report polls */
    NV2A_COST_D_REPORT, /* the backend's report requests */
    NV2A_COST_D_BLIT,   /* the backend's 2D blits */
    NV2A_COST_D_WAKE,   /* a semaphore release's wake of the fence waits */
    /* FLIP_STALL, chained: up to the present, the present, the pack and
     * flip hooks, the frame stats, the dump checks, the flip log (with this
     * line) and the rest (the hold) */
    NV2A_COST_F_HEAD,
    NV2A_COST_F_PRESENT,
    NV2A_COST_F_HOOKS,
    NV2A_COST_F_STATS,
    NV2A_COST_F_DUMPS,
    NV2A_COST_F_LOG,
    NV2A_COST_F_TAIL,
    NV2A_COST_N
};
#define NV2A_COST_DRAW_N (NV2A_COST_DRAW + 1)

extern int nv2a_pb_cost_on;
extern uint64_t nv2a_pb_cost_ticks[NV2A_COST_N];

static inline uint64_t nv2a_pb_cost_tick(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t t;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#else
    extern uint64_t nv2a_pb_now_ns(void);
    return nv2a_pb_now_ns();
#endif
}

/* The start of a stage chain: 0 when the trace is off, so the marks after
 * it skip the counter as well. */
static inline uint64_t nv2a_pb_cost_start(void)
{
    return nv2a_pb_cost_on ? nv2a_pb_cost_tick() : 0;
}

/* Charge the time since *t to `stage` and restart the clock there. */
static inline void nv2a_pb_cost_mark(uint64_t *t, enum nv2a_cost_stage stage)
{
    if (nv2a_pb_cost_on) {
        uint64_t now = nv2a_pb_cost_tick();
        nv2a_pb_cost_ticks[stage] += now - *t;
        *t = now;
    }
}

/* Charge the time since t (from nv2a_pb_cost_start) to `stage`, for a
 * stage timed on its own. t is 0 when the trace was off at the start,
 * including the walker's first method, before nv2a_pb_cost_init. */
static inline void nv2a_pb_cost_add(uint64_t t, enum nv2a_cost_stage stage)
{
    if (t)
        nv2a_pb_cost_ticks[stage] += nv2a_pb_cost_tick() - t;
}

/* nv2a_pb_exec_method's inclusive time by Kelvin method, for the
 * [PB-COST-M] table: which methods the decode time goes to. */
void nv2a_pb_cost_method(uint32_t subch, uint32_t method, uint64_t t);

/* Reads the switch once; the walker calls it at its first method. */
void nv2a_pb_cost_init(void);
/* The [PB-COST] line for one flip, `batches` and `draws` as [PB-PERF] counts
 * them, then the stages are cleared. settle_ns is [PB-PERF]'s settle, the
 * occlusion settle's wait in the walk: printed last as d_settle, the part of
 * d_poll (and so of d_walk, d_ckpt or d_exec) spent waiting on the GPU. */
void nv2a_pb_cost_flip(uint32_t flip, uint32_t batches, uint32_t draws,
                       uint64_t settle_ns);

#endif
