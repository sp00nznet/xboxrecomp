/*
 * nv2a_zbuf_cache.h - which CPU-raster depth/stencil buffer a draw uses.
 *
 * The CPU raster keeps one depth/stencil buffer per (zeta offset, extent),
 * the same shape as the D3D11 backend's depth_target. With a single shared
 * buffer, a level's mid-frame 256x256 offscreen pass reset the main depth
 * and the sky painted over the terrain drawn before it.
 *
 * The extent is the clip rectangle's (clip_x + clip_w, clip_y + clip_h), not
 * the surface's: the CPU raster sizes its buffers by the clip extent, where
 * D3D11 uses the render target's size. The zeta format and pitch are not in
 * the key: a surface reused at the same offset and extent with another
 * format keeps its old buffer contents (depth is stored as normalised float
 * either way, so only a game that relies on the reset would notice).
 *
 * Header-only and free of guest state so tests/nv2a_zbuf can drive it.
 * Only the selection lives here; the caller owns the buffers.
 */
#ifndef NV2A_ZBUF_CACHE_H
#define NV2A_ZBUF_CACHE_H

#include <stdint.h>

/* The slot array's bound; the caller picks how many are in use
 * (RECOMP_DEBUG=zbuf_slots, default 8). A slot costs its key until a
 * buffer is allocated for it. */
#define ZB_MAX 16

typedef struct {
    uint32_t zeta, w, h;
    int      used;
    uint64_t last_use;
} ZbSlot;

typedef struct {
    ZbSlot   slot[ZB_MAX];
    uint64_t tick;
    uint64_t evictions;
} ZbCache;

typedef struct {
    int index;      /* slot to use */
    int fresh;      /* 1: new key in this slot, the caller (re)initialises it */
    int evicted;    /* 1: a live key was dropped to make room (LRU) */
} ZbPick;

/* n: slots in use, 1..ZB_MAX (tests use fewer to force evictions). */
static inline ZbPick zb_select_n(ZbCache *c, int n, uint32_t zeta, uint32_t w,
                                 uint32_t h)
{
    ZbPick p = { -1, 0, 0 };
    int k, lru = 0, empty = -1;

    if (n > ZB_MAX) n = ZB_MAX;
    c->tick++;
    for (k = 0; k < n; k++) {
        ZbSlot *s = &c->slot[k];
        if (s->used && s->zeta == zeta && s->w == w && s->h == h) {
            p.index = k;
            break;
        }
        if (!s->used) {
            if (empty < 0) empty = k;
        } else if (!c->slot[lru].used || s->last_use < c->slot[lru].last_use) {
            lru = k;
        }
    }
    if (p.index < 0) {
        p.fresh = 1;
        if (empty >= 0) {
            p.index = empty;
        } else {
            p.index = lru;
            p.evicted = 1;
            c->evictions++;
        }
        c->slot[p.index].used = 1;
        c->slot[p.index].zeta = zeta;
        c->slot[p.index].w = w;
        c->slot[p.index].h = h;
    }
    c->slot[p.index].last_use = c->tick;
    return p;
}

static inline ZbPick zb_select(ZbCache *c, uint32_t zeta, uint32_t w, uint32_t h)
{
    return zb_select_n(c, ZB_MAX, zeta, w, h);
}

#endif
