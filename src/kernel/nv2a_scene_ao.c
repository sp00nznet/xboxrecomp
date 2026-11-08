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
    0, 1.5f, 20.0f, 0.6f, 1.2f, 16, 8, 0.6f, 0, 0,
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
    c->fb_force = 0;
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

/* The memo: the event count the next frame should fire at first, the
 * fewest of the last AO_MEMO_WIN frames. Firing early costs a pass, since
 * the same target's later events and the flip top it up (event); firing
 * late is a frame with no occlusion, as no event comes after the HUD. The
 * counts of BLiNX 2's hub on the Deck went 3, 2, 2 with slips
 * (3, 3, 2 and 3, 2, 3, 2): no period fitted them, and the period memo of
 * an earlier fix missed a frame in six. Eight frames cover a short frame up to
 * one in eight; the PC's wander (an extra event one frame in thirty, a
 * lost chunk now and then) costs a top-up on the frames above the low. */
#define AO_MEMO_WIN 8

static unsigned hist_at(const struct nv2a_ao_trig *t, unsigned back)
{
    return t->hist[(t->nhist - 1u - back) % NV2A_AO_HIST];
}

/* Frames back from the newest to the newest one with the fewest events. */
static unsigned memo_back(const struct nv2a_ao_trig *t)
{
    unsigned i, best = 0, n = t->nhist < AO_MEMO_WIN ? t->nhist : AO_MEMO_WIN;
    for (i = 1; i < n; i++)
        if (hist_at(t, i) < hist_at(t, best))
            best = i;
    return best;
}

/* Below the memo, does this event look like the last frame's end? A frame
 * one event short of the memo has no pass by its last 3D chunk and takes
 * the flip's fallback, whose colour mask also drops the occlusion under
 * anything translucent drawn after the copy (water, a full-screen dim), so
 * those pixels blink; two short, it gets none. Short frames come every 10
 * to 250 frames, often just past the memo's 8 (the Deck's Stage 1: about
 * 11), so the memo alone sees each one as new. The last event
 * is not marked, so: the kinds so far end with the shortest tail of the
 * last frame's kinds found nowhere else in it (Stage 1: an 'r' then a
 * 'z'). Only a frame with no such tail (the hub's are all 'z') falls back
 * to depth draws: the candidate has as many as the last frame had by its
 * last event, as the hub's short frames draw their 3D in fewer, larger
 * chunks. Not where a tail exists: a frame whose last chunk is small
 * reaches the count one event early (Stage 1 after a long frame, PC
 * flip 1479), a top-up more. And only on a target that ended one of the
 * last two frames (the shown one, double-buffered in BLiNX 2): a
 * display-sized offscreen target drawn first (a reflection, Burnout 3's
 * composited scene) may hold more depth draws than the shown one and
 * would take every first pass, leaving the scene with none. A frame that
 * goes on after all is topped up by its later events, a pass more. */
static int looks_last(const struct nv2a_ao_trig *t)
{
    unsigned n, i, k, m = t->prev_n, o = t->ord;
    if (!m || !o || o > NV2A_AO_EVENTS)
        return 0;
    /* The whole sequence is always unique and never fits (o < want <= m),
     * so only proper tails count. */
    for (n = 1; n < m; n++) {
        /* The tail of n kinds: unique if no earlier prefix ends with it. */
        int uniq = 1;
        for (i = n; i < m && uniq; i++) {
            for (k = 0; k < n && t->prev_why[i - n + k] == t->prev_why[m - n + k]; k++)
                ;
            uniq = k < n;
        }
        if (!uniq)
            continue;
        if (n > o)
            return 0;
        for (k = 0; k < n && t->ev_why[o - n + k] == t->prev_why[m - n + k]; k++)
            ;
        return k == n;
    }
    /* No proper tail is unique only when every event of the last frame
     * had the same kind. */
    return t->prev_zd && t->zdraws >= t->prev_zd && t->ev_why[o - 1] == t->prev_why[m - 1]
           && (t->cand == t->prev_rt[0] || t->cand == t->prev_rt[1]);
}

static const void *event(struct nv2a_ao_trig *t, int why)
{
    const void *fire = NULL;
    t->ord++;
    if (t->ord <= NV2A_AO_EVENTS) {
        t->ev_why[t->ord - 1] = (uint8_t)why;
        t->ev_zd[t->ord - 1] = (uint16_t)(t->zdraws > 0xFFFFu ? 0xFFFFu : t->zdraws);
        t->ev_rt[t->ord - 1] = t->cand;
    }
    t->armed = 0;
    t->since = 0;
    if (!t->fit.ok)
        return NULL;
    if (!t->fired && !t->gave_up) {
        int early = t->ord < t->want && t->cand && !s_cfg.fb_force && looks_last(t);
        if (t->ord == t->want || early) {
            fire = t->cand;
            t->earlies += early ? 1u : 0u;
            t->early_now = early;
            t->fired = 1;
            t->fired_at = t->ord;
            t->fired_rt = t->cand;
        } else if (t->ord + 1u == t->want && t->cand) {
            /* One short of the memo: if the frame ends here, its HUD is
             * next and no event follows it, so the flip's fallback needs
             * the colour as of now (nv2a_ao_snap). */
            t->snap_rt = t->cand;
            t->snap_req = 1;
        }
    } else if (t->cand && t->cand == t->fired_rt) {
        /* A top-up: the occluded target took a chunk more after its pass
         * (an event needs rearm_draws depth-writing draws since the last),
         * so the memo fired early. The backend occludes the pixels whose
         * depth changed since its last pass and corrects the rest. */
        fire = t->cand;
        t->fired++;
        t->topups++;
    }
    if (fire)
        t->late = 0;
    if (fire && t->ord <= NV2A_AO_EVENTS)
        t->ev_fired |= (uint16_t)(1u << (t->ord - 1u));
    return fire;
}

void nv2a_ao_dropped(struct nv2a_ao_trig *t)
{
    if (!t->fired)
        return;
    t->drops++;
    t->drop_total++;
    if (t->fallback) {
        t->fallback = 0;
        t->fallbacks--;
    }
    if (t->ord && t->ord <= NV2A_AO_EVENTS)
        t->ev_fired &= (uint16_t)~(1u << (t->ord - 1u));
    if (t->fired > 1) {
        t->fired--;
        t->topups--;
        return;
    }
    if (t->early_now) {
        t->early_now = 0;
        t->earlies--;
        /* Fired at the event one short of the memo, so no colour copy was
         * asked for: ask now (the backend copies after the pass), or a
         * frame that ends here would get neither its pass nor the flip's
         * fallback. */
        if (t->ord + 1u == t->want && t->fired_rt) {
            t->snap_rt = t->fired_rt;
            t->snap_req = 1;
        }
    }
    /* The first pass: the next event on a candidate takes its place, as
     * nothing of this frame was occluded yet. Once only: a second decline
     * is likely the same missing depth, and the event after it may come
     * after the HUD (a 3D portrait), so the frame goes without. */
    t->fired = 0;
    t->fired_at = 0;
    t->fired_rt = NULL;
    t->want = t->ord + 1;
    if (t->retried)
        t->gave_up = 1;
    t->retried = 1;
}

void nv2a_ao_forget(struct nv2a_ao_trig *t, const void *rt)
{
    if (rt && t->cand == rt) {
        t->cand = NULL;
        t->armed = 0;
        t->zdraws = 0;
        t->since = 0;
    }
    if (rt && t->fired_rt == rt)
        t->fired_rt = NULL;
    if (rt && t->snap_rt == rt) {
        t->snap_rt = NULL;
        t->snap_req = 0;
    }
    /* The backend reuses its target slots: a retired shown buffer's
     * pointer can come back as an offscreen target, which the depth-draw
     * test must not take for the shown one (looks_last). */
    for (unsigned i = 0; rt && i < 2; i++)
        if (t->prev_rt[i] == rt)
            t->prev_rt[i] = NULL;
}

const void *nv2a_ao_snap(struct nv2a_ao_trig *t)
{
    if (!t->snap_req)
        return NULL;
    t->snap_req = 0;
    return t->snap_rt;
}

const void *nv2a_ao_draw(struct nv2a_ao_trig *t, const void *rt, uint32_t w, uint32_t h,
                         int has_depth, int zwrite, int samples_cand)
{
    const void *fire = NULL;
    const struct nv2a_ao_cfg *c = &s_cfg;
    int disp = nv2a_ao_display_like(t, w, h);
    if (t->armed && (rt != t->cand || !zwrite || !has_depth || samples_cand))
        fire = event(t, rt != t->cand ? NV2A_AO_WHY_RT
                        : !has_depth  ? NV2A_AO_WHY_NODEPTH
                        : !zwrite     ? NV2A_AO_WHY_NOZ
                                      : NV2A_AO_WHY_SAMPLE);
    t->cur_disp = disp && has_depth && zwrite;
    if (!t->cur_disp)
        return fire;
    if (t->fired && rt == t->fired_rt)
        t->late++;
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

const void *nv2a_ao_flip(struct nv2a_ao_trig *t, const void *shown)
{
    /* The flip ends the frame's last pass. An armed candidate that has not
     * fired yet fires here (the memo was too high, or the frame is the
     * first), and one that fired tops up the chunk drawn since, since
     * nothing after the flip can be HUD. */
    if (!t->armed) {
        /* A frame one event short of the memo: its last 3D chunk ended
         * before the HUD, so a full pass now would occlude the HUD. The
         * fallback occludes what changed little since the copy (the event
         * one short of the memo): the 3D under a glow, not the HUD drawn
         * over it (nv2a_ao_fb_keep). Only on the shown target: one drawn
         * offscreen and composited (Burnout 3) was consumed already, and
         * a later read of it would see the occlusion. */
        if (t->fired || t->gave_up || !t->snap_rt || t->snap_rt != shown || !t->fit.ok)
            return NULL;
        t->fired = 1;
        t->fired_rt = t->snap_rt;
        t->fallback = 1;
        t->fallbacks++;
        return t->snap_rt;
    }
    t->want = t->fired ? t->want : t->ord + 1;
    return event(t, NV2A_AO_WHY_FLIP);
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
    if (t->late) {
        t->late_frames++;
        t->late_draws += t->late;
    }
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
    if (t->ord) {
        unsigned j = t->nhist++ % NV2A_AO_HIST, o = t->ord;
        t->hist[j] = (uint8_t)(o > 255u ? 255u : o);
        t->want = hist_at(t, memo_back(t)) + (s_cfg.fb_force ? 1u : 0u);
        t->prev_n = o <= NV2A_AO_EVENTS ? o : 0;
        memcpy(t->prev_why, t->ev_why, t->prev_n);
        t->prev_zd = t->prev_n ? t->ev_zd[o - 1] : 0;
        t->prev_rt[1] = t->prev_rt[0];
        t->prev_rt[0] = t->prev_n ? t->ev_rt[o - 1] : NULL;
    } else {
        t->want = 0;
        /* No events and no 3D at all (a loading screen): what follows is
         * another scene, and the old one's period must not steer it. */
        if (!t->ns) {
            t->nhist = 0;
            t->prev_n = 0;
            t->prev_zd = 0;
            t->prev_rt[0] = t->prev_rt[1] = NULL;
        }
    }
    t->memo = t->want;
    t->ord = 0;
    t->armed = 0;
    t->fired = 0;
    t->fired_at = 0;
    t->fired_rt = NULL;
    t->late = 0;
    t->drops = 0;
    t->retried = 0;
    t->gave_up = 0;
    t->snap_rt = NULL;
    t->snap_req = 0;
    t->fallback = 0;
    t->early_now = 0;
    t->ev_fired = 0;
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

void nv2a_ao_frame_str(const struct nv2a_ao_trig *t, char *buf, size_t cap,
                       uint32_t (*id)(const void *rt))
{
    static const char why[] = "?rzdsf";
    size_t o;
    unsigned i, n = t->ord < NV2A_AO_EVENTS ? t->ord : NV2A_AO_EVENTS;
    if (!cap)
        return;
    o = (size_t)snprintf(buf, cap,
                         "ord=%u want=%u at=%u ok=%d conf=%.3f ns=%u late=%u drop=%u fb=%d ev=",
                         t->ord, t->memo, t->fired_at, t->fit.ok, t->fit.conf, t->ns, t->late,
                         t->drops, t->fallback);
    for (i = 0; i < n && o < cap; i++) {
        uint32_t rid = id ? id(t->ev_rt[i]) : (uint32_t)((uintptr_t)t->ev_rt[i] & 0xFFFFFu);
        o += (size_t)snprintf(buf + o, cap - o, "%s%c%u@%x%s", i ? "," : "",
                              why[t->ev_why[i] < sizeof why - 1 ? t->ev_why[i] : 0],
                              (unsigned)t->ev_zd[i], rid, (t->ev_fired >> i) & 1u ? "*" : "");
    }
    if (o >= cap)
        buf[cap - 1] = 0;
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
    /* A fallback (nv2a_ao_flip), a top-up (nv2a_ao_draw) or a first pass. */
    o->p3[3] = t->fallback ? 2.0f : t->fired > 1 ? 1.0f : 0.0f;
}

float nv2a_ao_comp_px(float filt, float d, int top, float prev_t, float prev_d, float *t)
{
    float a = filt;
    *t = filt;
    /* Unchanged depth: the pixel has prev_t already. The total only falls
     * (a multiply cannot brighten), and the record is what was applied,
     * so the next top-up divides by the truth. */
    if (top && d == prev_d) {
        *t = filt < prev_t ? filt : prev_t;
        a = filt < prev_t ? filt / prev_t : 1.0f;
    }
    return a;
}

float nv2a_ao_fb_keep(float change)
{
    float k = 1.0f - (change - NV2A_AO_FB_LO) / (NV2A_AO_FB_HI - NV2A_AO_FB_LO);
    return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
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
