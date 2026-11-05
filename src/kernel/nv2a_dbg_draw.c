/*
 * nv2a_dbg_draw.c - the mods' 3D debug draw lists (nv2a_dbg_draw.h).
 *
 * Three lists: the guest fills `back`, the flip draws `front`, and `mid`
 * is the one between them. publish swaps back with mid and sets the fresh
 * bit; take swaps mid with front only when the bit is set. Each side
 * touches only its own list, so the growth (realloc) is the producer's
 * alone and the flip never waits.
 */
#include "nv2a_dbg_draw.h"

#include <stdlib.h>
#include <string.h>

#define FRESH 4

static struct nv2a_dbg_list s_list[3];
static int s_back = 0, s_front = 2;
static int s_mid = 1;                   /* atomic: index | FRESH */
static int s_any;                       /* atomic: 1 after the first publish, 0 hidden */
static uint32_t s_serial;               /* producer's */

void nv2a_dbg_transform(const float view[16], const float proj[16], const float p[3], int pull,
                        float out[4])
{
    float v[4];
    int j;

    for (j = 0; j < 4; j++)
        v[j] = p[0] * view[0 * 4 + j] + p[1] * view[1 * 4 + j] + p[2] * view[2 * 4 + j] +
               view[3 * 4 + j];
    /* The eye is view space's origin: scaling the point toward it moves it
     * along its own ray, so it covers the same pixel and only its depth
     * changes. */
    if (pull)
        for (j = 0; j < 3; j++)
            v[j] *= 1.0f - NV2A_DBG_PULL_K;
    for (j = 0; j < 4; j++)
        out[j] = v[0] * proj[0 * 4 + j] + v[1] * proj[1 * 4 + j] + v[2] * proj[2 * 4 + j] +
                 v[3] * proj[3 * 4 + j];
}

int nv2a_dbg_begin(const float view[16], const float proj[16])
{
    struct nv2a_dbg_list *l = &s_list[s_back];
    int i;

    if (l->open)
        return 0;
    for (i = 0; i < NV2A_DBG_SEGS; i++)
        l->n[i] = 0;
    l->total = 0;
    l->dropped = 0;
    memcpy(l->view, view, sizeof l->view);
    memcpy(l->proj, proj, sizeof l->proj);
    l->open = 1;
    return 1;
}

static int seg_of(int lines, uint32_t flags)
{
    if (flags & NV2A_DBG_XRAY)
        return lines ? NV2A_DBG_LINE_XRAY : NV2A_DBG_TRI_XRAY;
    if (lines)
        return NV2A_DBG_LINE;
    return (flags & NV2A_DBG_OPAQUE) ? NV2A_DBG_TRI_OPAQUE : NV2A_DBG_TRI_BLEND;
}

static int grow(struct nv2a_dbg_list *l, int s, uint32_t need)
{
    uint32_t c;
    struct nv2a_dbg_vtx *nv;

    if (need <= l->cap[s])
        return 1;
    c = l->cap[s] ? l->cap[s] : 1024u;
    while (c < need)
        c *= 2u;
    nv = (struct nv2a_dbg_vtx *)realloc(l->v[s], (size_t)c * sizeof *nv);
    if (!nv)
        return 0;
    l->v[s] = nv;
    l->cap[s] = c;
    return 1;
}

int nv2a_dbg_add(int lines, const void *verts, uint32_t stride, uint32_t n, uint32_t flags)
{
    struct nv2a_dbg_list *l = &s_list[s_back];
    const uint8_t *src = (const uint8_t *)verts;
    uint32_t per = lines ? 2u : 3u, room, i, k;
    int s;

    if (!l->open || !verts || !n || stride < 16u || (flags & ~NV2A_DBG_FLAGS))
        return 0;
    room = (NV2A_DBG_MAX_VTX - l->total) / per;
    if (n > room) {
        l->dropped += n - room;
        n = room;
    }
    if (!n)
        return 0;
    s = seg_of(lines, flags);
    if (!grow(l, s, l->n[s] + n * per)) {
        l->dropped += n;
        return 0;
    }
    for (i = 0; i < n * per; i++) {
        struct nv2a_dbg_vtx *o = &l->v[s][l->n[s] + i];
        float p[3], c[4];
        uint32_t argb;

        memcpy(p, src + (size_t)i * stride, sizeof p);
        memcpy(&argb, src + (size_t)i * stride + 12, sizeof argb);
        nv2a_dbg_transform(l->view, l->proj, p, (flags & NV2A_DBG_PULL) != 0, c);
        o->x = c[0];
        o->y = c[1];
        o->z = c[2];
        o->w = c[3];
        for (k = 0; k < 3; k++)
            o->rgba[k] = (uint8_t)(argb >> (16 - 8 * k));
        o->rgba[3] = (uint8_t)(argb >> 24);
    }
    l->n[s] += n * per;
    l->total += n * per;
    return (int)n;
}

int nv2a_dbg_publish(void)
{
    struct nv2a_dbg_list *l = &s_list[s_back];
    int old;

    if (!l->open)
        return -1;
    l->open = 0;
    l->serial = ++s_serial;
    old = __atomic_exchange_n(&s_mid, s_back | FRESH, __ATOMIC_ACQ_REL);
    s_back = old & 3;
    __atomic_store_n(&s_any, 1, __ATOMIC_RELEASE);
    return (int)l->total;
}

void nv2a_dbg_cancel(void)
{
    s_list[s_back].open = 0;
}

const struct nv2a_dbg_list *nv2a_dbg_take(void)
{
    if (!__atomic_load_n(&s_any, __ATOMIC_ACQUIRE))
        return NULL;
    if (__atomic_load_n(&s_mid, __ATOMIC_ACQUIRE) & FRESH)
        s_front = __atomic_exchange_n(&s_mid, s_front, __ATOMIC_ACQ_REL) & 3;
    return &s_list[s_front];
}

void nv2a_dbg_hide(void)
{
    __atomic_store_n(&s_any, 0, __ATOMIC_RELEASE);
}

uint32_t nv2a_dbg_published(void)
{
    return s_serial;
}

void nv2a_dbg_reset(void)
{
    int i, s;

    for (i = 0; i < 3; i++) {
        for (s = 0; s < NV2A_DBG_SEGS; s++)
            free(s_list[i].v[s]);
        memset(&s_list[i], 0, sizeof s_list[i]);
    }
    s_back = 0;
    s_front = 2;
    __atomic_store_n(&s_mid, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_any, 0, __ATOMIC_RELEASE);
    s_serial = 0;
}
