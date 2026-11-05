/* RECOMP_DEBUG=fb_dump_prog: the pre-trigger ring and the trigger's range
 * (see the header). */
#include "nv2a_fb_ring.h"

#include <stdlib.h>
#include <string.h>

int nv2a_fb_ring_init(struct nv2a_fb_ring *r, uint32_t size)
{
    memset(r, 0, sizeof *r);
    if (size > NV2A_FB_RING_MAX)
        return -1;
    if (size && !(r->slot = (struct nv2a_fb_frame *)calloc(size, sizeof *r->slot)))
        return -1;
    r->size = size;
    return 0;
}

int nv2a_fb_ring_push(struct nv2a_fb_ring *r, uint32_t flip, const uint8_t *src,
                      uint32_t pitch, uint32_t w, uint32_t h, uint32_t bpp)
{
    struct nv2a_fb_frame *s;
    size_t n = (size_t)pitch * h;

    if (!r->size)
        return 0;
    s = &r->slot[r->next];
    if (n > s->cap) {
        uint8_t *grown = (uint8_t *)realloc(s->px, n);
        if (!grown)
            return -1;
        s->px = grown;
        s->cap = n;
    }
    memcpy(s->px, src, n);
    s->flip = flip;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->bpp = bpp;
    r->next = (r->next + 1) % r->size;
    if (r->count < r->size)
        r->count++;
    return 0;
}

const struct nv2a_fb_frame *nv2a_fb_ring_get(const struct nv2a_fb_ring *r,
                                             uint32_t i)
{
    /* Full, the oldest is the slot about to be overwritten; filling, slot 0. */
    uint32_t base = r->count < r->size ? 0 : r->next;
    return &r->slot[(base + i) % r->size];
}

void nv2a_fb_ring_free(struct nv2a_fb_ring *r)
{
    uint32_t i;

    for (i = 0; i < r->size; i++)
        free(r->slot[i].px);
    free(r->slot);
    memset(r, 0, sizeof *r);
}

void nv2a_fb_trigger_range(uint32_t k, uint32_t before, uint32_t after,
                           uint32_t *first, uint32_t *lo, uint32_t *hi)
{
    *first = before >= k ? 1 : k - before + 1;
    *lo = k + 1;
    *hi = after > UINT32_MAX - k ? UINT32_MAX : k + after;
    if (k == UINT32_MAX) {      /* no flip after it to dump */
        *lo = 1;
        *hi = 0;
    }
}
