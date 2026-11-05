/* Hor+ (display.aspect): the 2D test of a program batch, apart from
 * nv2a_backend_common.c so the tests that link only the common helpers
 * do not need the vertex interpreter. */
#include "nv2a_backend_common.h"
#include "nv2a_vsh_cpu.h"
#include <math.h>
#include <string.h>

int nv2a_aspect_due(const struct nv2a_aspect_gate *gate)
{
    return gate->two_d == 1 || gate->n < 16 || (gate->n & 255u) == 0;
}

int nv2a_aspect_scan(struct nv2a_aspect_gate *gate,
                     const uint32_t prog[][4], uint32_t start, float c[][4],
                     const float *vf, unsigned nf4, const int *slot, uint32_t count,
                     float (*xy)[2], float *lo, float *hi)
{
    float in[NV2A_VSH_INPUTS][4], w0 = 1.0f, z0 = 0.0f;
    Nv2aVshOut out;
    uint32_t step = !xy && count > 32 ? count / 32 : 1, j;
    unsigned k;
    int check = nv2a_aspect_due(gate);

    gate->n++;
    if (!check || !count)
        return 0;
    for (j = 0; j < count; j += step) {
        float x;
        memset(in, 0, sizeof in);
        for (k = 0; k < nf4; k++)
            if (slot[k] >= 0 && slot[k] < NV2A_VSH_INPUTS)
                memcpy(in[slot[k]], vf + ((size_t)j * nf4 + k) * 4u, 16);
        nv2a_vsh_run((const uint32_t (*)[4])prog, start, (float (*)[4])c, 0,
                     (const float (*)[4])in, &out);
        if (!out.ok)
            return 0;
        x = out.o[NV2A_VSH_O_POS][0];
        if (xy) {
            xy[j][0] = x;
            xy[j][1] = out.o[NV2A_VSH_O_POS][1];
        }
        if (j == 0 ? !nv2a_aspect_layer_w(out.o[NV2A_VSH_O_POS][3])
                   : out.o[NV2A_VSH_O_POS][3] != w0) {
            if (gate->two_d < 1)
                gate->two_d = 0;
            return 0;
        }
        if (j == 0) {
            w0 = out.o[NV2A_VSH_O_POS][3];
            z0 = out.o[NV2A_VSH_O_POS][2];
            gate->vary_z = 0;
            *lo = *hi = x;
        } else {
            if (out.o[NV2A_VSH_O_POS][2] != z0)
                gate->vary_z = 1;
            if (x < *lo) *lo = x;
            if (x > *hi) *hi = x;
        }
    }
    gate->two_d = 1;
    return 1;
}

int nv2a_aspect_screen_quad(const uint32_t prog[][4], uint32_t start, float c[][4],
                            const float *vf, unsigned nf4, const int *slot, uint32_t count,
                            float (*xy)[2], float *lo, float *hi)
{
    float in[NV2A_VSH_INPUTS][4], w0 = 0.0f;
    Nv2aVshOut out;
    uint32_t j;
    unsigned k;

    if (!count || count > NV2A_ASPECT_QUAD_MAX)
        return 0;
    for (j = 0; j < count; j++) {
        const float *p;
        memset(in, 0, sizeof in);
        for (k = 0; k < nf4; k++)
            if (slot[k] >= 0 && slot[k] < NV2A_VSH_INPUTS)
                memcpy(in[slot[k]], vf + ((size_t)j * nf4 + k) * 4u, 16);
        nv2a_vsh_run((const uint32_t (*)[4])prog, start, (float (*)[4])c, 0,
                     (const float (*)[4])in, &out);
        if (!out.ok)
            return 0;
        p = out.o[NV2A_VSH_O_POS];
        if (j == 0 ? !(p[3] > 0.0f && isfinite(p[3])) : p[3] != w0)
            return 0;
        xy[j][0] = p[0];
        xy[j][1] = p[1];
        if (j == 0) {
            w0 = p[3];
            *lo = *hi = p[0];
        } else {
            if (p[0] < *lo) *lo = p[0];
            if (p[0] > *hi) *hi = p[0];
        }
    }
    return 1;
}

int nv2a_aspect_full_tris(const float (*xy)[2], const uint32_t *idx, uint32_t ntri,
                          uint32_t w)
{
    uint32_t t;

    for (t = 0; t < ntri; t++) {
        float a = xy[idx[3 * t]][0], b = xy[idx[3 * t + 1]][0], c = xy[idx[3 * t + 2]][0];
        if (nv2a_aspect_full_width(fminf(a, fminf(b, c)), fmaxf(a, fmaxf(b, c)), w))
            return 1;
    }
    return 0;
}

int nv2a_aspect_full_tris_in(const float (*xy)[2], const uint32_t *idx, uint32_t ntri,
                             uint32_t w, float lo, float hi)
{
    uint32_t t;

    for (t = 0; t < ntri; t++) {
        float a = xy[idx[3 * t]][0], b = xy[idx[3 * t + 1]][0], c = xy[idx[3 * t + 2]][0];
        float mn = fminf(a, fminf(b, c)), mx = fmaxf(a, fmaxf(b, c));
        if (nv2a_aspect_full_width(mn, mx, w) && (mn >= lo || mx <= hi))
            return 1;
    }
    return 0;
}

int nv2a_aspect_full(float lo_x, float hi_x, const float (*xy)[2], const uint32_t *idx,
                     uint32_t ntri, uint32_t w, uint32_t h, int *wide)
{
    int full, bound;

    if (wide)
        *wide = 0;
    if (!nv2a_aspect_full_width(lo_x, hi_x, w))
        return 0;
    full = !xy || !idx || nv2a_aspect_full_tris(xy, idx, ntri, w);
    if (!full || !nv2a_aspect_hud_on(w, h))
        return full;
    if (xy && idx)
        bound = nv2a_aspect_full_tris_in(xy, idx, ntri, w, -NV2A_HUD_WIDE_BAND,
                                         (float)w + NV2A_HUD_WIDE_BAND);
    else
        /* No triangles to look at: a bound past one edge says nothing of a
         * mixed batch, so the whole batch must sit inside the band. */
        bound = lo_x >= -NV2A_HUD_WIDE_BAND && hi_x <= (float)w + NV2A_HUD_WIDE_BAND;
    if (wide)
        *wide = !bound;
    return bound;
}

/* The finite bounds of xy[0..n), or 0 when n is 0 or a value is not
 * finite (fminf and fmaxf would skip a NaN). */
static int quad_box(const float (*xy)[2], uint32_t n, float *lo_x, float *hi_x, float *lo_y,
                    float *hi_y)
{
    uint32_t j;

    if (!n)
        return 0;
    *lo_x = *hi_x = xy[0][0];
    *lo_y = *hi_y = xy[0][1];
    for (j = 0; j < n; j++) {
        if (!isfinite(xy[j][0]) || !isfinite(xy[j][1]))
            return 0;
        *lo_x = fminf(*lo_x, xy[j][0]);
        *hi_x = fmaxf(*hi_x, xy[j][0]);
        *lo_y = fminf(*lo_y, xy[j][1]);
        *hi_y = fmaxf(*hi_y, xy[j][1]);
    }
    return 1;
}

unsigned nv2a_aspect_third(const float (*xy)[2], uint32_t n, uint32_t h)
{
    float lo_x, hi_x, lo_y, hi_y;

    if (!quad_box(xy, n, &lo_x, &hi_x, &lo_y, &hi_y) || lo_y < -2.0f
        || hi_y > (float)h + 2.0f)
        return 0;
    if (hi_y <= (float)h / 3.0f)
        return NV2A_BAND_TOP;
    if (lo_y >= (float)h * 2.0f / 3.0f)
        return NV2A_BAND_BOT;
    return 0;
}

int nv2a_aspect_band(const float (*xy)[2], uint32_t n, uint32_t w, uint32_t h, unsigned bars)
{
    float lo_x, hi_x, lo_y, hi_y;

    if (!(nv2a_aspect_third(xy, n, h) & bars) || !quad_box(xy, n, &lo_x, &hi_x, &lo_y, &hi_y))
        return 0;
    return lo_x >= -2.0f && hi_x <= (float)w + 2.0f
           && (lo_x <= 2.0f || hi_x >= (float)w - 2.0f);
}

int nv2a_aspect_flat(const uint32_t prog[][4], uint32_t start, float c[][4], const float *vf,
                     unsigned nf4, const int *slot, uint32_t count, float *w)
{
    float in[NV2A_VSH_INPUTS][4], w0 = 0.0f;
    Nv2aVshOut out;
    uint32_t step = nv2a_aspect_flat_step(count), j;
    unsigned k;

    for (j = 0; j < count; j += step) {
        float wj;
        memset(in, 0, sizeof in);
        for (k = 0; k < nf4; k++)
            if (slot[k] >= 0 && slot[k] < NV2A_VSH_INPUTS)
                memcpy(in[slot[k]], vf + ((size_t)j * nf4 + k) * 4u, 16);
        nv2a_vsh_run((const uint32_t (*)[4])prog, start, (float (*)[4])c, 0,
                     (const float (*)[4])in, &out);
        if (!out.ok)
            return 0;
        wj = out.o[NV2A_VSH_O_POS][3];
        /* The first vertex rules out the world's batches at once. */
        if (j == 0 ? !nv2a_aspect_flat_w(wj) : wj != w0)
            return 0;
        w0 = wj;
    }
    if (w)
        *w = w0;
    return count != 0;
}

uint32_t nv2a_aspect_uv_sample(const uint32_t prog[][4], uint32_t start, float c[][4],
                               const float *vf, unsigned nf4, const int *slot, uint32_t count,
                               unsigned stage, float (*xy)[2], float (*uv)[2], uint32_t max)
{
    float in[NV2A_VSH_INPUTS][4];
    Nv2aVshOut out;
    uint32_t step, j, n = 0;
    unsigned k;
    int kp = -1, kt = -1;

    if (!count || !max || stage > 3)
        return 0;
    for (k = 0; k < nf4; k++) {
        if (slot[k] == 0)
            kp = (int)k;
        if (slot[k] == 9 + (int)stage)
            kt = (int)k;
    }
    if (!prog && (kp < 0 || kt < 0))
        return 0;
    step = count > max ? count / max : 1;
    for (j = 0; j < count && n < max; j += step, n++) {
        const float *v = vf + (size_t)j * nf4 * 4u;
        if (!prog) {
            xy[n][0] = v[(size_t)kp * 4u];
            xy[n][1] = v[(size_t)kp * 4u + 1];
            uv[n][0] = v[(size_t)kt * 4u];
            uv[n][1] = v[(size_t)kt * 4u + 1];
            continue;
        }
        memset(in, 0, sizeof in);
        for (k = 0; k < nf4; k++)
            if (slot[k] >= 0 && slot[k] < NV2A_VSH_INPUTS)
                memcpy(in[slot[k]], v + (size_t)k * 4u, 16);
        nv2a_vsh_run((const uint32_t (*)[4])prog, start, (float (*)[4])c, 0,
                     (const float (*)[4])in, &out);
        if (!out.ok)
            return 0;
        xy[n][0] = out.o[NV2A_VSH_O_POS][0];
        xy[n][1] = out.o[NV2A_VSH_O_POS][1];
        uv[n][0] = out.o[NV2A_VSH_O_T0 + stage][0];
        uv[n][1] = out.o[NV2A_VSH_O_T0 + stage][1];
    }
    return n;
}
