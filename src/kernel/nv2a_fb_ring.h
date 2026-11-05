/**
 * RECOMP_DEBUG=fb_dump_prog: the frames before and after a vertex program's
 * first draw.
 *
 * Some titles don't replay a run the same way twice (SSX 3's races finish
 * differently with the same save and pad script), so the flip where a
 * rendering fault starts can't be named in advance for fb_dump_at. The
 * trigger fires at the first draw of a listed program instead. The flips
 * after it are dumped as they come; the flips before it are kept in this ring
 * of presented frames, copied from guest memory, and written when it fires.
 *
 * This is free of the executor's state so tests/nv2a_fb_ring can run it.
 */
#ifndef XBOXRECOMP_NV2A_FB_RING_H
#define XBOXRECOMP_NV2A_FB_RING_H

#include <stddef.h>
#include <stdint.h>

#define NV2A_FB_RING_MAX 240    /* 295 MB at 640x480x4 */

struct nv2a_fb_frame {
    uint32_t flip, w, h, pitch, bpp;
    uint8_t *px;                /* h rows of pitch bytes */
    size_t cap;
};

struct nv2a_fb_ring {
    struct nv2a_fb_frame *slot;
    uint32_t size, next, count;
};

/* size 0 is a valid ring that keeps nothing. Returns 0, or -1 (no memory or
 * size past NV2A_FB_RING_MAX). */
int nv2a_fb_ring_init(struct nv2a_fb_ring *r, uint32_t size);

/* Copy one presented frame in, over the oldest once the ring is full.
 * Returns 0, or -1 if a slot could not grow (the frame is skipped). */
int nv2a_fb_ring_push(struct nv2a_fb_ring *r, uint32_t flip, const uint8_t *src,
                      uint32_t pitch, uint32_t w, uint32_t h, uint32_t bpp);

/* The i-th frame held, 0 the oldest, i < r->count. */
const struct nv2a_fb_frame *nv2a_fb_ring_get(const struct nv2a_fb_ring *r,
                                             uint32_t i);

void nv2a_fb_ring_free(struct nv2a_fb_ring *r);

/* A trigger at a draw with the flip count at k: its frame is presented by
 * flip k+1. The ring's frames are flips first..k (first is 1 at the start of
 * a run), and flips lo..hi are dumped after; lo > hi when after is 0. */
void nv2a_fb_trigger_range(uint32_t k, uint32_t before, uint32_t after,
                           uint32_t *first, uint32_t *lo, uint32_t *hi);

#endif
