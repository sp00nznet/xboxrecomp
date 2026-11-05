/*
 * nv2a_overlay_ref.c - the overlay's CPU reference rasteriser (F-2): a
 * draw list into a premultiplied RGBA8 picture, as the GPU presenters draw
 * it. The ctests check the overlay against it; it is not in the runtime.
 */
#include "nv2a_overlay_int.h"

#include <math.h>

static void unpack(uint32_t v, float c[4])
{
    const unsigned char *b = (const unsigned char *)&v;
    c[0] = b[0] / 255.0f;
    c[1] = b[1] / 255.0f;
    c[2] = b[2] / 255.0f;
    c[3] = b[3] / 255.0f;
}

static void texel(const struct nv2a_ovl_atlas *a, int x, int y, float c[4])
{
    x = x < 0 ? 0 : x >= (int)a->w ? (int)a->w - 1 : x;
    y = y < 0 ? 0 : y >= (int)a->h ? (int)a->h - 1 : y;
    unpack(a->px[(size_t)y * a->w + (size_t)x], c);
}

/* Bilinear with clamp to edge, texel centres at +0.5, as the presenters'
 * samplers. */
static void sample(const struct nv2a_ovl_atlas *a, float u, float v, float c[4])
{
    float x = u * (float)a->w - 0.5f, y = v * (float)a->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), k;
    float fx = x - (float)x0, fy = y - (float)y0, t[4][4];
    texel(a, x0, y0, t[0]);
    texel(a, x0 + 1, y0, t[1]);
    texel(a, x0, y0 + 1, t[2]);
    texel(a, x0 + 1, y0 + 1, t[3]);
    for (k = 0; k < 4; k++)
        c[k] = (t[0][k] * (1 - fx) + t[1][k] * fx) * (1 - fy)
             + (t[2][k] * (1 - fx) + t[3][k] * fx) * fy;
}

/* Edges in 24.8 fixed point with pixel centres at +0.5 and the top-left
 * fill rule, so a pixel on an edge two triangles share is drawn once, as
 * the GPUs do. */
#define SUB 256
static int64_t edge(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t px, int64_t py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* With the triangle wound so its area is positive (y down), a top edge
 * runs right along a row and a left edge runs up. */
static int64_t bias(int64_t ax, int64_t ay, int64_t bx, int64_t by)
{
    return (ay == by && bx > ax) || by < ay ? 0 : -1;
}

static int64_t fx(float v) { return (int64_t)llroundf(v * SUB); }

void nv2a_ovl_ref_draw(uint32_t *dst, int w, int h, const struct nv2a_ovl_list *l,
                       const struct nv2a_ovl_atlas *a)
{
    float sx = (float)w / (float)l->out_w, sy = (float)h / (float)l->out_h;
    uint32_t ci, i;
    for (ci = 0; ci < l->ncmd; ci++) {
        const struct nv2a_ovl_cmd *c = &l->cmd[ci];
        int cx0 = (int)floorf(c->clip[0] * sx + 0.5f), cy0 = (int)floorf(c->clip[1] * sy + 0.5f);
        int cx1 = (int)floorf((c->clip[0] + c->clip[2]) * sx + 0.5f);
        int cy1 = (int)floorf((c->clip[1] + c->clip[3]) * sy + 0.5f);
        if (c->tex)
            continue;   /* image slots: not in the ctests yet */
        for (i = c->first; i + 3 <= c->first + c->count && i + 3 <= l->nidx; i += 3) {
            const struct nv2a_ovl_vtx *v0 = &l->vtx[l->idx[i]], *v1 = &l->vtx[l->idx[i + 1]],
                                      *v2 = &l->vtx[l->idx[i + 2]];
            int64_t x0 = fx(v0->x * sx), y0 = fx(v0->y * sy), x1 = fx(v1->x * sx);
            int64_t y1 = fx(v1->y * sy), x2 = fx(v2->x * sx), y2 = fx(v2->y * sy);
            int64_t area = edge(x0, y0, x1, y1, x2, y2), b0, b1, b2;
            float c0[4], c1[4], c2[4];
            int bx0, by0, bx1, by1, px, py;
            if (area == 0)
                continue;
            if (area < 0) {
                const struct nv2a_ovl_vtx *t = v1;
                int64_t tx = x1, ty = y1;
                v1 = v2; x1 = x2; y1 = y2;
                v2 = t; x2 = tx; y2 = ty;
                area = -area;
            }
            b0 = bias(x1, y1, x2, y2);
            b1 = bias(x2, y2, x0, y0);
            b2 = bias(x0, y0, x1, y1);
            unpack(v0->rgba, c0);
            unpack(v1->rgba, c1);
            unpack(v2->rgba, c2);
            bx0 = (int)((x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2)) / SUB) - 1;
            by0 = (int)((y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2)) / SUB) - 1;
            bx1 = (int)((x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) / SUB) + 2;
            by1 = (int)((y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) / SUB) + 2;
            if (bx0 < cx0) bx0 = cx0;
            if (by0 < cy0) by0 = cy0;
            if (bx1 > cx1) bx1 = cx1;
            if (by1 > cy1) by1 = cy1;
            if (bx0 < 0) bx0 = 0;
            if (by0 < 0) by0 = 0;
            if (bx1 > w) bx1 = w;
            if (by1 > h) by1 = h;
            for (py = by0; py < by1; py++)
                for (px = bx0; px < bx1; px++) {
                    int64_t qx = (int64_t)px * SUB + SUB / 2, qy = (int64_t)py * SUB + SUB / 2;
                    int64_t e0 = edge(x1, y1, x2, y2, qx, qy), e1 = edge(x2, y2, x0, y0, qx, qy);
                    int64_t e2 = edge(x0, y0, x1, y1, qx, qy);
                    float w0, w1, w2, t[4], sc[4], d[4];
                    unsigned char *o;
                    int k;
                    if (e0 + b0 < 0 || e1 + b1 < 0 || e2 + b2 < 0)
                        continue;
                    w0 = (float)e0 / (float)area;
                    w1 = (float)e1 / (float)area;
                    w2 = (float)e2 / (float)area;
                    sample(a, w0 * v0->u + w1 * v1->u + w2 * v2->u,
                           w0 * v0->v + w1 * v1->v + w2 * v2->v, t);
                    o = (unsigned char *)&dst[(size_t)py * (size_t)w + (size_t)px];
                    for (k = 0; k < 4; k++) {
                        sc[k] = (w0 * c0[k] + w1 * c1[k] + w2 * c2[k]) * t[k];
                        d[k] = o[k] / 255.0f;
                    }
                    for (k = 0; k < 4; k++) {
                        float v = sc[k] + d[k] * (1.0f - sc[3]);
                        o[k] = (unsigned char)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f));
                    }
                }
        }
    }
}
