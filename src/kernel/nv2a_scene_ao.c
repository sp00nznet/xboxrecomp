/*
 * nv2a_scene_ao.c - the scene stage's trigger, depth fit and constants
 * (nv2a_scene_ao.h). No GPU here: both backends call it, the test alone.
 */
#include "nv2a_scene_ao.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct nv2a_ao_cfg s_cfg = {
    0, 1.5f, 20.0f, 0.6f, 1.2f, 16, 8, 0.6f, 0,
};

void nv2a_ao_cfg_default(struct nv2a_ao_cfg *c)
{
    c->on = 0;
    c->strength = 1.5f;
    c->radius = 20.0f;
    /* Shares of the scene depth (the 90th percentile of the sampled w):
     * the far landscape gets no occlusion, where depth from a 24-bit
     * buffer is too coarse and the halo it would draw reads as dirt. */
    c->fade_lo = 0.6f;
    c->fade_hi = 1.2f;
    /* The post-effects probe: a real 3D pass took dozens of depth-writing draws,
     * a lone 3D HUD widget or a menu backdrop a handful. */
    c->arm_draws = 16;
    c->rearm_draws = 8;
    /* JSRF's menus fitted at 0.23, every 3D scene at 0.9 or more. */
    c->min_conf = 0.6f;
    c->view = 0;
}

void nv2a_ao_cfg_set(const struct nv2a_ao_cfg *c) { s_cfg = *c; }
const struct nv2a_ao_cfg *nv2a_ao_cfg(void) { return &s_cfg; }

/* A fixed LCG: the fit and the reservoir are the same for the same frame,
 * so a still scene keeps its model and a test can pin it. */
static uint32_t rng_next(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

#define AO_FIT_ITERS 96
/* In d units: 3e-4 of the 24-bit range, about 5000 steps. The probe's
 * RANSAC (zfit2.py) used the same and split JSRF's two projections. */
#define AO_FIT_TOL 3e-4f

int nv2a_ao_fit_z(const float *d, const float *iw, unsigned n, float min_conf,
                  struct nv2a_ao_fit *fit)
{
    uint32_t seed = 0x9e3779b9u;
    unsigned best = 0, it, i;
    float ba = 0, bb = 0;
    fit->ok = 0;
    fit->conf = 0;
    if (n < 2)
        return 0;
    for (it = 0; it < AO_FIT_ITERS; it++) {
        unsigned i0 = rng_next(&seed) % n, i1 = rng_next(&seed) % n, cnt = 0;
        float diw = iw[i1] - iw[i0], a, b;
        if (i0 == i1 || fabsf(diw) < 1e-9f + 1e-4f * fabsf(iw[i0]))
            continue;
        b = (d[i1] - d[i0]) / diw;
        a = d[i0] - b * iw[i0];
        for (i = 0; i < n; i++)
            if (fabsf(d[i] - (a + b * iw[i])) < AO_FIT_TOL)
                cnt++;
        if (cnt > best) {
            best = cnt;
            ba = a;
            bb = b;
        }
    }
    if (best >= 2) {
        /* Least squares on the inliers: the two-point model carries the
         * rounding of its two samples. */
        double sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0;
        for (i = 0; i < n; i++)
            if (fabsf(d[i] - (ba + bb * iw[i])) < AO_FIT_TOL) {
                sx += iw[i];
                sy += d[i];
                sxx += (double)iw[i] * iw[i];
                sxy += (double)iw[i] * d[i];
                m += 1;
            }
        double den = m * sxx - sx * sx;
        if (m >= 2 && fabs(den) > 1e-30) {
            bb = (float)((m * sxy - sx * sy) / den);
            ba = (float)((sy - bb * sx) / m);
        }
    }
    fit->a = ba;
    fit->b = bb;
    fit->conf = (float)best / (float)n;
    fit->ok = n >= NV2A_AO_MIN_SAMPLES && fit->conf >= min_conf && bb < 0.0f && ba > 0.0f;
    return fit->ok;
}

void nv2a_ao_reset(struct nv2a_ao_trig *t)
{
    memset(t, 0, sizeof *t);
    t->rng = 0x2545f491u;
}

int nv2a_ao_display_like(const struct nv2a_ao_trig *t, uint32_t w, uint32_t h)
{
    if (!w || !h)
        return 0;
    if (!t->disp_w || !t->disp_h)
        return w >= 320;
    if (4u * w < 3u * t->disp_w)
        return 0;
    /* Aspect within 5%: w / h against disp_w / disp_h, cross-multiplied. */
    double l = (double)w * t->disp_h, r = (double)t->disp_w * h;
    return fabs(l - r) <= 0.05 * r;
}

static const void *event(struct nv2a_ao_trig *t)
{
    const void *fire = NULL;
    t->ord++;
    t->armed = 0;
    t->since = 0;
    if (!t->fired && t->ord == t->want && t->fit.ok) {
        fire = t->cand;
        t->fired = 1;
    }
    return fire;
}

void nv2a_ao_forget(struct nv2a_ao_trig *t, const void *rt)
{
    if (rt && t->cand == rt) {
        t->cand = NULL;
        t->armed = 0;
        t->zdraws = 0;
        t->since = 0;
    }
}

const void *nv2a_ao_draw(struct nv2a_ao_trig *t, const void *rt, uint32_t w, uint32_t h,
                         int has_depth, int zwrite, int samples_cand)
{
    const void *fire = NULL;
    const struct nv2a_ao_cfg *c = &s_cfg;
    int disp = nv2a_ao_display_like(t, w, h);
    if (t->armed && (rt != t->cand || !zwrite || !has_depth || samples_cand))
        fire = event(t);
    t->cur_disp = disp && has_depth && zwrite;
    if (!t->cur_disp)
        return fire;
    if (rt != t->cand) {
        t->cand = rt;
        t->zdraws = 0;
        t->since = 0;
        t->armed = 0;
    }
    t->zdraws++;
    t->since++;
    /* The first arm on a candidate waits out a full pass; after an event
     * (a chunk of the world, a lone 2D draw in it) fewer draws re-arm. */
    if (!t->armed && t->since >= (t->since == t->zdraws ? c->arm_draws : c->rearm_draws))
        t->armed = 1;
    return fire;
}

int nv2a_ao_sample_slot(struct nv2a_ao_trig *t, int wmode)
{
    if (!t->cur_disp)
        return -1;
    t->cur_wmode = wmode ? 1 : 0;
    if (wmode)
        t->wdraws++;
    t->zdraws_all++;
    t->seen++;
    if (t->ns < NV2A_AO_SAMPLES)
        return (int)t->ns++;
    unsigned j = rng_next(&t->rng) % t->seen;
    return j < NV2A_AO_SAMPLES ? (int)j : -1;
}

void nv2a_ao_sample_put(struct nv2a_ao_trig *t, int slot, float z, float w, float zmax)
{
    if (slot < 0 || slot >= NV2A_AO_SAMPLES)
        return;
    if (!(w > 0.0f) || !isfinite(w) || !isfinite(z) || !(zmax > 0.0f)) {
        t->sw[slot] = 0; /* the fit skips it */
        return;
    }
    t->sd[slot] = z / zmax;
    t->siw[slot] = 1.0f / w;
    t->sw[slot] = w;
    t->sm[slot] = (uint8_t)t->cur_wmode;
    t->zmax = zmax;
}

const void *nv2a_ao_flip(struct nv2a_ao_trig *t)
{
    /* The flip ends the frame's last pass. The memo can lag a frame when
     * the pass count changes; an armed candidate that has not fired yet
     * fires here, since nothing after the flip can be HUD. */
    if (!t->armed)
        return NULL;
    t->want = t->fired ? t->want : t->ord + 1;
    return event(t);
}

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void nv2a_ao_frame_end(struct nv2a_ao_trig *t, uint32_t shown_w, uint32_t shown_h)
{
    float d[NV2A_AO_SAMPLES], iw[NV2A_AO_SAMPLES], ws[NV2A_AO_SAMPLES];
    unsigned i, nz = 0, nw = 0;
    int wmode = t->zdraws_all && 2u * t->wdraws > t->zdraws_all;
    struct nv2a_ao_fit f = t->fit;

    t->frames++;
    if (t->fired)
        t->fired_frames++;
    else if (t->fit.ok && t->ord)
        t->missed++;
    for (i = 0; i < t->ns; i++) {
        if (!(t->sw[i] > 0.0f) || t->sm[i] != wmode)
            continue;
        ws[nw++] = t->sw[i];
        if (!wmode) {
            d[nz] = t->sd[i];
            iw[nz++] = t->siw[i];
        }
    }
    if (wmode) {
        f.ok = nw > 0;
        f.wmode = 1;
        f.conf = 1.0f;
        f.a = 0;
        f.b = 0;
        f.zmax = t->zmax;
    } else {
        f.wmode = 0;
        f.zmax = t->zmax;
        nv2a_ao_fit_z(d, iw, nz, s_cfg.min_conf, &f);
    }
    if (nw) {
        qsort(ws, nw, sizeof ws[0], cmp_f);
        float p90 = ws[(nw * 9u) / 10u];
        /* Smoothed: the fade's far edge must not breathe with each frame's
         * sample of vertices. */
        f.wref = t->fit.wref > 0.0f && t->fit.wmode == f.wmode ? 0.9f * t->fit.wref + 0.1f * p90
                                                                 : p90;
    }
    if (!f.ok && t->ns)
        t->no_fit++;
    /* A frame with no samples at all (a loading screen) keeps the model of
     * the last 3D frame rather than dropping it and fading in again. */
    if (t->ns)
        t->fit = f;
    t->want = t->ord;
    t->ord = 0;
    t->armed = 0;
    t->fired = 0;
    t->cand = NULL;
    t->zdraws = 0;
    t->since = 0;
    t->ns = 0;
    t->seen = 0;
    t->wdraws = 0;
    t->zdraws_all = 0;
    t->cur_disp = 0;
    if (shown_w && shown_h) {
        t->disp_w = shown_w;
        t->disp_h = shown_h;
    }
}

void nv2a_ao_consts_fill(const struct nv2a_ao_trig *t, const struct nv2a_ao_cfg *c,
                         uint32_t hw, uint32_t hh, uint32_t guest_h, unsigned n,
                         struct nv2a_ao_consts *o)
{
    const float tany = 0.57735027f; /* tan 30: the assumed 60 degree field */
    float gh = guest_h ? (float)guest_h : 480.0f;
    memset(o, 0, sizeof *o);
    o->p0[0] = t->fit.a;
    o->p0[1] = t->fit.b;
    o->p0[2] = (float)t->fit.wmode;
    o->p0[3] = t->fit.zmax;
    /* Screen-constant: the radius is in 480-line guest pixels, so it covers
     * the same share of the picture at 576 lines and at render scale 3. */
    o->p1[0] = c->radius * (gh / 480.0f) * (float)(n ? n : 1);
    o->p1[1] = t->fit.wref > 0.0f ? 1.0f / t->fit.wref : 0.0f;
    o->p1[2] = c->fade_lo;
    o->p1[3] = c->fade_hi;
    o->p2[0] = (float)hw;
    o->p2[1] = (float)hh;
    o->p2[2] = hh ? tany * (float)hw / (float)hh : tany;
    o->p2[3] = tany;
    o->p3[0] = c->strength;
    o->p3[1] = 0.1f; /* angle bias: flat ground shades itself without it */
    o->p3[2] = (float)c->view;
}

void nv2a_ao_dump_parse(const char *v, char *dir, size_t cap, unsigned long *from,
                        unsigned long *n)
{
    const char *c;
    size_t len;
    int drive;
    *from = 0;
    *n = 300;
    if (!cap)
        return;
    dir[0] = 0;
    if (!v || !*v)
        return;
    /* A drive (D:/x, D:\x, D:) only when a separator or the end follows the
     * colon: a:100 is folder a from flip 100. */
    drive = isalpha((unsigned char)v[0]) && v[1] == ':'
        && (v[2] == '/' || v[2] == '\\' || v[2] == 0);
    c = strchr(v + (drive ? 2 : 0), ':');
    len = c ? (size_t)(c - v) : strlen(v);
    if (len >= cap)
        len = cap - 1;
    memcpy(dir, v, len);
    dir[len] = 0;
    if (c)
        sscanf(c + 1, "%lu:%lu", from, n);
}
