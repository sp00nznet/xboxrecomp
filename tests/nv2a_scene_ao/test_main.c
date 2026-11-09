/*
 * The scene stage's trigger and depth fit (nv2a_scene_ao.c).
 *
 * Driven with made-up frames: one 3D pass then a HUD (fires before the
 * first HUD draw from the second frame on, never on the first: the memo is
 * empty), a world drawn in three chunks split by 2D draws (fires at the
 * last chunk's end, not the first), a pass read by a later draw (a glow
 * reading the 3D target fires at that draw), a handful of depth-writing HUD
 * draws (never arm), a square shadow map (never a candidate), a frame
 * whose 3D runs to the flip (fires at the flip), a pass count that grows
 * by one (fires early on the stale memo), then shrinks back (one frame
 * missed: the last event is known only at the flip), no fire with a
 * failed fit, and the fit itself: a clean projection, two projections in
 * one frame (the majority wins), a menu of scattered depths (refused),
 * W-buffer frames (no fit, wref from w), the constants' radius scaling,
 * the ao_dump parse, a freed candidate (never handed back), a frame one
 * event short of the memo (the colour copied at the event before, the
 * flip's fallback pass, only on the shown target), passes the
 * backend declines (one retry a frame), and frames one event short of the
 * memo past its window: fired at their last event by the last
 * frame's unique tail of event kinds or, with none, by its depth draws,
 * never at an earlier event that reaches the count, on a display-sized
 * offscreen target drawn first, or with an alternating shown buffer lost;
 * an early pass declined still leaves the fallback.
 */
#include "nv2a_scene_ao.h"
#include "nv2a_scene_ao_shaders.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int A, S; /* two targets: the 3D one and a shadow map */
/* The frame helpers return the draw of the frame's last pass (a top-up
 * after the first, if any); g_first holds the first's, g_passes the count. */
static int g_first, g_passes;
/* The draw the colour was copied before (nv2a_ao_snap, -1 none) and
 * whether the frame's pass was the flip's fallback. */
static int g_snap = -1, g_fb;
#define RT_3D ((const void *)&A)
#define RT_SHADOW ((const void *)&S)

/* Draw one vertex sample of a clean projection: n = 0.5, f = 2000. */
static void put_proj(struct nv2a_ao_trig *t, float w, float n, float f, int wmode)
{
    int s = nv2a_ao_sample_slot(t, wmode);
    if (s < 0) return;
    float z;
    if (wmode)
        z = w / f * 16777215.0f;
    else
        z = (f / (f - n) - f * n / ((f - n) * w)) * 16777215.0f;
    nv2a_ao_sample_put(t, s, z, w, 16777215.0f);
}

/* Ends the frame: g_first and g_passes from the trigger's own record. */
static void frame_done(struct nv2a_ao_trig *t)
{
    g_passes = (int)t->fired;
    g_first = t->fired ? (int)t->fired_at : 0;
    g_fb = t->fallback;
    nv2a_ao_frame_end(t, 640, 480);
}

/* Draw i of a frame on the 3D target, as the backend steps it: the
 * trigger, then the colour copy it asks for. */
static const void *draw3d(struct nv2a_ao_trig *t, int zw, int i)
{
    const void *f = nv2a_ao_draw(t, RT_3D, 640, 480, 1, zw, 0);
    if (nv2a_ao_snap(t) == RT_3D)
        g_snap = i;
    return f;
}

/* A frame: 3D (n draws), then HUD (h non-depth draws). Returns the index
 * of the draw that fired (n = first HUD draw), -1 for the flip, -2 none. */
static int frame_3d_hud(struct nv2a_ao_trig *t, unsigned n, unsigned h)
{
    int at = -2;
    unsigned i;
    for (i = 0; i < n + h; i++) {
        int zw = i < n;
        if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, zw, 0)) at = (int)i;
        if (zw) put_proj(t, 2.0f + (float)(i * 53 % 900), 0.5f, 2000.0f, 0);
    }
    if (nv2a_ao_flip(t, RT_3D)) at = -1;
    frame_done(t);
    return at;
}

static void test_simple(void)
{
    struct nv2a_ao_trig t;
    nv2a_ao_reset(&t);
    int a0 = frame_3d_hud(&t, 60, 10);
    int a1 = frame_3d_hud(&t, 60, 10);
    int a2 = frame_3d_hud(&t, 60, 10);
    check(a0 == -2, "first frame: no fit yet, no fire");
    check(a1 == 60 && a2 == 60, "3D then HUD: fires before the first HUD draw");
    check(t.fit.ok && t.fit.b < 0, "clean projection fits");
}

static void test_chunks(void)
{
    struct nv2a_ao_trig t;
    int f, k, last = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++) {
        int at = -2, i = 0;
        /* Three chunks of 30 depth draws split by 2 non-depth draws, a HUD
         * of 5. Draw index of each event: 30, 62, 94. */
        for (k = 0; k < 3; k++) {
            int j;
            for (j = 0; j < 30; j++, i++) {
                if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0) && at == -2) at = i;
                put_proj(&t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
            }
            for (j = 0; j < 2; j++, i++)
                if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0) && at == -2) at = i;
        }
        for (k = 0; k < 5; k++, i++)
            if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0) && at == -2) at = i;
        if (nv2a_ao_flip(&t, RT_3D) && at == -2) at = -1;
        nv2a_ao_frame_end(&t, 640, 480);
        last = at;
    }
    check(last == 94, "three chunks: fires at the last chunk's end");
    check(t.want == 3, "memo holds three events");
}

static void test_sampled(void)
{
    struct nv2a_ao_trig t;
    int f, at = -2;
    nv2a_ao_reset(&t);
    for (f = 0; f < 3; f++) {
        unsigned i;
        at = -2;
        for (i = 0; i < 40; i++) {
            if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0) && at == -2) at = (int)i;
            put_proj(&t, 5.0f + (float)(i * 13 % 300), 0.5f, 2000.0f, 0);
        }
        /* A glow pass on the 3D target itself, still depth-writing state
         * but sampling it. */
        if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 1) && at == -2) at = 40;
        if (nv2a_ao_flip(&t, RT_3D) && at == -2) at = -1;
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(at == 40, "a draw sampling the candidate fires");
}

static void test_hud_never_arms(void)
{
    int f, fired = 0;
    struct nv2a_ao_trig u;
    nv2a_ao_reset(&u);
    for (f = 0; f < 4; f++) {
        unsigned i;
        for (i = 0; i < 6; i++) {
            fired += nv2a_ao_draw(&u, RT_3D, 640, 480, 1, 1, 0) != NULL;
            put_proj(&u, 2.0f + (float)i, 0.5f, 2000.0f, 0);
        }
        for (i = 0; i < 6; i++) fired += nv2a_ao_draw(&u, RT_3D, 640, 480, 1, 0, 0) != NULL;
        fired += nv2a_ao_flip(&u, RT_3D) != NULL;
        nv2a_ao_frame_end(&u, 640, 480);
    }
    check(fired == 0, "six depth-writing draws never arm");
}

static void test_shadow_and_flip(void)
{
    struct nv2a_ao_trig t;
    int f, at = -2;
    nv2a_ao_reset(&t);
    nv2a_ao_frame_end(&t, 640, 480);
    check(!nv2a_ao_display_like(&t, 512, 512), "square shadow map is not display-like");
    check(!nv2a_ao_display_like(&t, 320, 240), "half-size pass is not display-like");
    check(nv2a_ao_display_like(&t, 640, 480) && nv2a_ao_display_like(&t, 720, 540),
          "display size and a larger 4:3 one are");
    for (f = 0; f < 3; f++) {
        unsigned i;
        at = -2;
        for (i = 0; i < 40; i++)
            if (nv2a_ao_draw(&t, RT_SHADOW, 512, 512, 1, 1, 0)) at = 100;
        for (i = 0; i < 40; i++) {
            if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0) && at == -2) at = (int)i;
            put_proj(&t, 2.0f + (float)(i * 31 % 500), 0.5f, 2000.0f, 0);
        }
        if (nv2a_ao_flip(&t, RT_3D) && at == -2) at = -1;
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(at == -1, "3D up to the flip fires at the flip; shadow map never");
}

static void test_growing(void)
{
    struct nv2a_ao_trig t;
    int a1, a2, a3;
    nv2a_ao_reset(&t);
    frame_3d_hud(&t, 60, 10);
    frame_3d_hud(&t, 60, 10);
    /* Now one more event: a 2D draw in the middle (2 events) and the 3D
     * again after it, with nothing at the end but the flip. */
    {
        int at = -2;
        unsigned i;
        for (i = 0; i < 70; i++) {
            int zw = i != 30;
            if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, zw, 0) && at == -2) at = (int)i;
            if (zw) put_proj(&t, 2.0f + (float)(i * 53 % 900), 0.5f, 2000.0f, 0);
        }
        a1 = at;
        if (nv2a_ao_flip(&t, RT_3D)) a1 = a1 == 30 ? -1 : -3;
        frame_done(&t);
    }
    check(a1 == -1 && g_passes == 2,
          "a new early event fires on the memo, the flip tops up the 3D after it");
    a2 = frame_3d_hud(&t, 60, 10);
    a3 = frame_3d_hud(&t, 60, 10);
    check(a2 == 60 && g_passes == 1 && t.missed == 0,
          "fewer events again: the memo kept the low, no frame missed");
    check(a3 == 60, "and stays on it");
}

/* A frame of k chunks of 30 depth draws, each ended by 2 non-depth draws
 * (k events), then a HUD of 5. Returns the draw that fired, -1 the flip, -2
 * none; *last is the index of the last chunk's end. */
static int frame_chunks(struct nv2a_ao_trig *t, unsigned k, int *last)
{
    int at = -2, i = 0;
    g_snap = -1;
    unsigned c, j;
    for (c = 0; c < k; c++) {
        for (j = 0; j < 30; j++, i++) {
            if (draw3d(t, 1, i)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        for (j = 0; j < 2; j++, i++)
            if (draw3d(t, 0, i)) at = i;
    }
    for (j = 0; j < 5; j++, i++)
        if (draw3d(t, 0, i)) at = i;
    if (nv2a_ao_flip(t, RT_3D)) at = -1;
    frame_done(t);
    return at;
}

static int frame_sizes(struct nv2a_ao_trig *t, const unsigned *sz, unsigned k, int *last);

/* frame_chunks up to its HUD, the flip and frame end left to the caller. */
static void frame_chunks_open(struct nv2a_ao_trig *t, unsigned k)
{
    int i = 0;
    unsigned c, j;
    g_snap = -1;
    for (c = 0; c < k; c++) {
        for (j = 0; j < 30; j++, i++) {
            draw3d(t, 1, i);
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        for (j = 0; j < 2; j++, i++)
            draw3d(t, 0, i);
    }
    for (j = 0; j < 5; j++, i++)
        draw3d(t, 0, i);
}

/* The Deck's event count cycled N, N, N + 1. The previous frame's
 * count fired early on the long frame and missed the frame after it; the
 * memo keeps the low and the long frame is topped up. bad: frames whose
 * last event got no pass; *extra: the top-ups. */
static unsigned cycle_bad(const unsigned *pat, unsigned plen, unsigned frames, unsigned from,
                          unsigned *extra)
{
    struct nv2a_ao_trig t;
    unsigned f, bad = 0, x = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < frames; f++) {
        int last, at = frame_chunks(&t, pat[f % plen], &last);
        if (f >= from && at != last) bad++;
        if (f >= from && g_passes > 1) x += (unsigned)g_passes - 1u;
    }
    if (extra) *extra = x;
    return bad;
}

static void test_cycle(void)
{
    static const unsigned n_n_n1[] = { 2, 2, 3 }, n_n1[] = { 3, 4 }, n4[] = { 2, 2, 2, 3 },
                          flat[] = { 3 };
    unsigned x;
    check(cycle_bad(n_n_n1, 3, 60, 16, &x) == 0 && x == 15,
          "N, N, N+1: every frame covered, a top-up on each long one");
    check(cycle_bad(n_n1, 2, 60, 16, NULL) == 0, "N, N+1: every frame covered");
    check(cycle_bad(n4, 4, 60, 16, NULL) == 0, "N, N, N, N+1: every frame covered");
    check(cycle_bad(flat, 1, 60, 2, &x) == 0 && x == 0,
          "a steady count: every frame after the first fit, no top-up");
}

/* Longer periods, both ways: one short or one long frame in k.
 * Up to one in eight the memo's window holds the short frame. */
static void test_one_in_k(void)
{
    unsigned k, bad = 0;
    for (k = 5; k <= 8; k++) {
        unsigned pat[8], i;
        for (i = 0; i < k; i++) pat[i] = i + 1 < k ? 3 : 2;
        bad += cycle_bad(pat, k, 80, 16, NULL);
        for (i = 0; i < k; i++) pat[i] = i + 1 < k ? 2 : 3;
        bad += cycle_bad(pat, k, 80, 16, NULL);
    }
    check(bad == 0, "one short or long frame in 5 to 8: every frame covered");
}

/* BLiNX 2's hub on the Deck: chunks of 291, about 344 and about
 * 333 depth draws, the third present on about one frame in three, the
 * counts as the trace had them (3, 2, 2 with slips: 3, 3, 3 and 3, 2, 3, 2).
 * The old period memo missed 235 of 1305 such frames; the chunks are
 * alike, so the tail rule cannot tell the third from the second. */
static void test_deck_hub(void)
{
    static const char seq[] =
        "322322322333222322322322323223332232232232233322232232232232"
        "322333223223223223332223223223223232233322322322322333222322";
    struct nv2a_ao_trig t;
    unsigned f, bad = 0, extra = 0, seed = 7;
    uint64_t missed = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 2 * (sizeof seq - 1); f++) {
        if (f == 8) missed = t.missed;
        unsigned sz[3], k = (unsigned)(seq[f % (sizeof seq - 1)] - '0');
        int last, at;
        seed = seed * 1103515245u + 12345u;
        sz[0] = 291;
        sz[1] = 340 + (seed >> 16) % 13;
        sz[2] = 329 + (seed >> 20) % 13;
        at = frame_sizes(&t, sz, k, &last);
        if (f >= 8 && at != last) bad++;
        if (f >= 8 && g_passes > 1) extra++;
    }
    check(bad == 0 && t.missed == missed, "Deck hub counts: every frame covered, none missed");
    check(extra > 0 && extra < f / 2, "Deck hub counts: the long frames topped up");
}

/* A top-up is for the target the first pass occluded: a later chunk on
 * another display-sized target (a second 3D view) runs nothing, and a
 * forgotten target is never topped up. */
static void test_topup_target(void)
{
    static int B;
    struct nv2a_ao_trig t;
    unsigned i, f;
    int got = 0, freed = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++) {
        for (i = 0; i < 40; i++) {
            nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
            put_proj(&t, 2.0f + (float)(i * 31 % 500), 0.5f, 2000.0f, 0);
        }
        nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0);
        if (f < 3) {
            nv2a_ao_flip(&t, RT_3D);
            frame_done(&t);
            continue;
        }
        /* Fired on RT_3D at its event; then 40 depth draws on B. */
        for (i = 0; i < 40; i++)
            got |= nv2a_ao_draw(&t, (const void *)&B, 640, 480, 1, 1, 0) != NULL;
        got |= nv2a_ao_draw(&t, (const void *)&B, 640, 480, 1, 0, 0) != NULL;
        got |= nv2a_ao_flip(&t, RT_3D) != NULL;
        frame_done(&t);
    }
    check(!got && g_passes == 1, "top-up: another target's chunk runs nothing");
    for (i = 0; i < 40; i++) {
        nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
        put_proj(&t, 2.0f + (float)(i * 31 % 500), 0.5f, 2000.0f, 0);
    }
    nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0);
    freed = t.fired == 1;
    nv2a_ao_forget(&t, RT_3D);
    for (i = 0; i < 40; i++)
        got |= nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0) != NULL;
    got |= nv2a_ao_flip(&t, RT_3D) != NULL;
    frame_done(&t);
    check(freed && !got, "top-up: a forgotten target is not topped up");
}

/* One frame in thirty with an extra event (the PC's wander): the memo stays
 * on the usual count and the odd frame is topped up. */
static void test_wander(void)
{
    struct nv2a_ao_trig t;
    unsigned f, bad = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 89; f++) {
        int last, at = frame_chunks(&t, f % 30 == 29 ? 3 : 2, &last);
        if (f >= 2 && at != last) bad++;
    }
    check(bad == 0, "a lone extra event is topped up, no frame off");
    check(t.want == 2, "and the memo is back on the usual count");
}

/* A frame of k chunks of 30 depth draws and a tail of 12, each ended by 2
 * non-depth draws, then a HUD of 5: k + 1 events, the last 12 draws after
 * the one before. Returns as frame_chunks. */
static int frame_tail(struct nv2a_ao_trig *t, unsigned k, int *last)
{
    int at = -2, i = 0;
    g_snap = -1;
    unsigned c, j;
    for (c = 0; c <= k; c++) {
        for (j = 0; j < (c < k ? 30u : 12u); j++, i++) {
            if (draw3d(t, 1, i)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        for (j = 0; j < 2; j++, i++)
            if (draw3d(t, 0, i)) at = i;
    }
    for (j = 0; j < 5; j++, i++)
        if (draw3d(t, 0, i)) at = i;
    if (nv2a_ao_flip(t, RT_3D)) at = -1;
    frame_done(t);
    return at;
}

/* A frame that loses a chunk now and then (the PC's misses): one
 * event short of the memo, the frame ends with no pass before its HUD. The
 * colour is copied before the draw after its last event, and the flip runs
 * the fallback (masked to the pixels that changed little since that copy),
 * so no frame is missed; the frames after a short one fire early and are
 * topped up. */
static void test_lost_chunk(void)
{
    struct nv2a_ao_trig t;
    unsigned f, bad = 0, fb = 0;
    uint64_t missed;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++) {
        int last;
        frame_tail(&t, 3, &last);
    }
    missed = t.missed;
    for (f = 0; f < 20; f++) {
        int shortf = f % 5 == 4, last, at = frame_tail(&t, shortf ? 2 : 3, &last);
        fb += g_fb;
        /* The first short frame: the memo still says 4. The next ones are
         * in the window of 8, so the memo is 3 and they fire at the last. */
        if (shortf && f == 4)
            bad += at != -1 || !g_fb || g_snap != last;
        else
            bad += at != last || g_fb;
    }
    check(t.missed == missed && fb == 1 && t.fallbacks == 1,
          "a frame one chunk short gets the flip's fallback, none missed");
    check(bad == 0, "copied at its last event; the frames after it fire early, topped up");
}

/* BLiNX 2's hub on the Mac: 4 events for about 45 frames, then
 * 3 for about 30. The memo is the low of 8 frames, so the first frame of
 * each run of 3s came one event short of it with no pass: a full dropout
 * every 2.5 s. Now each of those is the flip's fallback, and only those. */
static void test_mac_hub(void)
{
    struct nv2a_ao_trig t;
    unsigned f, k, prev = 4, bad = 0, downs = 0, fb = 0;
    uint64_t missed = 0, fb0 = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 600; f++) {
        int last, at;
        k = (f % 75) < 45 ? 4 : 3;
        if (f == 8) {
            missed = t.missed;
            fb0 = t.fallbacks;
        }
        at = frame_chunks(&t, k, &last);
        if (f >= 8) {
            downs += k < prev;
            fb += g_fb;
            bad += g_fb ? at != -1 || g_snap != last || k >= prev : at != last;
        }
        prev = k;
    }
    check(bad == 0 && t.missed == missed, "Mac hub runs of 4 and 3: every frame covered");
    check(fb == downs && t.fallbacks - fb0 == downs && downs == 8,
          "Mac hub: a fallback on each first frame of 3s, no other");
}

/* The fallback's mask (nv2a_ao_fb_keep, ao_comp_fs): the story hub's glow
 * moves most pixels a few levels after the last chunk and must keep the
 * occlusion; the HUD's text moves them far and must keep none. Both
 * shaders spell the same ramp. */
static void test_fallback_mask(void)
{
    char ramp[128];
    check(nv2a_ao_fb_keep(0.0f) == 1.0f && nv2a_ao_fb_keep(0.06f) == 1.0f,
          "fallback mask: unchanged and glow-sized changes keep it all");
    check(fabsf(nv2a_ao_fb_keep(0.13f) - 0.5f) < 1e-4f, "fallback mask: half way at 0.13");
    check(nv2a_ao_fb_keep(0.2f) == 0.0f && nv2a_ao_fb_keep(1.0f) == 0.0f,
          "fallback mask: HUD-sized changes keep none");
    /* The whole mask, not just its numbers: the largest channel's change,
     * 1 - the ramp, clamped, then applied to both the factor and the
     * record (MSL mix, HLSL lerp). */
    snprintf(ramp, sizeof ramp,
             "float k = saturate(1.0 - (max(dc.r, max(dc.g, dc.b)) - %.2f) / %.2f);\n",
             (double)NV2A_AO_FB_LO, (double)(NV2A_AO_FB_HI - NV2A_AO_FB_LO));
    check(strstr(NV2A_AO_MSL, ramp) && strstr(NV2A_AO_HLSL, ramp),
          "fallback mask: MSL and HLSL use the C ramp");
    check(strstr(NV2A_AO_MSL, "a = mix(1.0, a, k);\n")
              && strstr(NV2A_AO_MSL, "t = mix(1.0, t, k);\n")
              && strstr(NV2A_AO_HLSL, "a = lerp(1.0, a, k);\n")
              && strstr(NV2A_AO_HLSL, "t = lerp(1.0, t, k);\n"),
          "fallback mask: both shaders mask the factor and the record");
}

/* The fallback's own edges: none when the copied target was freed, one
 * the backend declines (no copy of the target's size) is undone, and its
 * constants select the masked pass. */
static void test_fallback_edges(void)
{
    struct nv2a_ao_trig t;
    struct nv2a_ao_consts k;
    const void *got;
    char line[400];
    int last;
    unsigned f;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++)
        frame_chunks(&t, 3, &last);
    /* Two chunks: the second's event asks for the copy. */
    frame_chunks_open(&t, 2);
    check(g_snap == 62, "fallback: the copy is asked for at the event one short");
    got = nv2a_ao_flip(&t, RT_3D);
    nv2a_ao_consts_fill(&t, nv2a_ao_cfg(), 640, 480, 480, 1, &k);
    nv2a_ao_frame_str(&t, line, sizeof line, NULL);
    check(got == RT_3D && t.fallback && t.fired == 1 && k.p3[3] == 2.0f,
          "fallback: the flip hands the copied target back, mode 2");
    check(strstr(line, " fb=1 ev=") != NULL, "fallback: ao_frames shows fb=1");
    nv2a_ao_dropped(&t);
    check(!t.fallback && !t.fired && t.fallbacks == 0, "fallback: a declined one is undone");
    frame_done(&t);
    check(t.missed == 1, "fallback: and that frame counts as missed");
    for (f = 0; f < 8; f++)
        frame_chunks(&t, 3, &last);
    frame_chunks_open(&t, 2);
    nv2a_ao_forget(&t, RT_3D);
    got = nv2a_ao_flip(&t, RT_3D);
    check(got == NULL && !t.fallback, "fallback: none when the copied target was freed");
    frame_done(&t);
    for (f = 0; f < 8; f++)
        frame_chunks(&t, 3, &last);
    frame_chunks_open(&t, 3);
    got = nv2a_ao_flip(&t, RT_3D);
    check(got == NULL && !t.fallback && t.fired == 1, "fallback: none after a pass");
    frame_done(&t);
    /* A target drawn offscreen and composited before the flip (Burnout 3):
     * the copy is of it, but the flip shows another; a pass there would
     * occlude a target already consumed. */
    for (f = 0; f < 8; f++)
        frame_chunks(&t, 3, &last);
    frame_chunks_open(&t, 2);
    got = nv2a_ao_flip(&t, RT_SHADOW);
    check(got == NULL && !t.fallback && t.fallbacks == 0,
          "fallback: none when another target is shown");
    got = nv2a_ao_flip(&t, NULL);
    check(got == NULL && !t.fallback, "fallback: none when nothing is shown");
    frame_done(&t);
}

/* Frames of chunks of the given sizes, each ended by 2 non-depth draws,
 * then a HUD of 5. Returns as frame_chunks. */
static int frame_sizes(struct nv2a_ao_trig *t, const unsigned *sz, unsigned k, int *last)
{
    int at = -2, i = 0;
    g_snap = -1;
    unsigned c, j;
    for (c = 0; c < k; c++) {
        for (j = 0; j < sz[c]; j++, i++) {
            if (draw3d(t, 1, i)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        for (j = 0; j < 2; j++, i++)
            if (draw3d(t, 0, i)) at = i;
    }
    for (j = 0; j < 5; j++, i++)
        if (draw3d(t, 0, i)) at = i;
    if (nv2a_ao_flip(t, RT_3D)) at = -1;
    frame_done(t);
    return at;
}

/* Chunks of the given sizes on the 3D target, each ended by 2 non-depth
 * draws (a 'z' event) or, where rt[c] is set, by a draw to the shadow map
 * (an 'r'), then a HUD of 5. Returns as frame_chunks. */
static int frame_kinds(struct nv2a_ao_trig *t, const unsigned *sz, const char *rt, unsigned k,
                       int *last)
{
    int at = -2, i = 0;
    unsigned c, j;
    g_snap = -1;
    for (c = 0; c < k; c++) {
        for (j = 0; j < sz[c]; j++, i++) {
            if (draw3d(t, 1, i)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        if (rt[c]) {
            if (nv2a_ao_draw(t, RT_SHADOW, 512, 512, 1, 1, 0)) at = i;
            i++;
        } else {
            for (j = 0; j < 2; j++, i++)
                if (draw3d(t, 0, i)) at = i;
        }
    }
    for (j = 0; j < 5; j++, i++)
        if (draw3d(t, 0, i)) at = i;
    if (nv2a_ao_flip(t, RT_3D)) at = -1;
    frame_done(t);
    return at;
}

/* A frame one event short every 11, just past the memo's 8 (the
 * Deck's Stage 1), saw a memo one high each time and took the flip's
 * fallback, whose mask drops the occlusion under translucent water: a
 * blink. Stage 1's kinds end 'r' then 'z' (zzzzrz, short zzzrz), which
 * nothing else in the frame does, so the short frame fires at its last
 * event; the hub's are all 'z' (zzzz, short zzz) and its short frames draw
 * the same 3D in fewer chunks, so they fire once the candidate has the
 * last frame's depth draws. Either way one pass, never a second. */
static void test_short_past_memo(void)
{
    static const unsigned st_long[] = { 30, 30, 30, 30, 30, 30 }, st_short[] = { 30, 30, 30, 30, 30 };
    static const char st_rl[] = { 0, 0, 0, 0, 1, 0 }, st_rs[] = { 0, 0, 0, 1, 0 };
    static const unsigned hub_long[] = { 30, 30, 30, 30 }, hub_short[] = { 40, 40, 40 };
    static const char none[6] = { 0 };
    struct nv2a_ao_trig t;
    unsigned f, bad = 0, dbl = 0, shorts = 0;
    uint64_t fb0 = 0, missed = 0, early0 = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 120; f++) {
        int sh = f % 11 == 10, last, at;
        if (f == 8) {
            fb0 = t.fallbacks;
            missed = t.missed;
            early0 = t.earlies;
        }
        at = sh ? frame_kinds(&t, st_short, st_rs, 5, &last) : frame_kinds(&t, st_long, st_rl, 6, &last);
        if (f < 8)
            continue;
        shorts += sh;
        bad += at != last || g_fb;
        dbl += sh && g_passes != 1;
    }
    check(bad == 0 && t.fallbacks == fb0 && t.missed == missed,
          "Stage 1, a short frame in 11: each fires at its last event, no fallback");
    check(dbl == 0 && t.earlies - early0 == shorts, "Stage 1: the short ones by their tail, one pass");
    nv2a_ao_reset(&t);
    bad = dbl = shorts = 0;
    for (f = 0; f < 120; f++) {
        int sh = f % 11 == 10, last, at;
        if (f == 8) {
            fb0 = t.fallbacks;
            missed = t.missed;
            early0 = t.earlies;
        }
        at = sh ? frame_kinds(&t, hub_short, none, 3, &last) : frame_kinds(&t, hub_long, none, 4, &last);
        if (f < 8)
            continue;
        shorts += sh;
        bad += at != last || g_fb;
        dbl += sh && g_passes != 1;
    }
    check(bad == 0 && t.fallbacks == fb0 && t.missed == missed,
          "hub, a short frame in 11: each fires at its last event, no fallback");
    check(dbl == 0 && t.earlies - early0 == shorts, "hub: the short ones by their depth draws, one pass");
}

/* A frame on the shown target `shown`, after `off` depth draws on a
 * display-sized offscreen target (a reflection; 0 none) ended by 2
 * non-depth draws, so its event is a 'z' like the chunks': chunks of the
 * given sizes, each ended by 2 non-depth draws or (rt[c]) a draw to the
 * shadow map, then a HUD of 5 and the flip. *first_rt: the target of the
 * first pass. With drop set, the backend declines an early first pass
 * (nv2a_ao_dropped) and copies the colour if asked (g_snap). Returns as
 * frame_chunks. */
static int g_off, g_a2; /* the offscreen target, the shown one's twin */
static int frame_on(struct nv2a_ao_trig *t, const void *shown, unsigned off, const unsigned *sz,
                    const char *rt, unsigned k, int drop, int *last, const void **first_rt)
{
    int at = -2, i = 0;
    unsigned c, j;
    const void *f;
    g_snap = -1;
    *first_rt = NULL;
    for (j = 0; j < off + (off ? 2u : 0u); j++, i++) {
        f = nv2a_ao_draw(t, &g_off, 640, 480, 1, j < off, 0);
        if (f && !*first_rt) *first_rt = f;
        if (j < off) put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
    }
    for (c = 0; c < k; c++) {
        for (j = 0; j < sz[c] + 2u; j++, i++) {
            int zw = j < sz[c];
            if (rt && rt[c] && !zw) {
                if (j == sz[c]) {
                    f = nv2a_ao_draw(t, RT_SHADOW, 512, 512, 1, 1, 0);
                    if (f && !*first_rt) *first_rt = f;
                    if (f) at = i;
                }
                continue;
            }
            f = nv2a_ao_draw(t, shown, 640, 480, 1, zw, 0);
            if (f && !*first_rt) {
                *first_rt = f;
                if (drop && t->early_now) {
                    nv2a_ao_dropped(t);
                    f = NULL;
                    *first_rt = NULL;
                }
            }
            if (f) at = i;
            if (nv2a_ao_snap(t) == shown) g_snap = i;
            if (zw) put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i - 2;
    }
    for (j = 0; j < 5; j++, i++) {
        f = nv2a_ao_draw(t, shown, 640, 480, 1, 0, 0);
        if (f && !*first_rt) *first_rt = f;
        if (f) at = i;
    }
    f = nv2a_ao_flip(t, shown);
    if (f && !*first_rt) *first_rt = f;
    if (f) at = -1;
    frame_done(t);
    return at;
}

/* Review of the tail rule: where the last frame has a unique tail, the depth
 * draws must not decide. The PC's Stage 1 after a long frame
 * (a PC run, flips 1479 and 4655): the long frame
 * zzzzrz ends at 163 draws on a last chunk of 13, the short zzzrz's 'r'
 * comes at 163 too, and firing there ran a top-up at the real last 'z'. */
static void test_short_tail_first(void)
{
    static const unsigned lng[] = { 30, 30, 30, 30, 30, 13 }, sht[] = { 40, 40, 40, 43, 13 };
    static const char rl[] = { 0, 0, 0, 0, 1, 0 }, rs[] = { 0, 0, 0, 1, 0 };
    struct nv2a_ao_trig t;
    const void *frt;
    unsigned f, bad = 0, dbl = 0, ord = 0, shorts = 0;
    uint64_t early0 = 0, fb0 = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 120; f++) {
        int sh = f % 11 == 10, last, at;
        if (f == 8) {
            early0 = t.earlies;
            fb0 = t.fallbacks;
        }
        at = sh ? frame_on(&t, RT_3D, 0, sht, rs, 5, 0, &last, &frt)
                : frame_on(&t, RT_3D, 0, lng, rl, 6, 0, &last, &frt);
        if (f < 8 || !sh)
            continue;
        shorts++;
        bad += at != last || g_fb;
        dbl += g_passes != 1;
        ord += g_first != 5;
    }
    check(bad == 0 && dbl == 0 && t.fallbacks == fb0, "short zzzrz at the long's count: one pass, at the end");
    check(ord == 0 && t.earlies - early0 == shorts, "short zzzrz: fires at its last 'z', not the 'r'");
}

/* Review 2 of the tail rule: the order of the two tests and the depth-draw
 * test's kind check, each against a short frame its depth draws would
 * fire early. (a) After zzzzrz ending at 163 draws, a short zzrz
 * (z80, z163, r173, z183) reaches 163 at its second event: the tail
 * test must decide, at the fourth, in one pass. (b) After an all-'z'
 * frame ending at 120, a short zrz (z60, r120, z130) reaches 120 at its
 * 'r': only a 'z' may fire, at the third, in one pass. */
static void test_short_order_kind(void)
{
    static const unsigned la[] = { 30, 30, 30, 30, 30, 13 }, sa[] = { 80, 83, 10, 10 };
    static const char rla[] = { 0, 0, 0, 0, 1, 0 }, rsa[] = { 0, 0, 1, 0 };
    static const unsigned lb[] = { 30, 30, 30, 30 }, sb[] = { 60, 60, 10 };
    static const char rsb[] = { 0, 1, 0 };
    struct nv2a_ao_trig t;
    const void *frt;
    unsigned f, bad = 0, shorts = 0, pass;
    for (pass = 0; pass < 2; pass++) {
        nv2a_ao_reset(&t);
        bad = shorts = 0;
        for (f = 0; f < 120; f++) {
            int sh = f % 11 == 10, last, at;
            if (pass == 0)
                at = sh ? frame_on(&t, RT_3D, 0, sa, rsa, 4, 0, &last, &frt)
                        : frame_on(&t, RT_3D, 0, la, rla, 6, 0, &last, &frt);
            else
                at = sh ? frame_on(&t, RT_3D, 0, sb, rsb, 3, 0, &last, &frt)
                        : frame_on(&t, RT_3D, 0, lb, NULL, 4, 0, &last, &frt);
            if (f < 8 || !sh)
                continue;
            shorts++;
            bad += at != last || g_fb || g_passes != 1 || g_first != (pass == 0 ? 4 : 3);
        }
        check(shorts && bad == 0,
              pass == 0 ? "short zzrz past the long's count early: the tail decides, one pass"
                        : "after all 'z', a short frame's 'r' at the count: fires at the 'z'");
    }
}

/* Review of the tail rule: a display-sized offscreen target drawn first (B,
 * 150 depth draws) then the shown one, double-buffered (A and its twin swap
 * each frame, as BLiNX 2's @8313c000 and @83268000), in 4 chunks of 30: every
 * event a 'z', so only the depth-draw test can fire early. B has more
 * than the shown target's 120 at its event, but ended no frame: it must
 * never take the first pass, or the scene goes without. Short frames (3
 * chunks of 40, one in 11) still fire early on the shown buffer, which
 * ended the frame before last. */
static void test_short_offscreen(void)
{
    static const unsigned lng[] = { 30, 30, 30, 30 }, sht[] = { 40, 40, 40 };
    struct nv2a_ao_trig t;
    const void *frt;
    unsigned f, onb = 0, bad = 0, dbl = 0, shorts = 0, steady_bad = 0;
    uint64_t early0 = 0, fb0 = 0, miss0 = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 60; f++) {
        const void *shown = f & 1 ? (const void *)&g_a2 : RT_3D;
        int last, at = frame_on(&t, shown, 150, lng, NULL, 4, 0, &last, &frt);
        if (f < 8)
            continue;
        onb += frt == &g_off;
        steady_bad += frt != shown || at != last || g_passes != 1;
    }
    check(onb == 0 && steady_bad == 0 && t.earlies == 0,
          "offscreen first, steady: every pass on the shown buffer, at its end");
    nv2a_ao_reset(&t);
    onb = 0;
    for (f = 0; f < 120; f++) {
        const void *shown = f & 1 ? (const void *)&g_a2 : RT_3D;
        int sh = f % 11 == 10, last, at;
        if (f == 8) {
            early0 = t.earlies;
            fb0 = t.fallbacks;
            miss0 = t.missed;
        }
        at = sh ? frame_on(&t, shown, 150, sht, NULL, 3, 0, &last, &frt)
                : frame_on(&t, shown, 150, lng, NULL, 4, 0, &last, &frt);
        if (f < 8)
            continue;
        onb += frt == &g_off;
        if (!sh)
            continue;
        shorts++;
        bad += frt != shown || at != last || g_fb;
        dbl += g_passes != 1;
    }
    check(onb == 0 && bad == 0 && dbl == 0 && t.fallbacks == fb0 && t.missed == miss0,
          "offscreen first, a short frame in 11: one pass on the shown");
    check(t.earlies - early0 == shorts, "alternating shown buffers: the short ones fire early");
}

/* Review of the tail rule: an early first pass the backend declines is not
 * counted, and one fired at the event one short of the memo still copies
 * the colour, so a frame that ends there takes the flip's fallback. */
static void test_short_early_dropped(void)
{
    static const unsigned lng[] = { 30, 30, 30, 30, 30, 30 }, sht[] = { 30, 30, 30, 30, 30 };
    static const char rl[] = { 0, 0, 0, 0, 1, 0 }, rs[] = { 0, 0, 0, 1, 0 };
    struct nv2a_ao_trig t;
    const void *frt;
    unsigned f, fb = 0, snap = 0, shorts = 0;
    uint64_t early0 = 0, miss0 = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 120; f++) {
        int sh = f % 11 == 10, last;
        if (f == 8) {
            early0 = t.earlies;
            miss0 = t.missed;
        }
        if (sh)
            frame_on(&t, RT_3D, 0, sht, rs, 5, 1, &last, &frt);
        else
            frame_on(&t, RT_3D, 0, lng, rl, 6, 0, &last, &frt);
        if (f < 8 || !sh)
            continue;
        shorts++;
        fb += g_fb;
        snap += g_snap == last;
    }
    check(shorts && t.earlies == early0, "early pass declined: not counted as fired early");
    check(fb == shorts && snap == shorts && t.missed == miss0,
          "early pass declined one short: colour copied, the flip's fallback");
}

/* A steady count of chunks whose sizes jitter by a few draws, the last
 * two close (review of the memo fix): the memo counts events, not draws, so
 * no frame fires a chunk early (a top-up) or misses its last. */
static void test_jittered_chunks(void)
{
    static const unsigned base[][2] = { { 24, 20 }, { 20, 24 }, { 30, 26 }, { 14, 13 } };
    unsigned b, f, early = 0, missed = 0, seed = 1;
    for (b = 0; b < 4; b++) {
        struct nv2a_ao_trig t;
        nv2a_ao_reset(&t);
        for (f = 0; f < 200; f++) {
            unsigned sz[3];
            int last, at;
            seed = seed * 1103515245u + 12345u;
            sz[0] = 40;
            sz[1] = base[b][0] + (seed >> 16) % 5;
            sz[2] = base[b][1] + (seed >> 20) % 5;
            at = frame_sizes(&t, sz, 3, &last);
            if (f >= 2 && g_passes > 1) early++;
            if (f >= 2 && at != last) missed++;
        }
    }
    check(early == 0 && missed == 0, "close, jittered last chunks: never early, never missed");
}

/* A scene change after the memo locked on to a period: a loading screen
 * (no events, no 3D) clears the history, so the new scene's steady count
 * is the memo from its second frame. */
static void test_scene_change(void)
{
    static const unsigned pat[] = { 2, 2, 3 };
    struct nv2a_ao_trig t;
    unsigned f, bad = 0;
    int last;
    nv2a_ao_reset(&t);
    for (f = 0; f < 30; f++)
        frame_chunks(&t, pat[f % 3], &last);
    nv2a_ao_flip(&t, RT_3D);
    nv2a_ao_frame_end(&t, 640, 480);
    check(t.nhist == 0 && t.want == 0, "a loading screen clears the memo's history");
    for (f = 0; f < 12; f++) {
        int at = frame_chunks(&t, 4, &last);
        if (f >= 1 && at != last) bad++;
    }
    check(bad == 0, "the new scene's count is the memo from its second frame");
}

static void test_no_fit(void)
{
    struct nv2a_ao_trig t;
    int f, fired = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 3; f++) {
        unsigned i;
        for (i = 0; i < 60; i++) {
            fired += nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0) != NULL;
            /* Scattered: no projection behind them (a menu's flat cards). */
            int s = nv2a_ao_sample_slot(&t, 0);
            if (s >= 0)
                nv2a_ao_sample_put(&t, s, (float)((i * 7919u) % 1000u) * 16777.0f,
                                   1.0f + (float)((i * 104729u) % 50u), 16777215.0f);
        }
        for (i = 0; i < 5; i++) fired += nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0) != NULL;
        fired += nv2a_ao_flip(&t, RT_3D) != NULL;
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(fired == 0 && !t.fit.ok && t.no_fit == 3, "scattered depths: refused, no fire");
}

static void test_fit_two_projections(void)
{
    float d[200], iw[200];
    unsigned i, n = 0;
    struct nv2a_ao_fit f;
    for (i = 0; i < 150; i++, n++) {
        float w = 1.0f + (float)i * 7.0f, nn = 0.5f, ff = 2000.0f;
        d[n] = ff / (ff - nn) - ff * nn / ((ff - nn) * w);
        iw[n] = 1.0f / w;
    }
    for (i = 0; i < 50; i++, n++) {
        float w = 1.0f + (float)i * 3.0f, nn = 0.1f, ff = 100.0f;
        d[n] = ff / (ff - nn) - ff * nn / ((ff - nn) * w);
        iw[n] = 1.0f / w;
    }
    nv2a_ao_fit_z(d, iw, n, 0.6f, &f);
    float ea = 2000.0f / 1999.5f, eb = -2000.0f * 0.5f / 1999.5f;
    check(f.ok && fabsf(f.a - ea) < 1e-3f && fabsf(f.b - eb) < 1e-3f,
          "two projections: the majority's model");
    check(f.conf > 0.7f && f.conf < 0.8f, "two projections: inlier share 0.75");
    nv2a_ao_fit_z(d, iw, 10, 0.6f, &f);
    check(!f.ok, "too few samples: refused");
}

static void test_wmode(void)
{
    struct nv2a_ao_trig t;
    int f, at = -2;
    nv2a_ao_reset(&t);
    for (f = 0; f < 3; f++) {
        unsigned i;
        at = -2;
        for (i = 0; i < 50; i++) {
            if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0) && at == -2) at = (int)i;
            put_proj(&t, 1.0f + (float)i * 10.0f, 0.5f, 1000.0f, 1);
        }
        if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0) && at == -2) at = 50;
        nv2a_ao_flip(&t, RT_3D);
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(at == 50 && t.fit.ok && t.fit.wmode, "W-buffer frames: no fit needed, fires");
    check(t.fit.wref > 400.0f && t.fit.wref < 500.0f, "W-buffer: wref near the 90th percentile");
}

static void test_consts(void)
{
    struct nv2a_ao_trig t;
    struct nv2a_ao_cfg c;
    struct nv2a_ao_consts k1, k3;
    nv2a_ao_reset(&t);
    nv2a_ao_cfg_default(&c);
    check(!c.on, "AO defaults off");
    t.fit.wref = 100.0f;
    nv2a_ao_consts_fill(&t, &c, 640, 480, 480, 1, &k1);
    nv2a_ao_consts_fill(&t, &c, 1920, 1440, 480, 3, &k3);
    check(fabsf(k3.p1[0] - 3.0f * k1.p1[0]) < 1e-4f, "radius scales with the render scale");
    check(fabsf(k1.p1[1] - 0.01f) < 1e-6f, "1 / wref");
    check(fabsf(k1.p2[2] / k1.p2[3] - 640.0f / 480.0f) < 1e-4f, "tan x follows the aspect");
}

/* ao_comp_fs's step in C (nv2a_ao_comp_px): the colour's factors multiply
 * up to the record's total, through any chain of top-ups. */
static void test_comp(void)
{
    float t, c, a;
    unsigned i, k, bad = 0, seed = 7;
    /* A later pass that wants less occlusion (0.8 after 0.5): the multiply
     * cannot brighten, so the pixel keeps 0.5, and the next top-up (0.6)
     * divides by the 0.5 applied, not by the 0.8 wanted (that left it at
     * 0.5 * 0.6 / 0.8, darker with each top-up). */
    c = nv2a_ao_comp_px(0.5f, 0.3f, 0, 1.0f, 0.0f, &t);
    a = nv2a_ao_comp_px(0.8f, 0.3f, 1, t, 0.3f, &t);
    c *= a;
    check(a == 1.0f && t == 0.5f, "comp: a lighter top-up leaves the pixel as it was");
    c *= nv2a_ao_comp_px(0.6f, 0.3f, 1, t, 0.3f, &t);
    check(fabsf(c - 0.5f) < 1e-6f && t == 0.5f, "comp: and the next one divides by what was applied");
    c *= nv2a_ao_comp_px(0.2f, 0.3f, 1, t, 0.3f, &t);
    check(fabsf(c - 0.2f) < 1e-6f && t == 0.2f, "comp: a darker one brings the pixel to it");
    a = nv2a_ao_comp_px(0.4f, 0.3f, 1, 0.0f, 0.3f, &t);
    check(a == 1.0f && t == 0.0f, "comp: a pixel at no light divides by nothing");
    a = nv2a_ao_comp_px(0.4f, 0.25f, 1, 0.5f, 0.3f, &t);
    check(a == 0.4f && t == 0.4f, "comp: new depth (drawn again) takes the map in full");
    a = nv2a_ao_comp_px(0.4f, 0.3f, 0, 0.5f, 0.3f, &t);
    check(a == 0.4f && t == 0.4f, "comp: a first pass takes the map in full");
    /* Random chains: some passes over new depth (the colour drawn again,
     * so it starts over at 1), the others over the same. */
    for (i = 0; i < 2000; i++) {
        float d = 0.5f;
        c = nv2a_ao_comp_px(0.3f + 0.7f * (float)(i % 97) / 96.0f, d, 0, 1.0f, 0.0f, &t);
        for (k = 0; k < 6; k++) {
            float pd = d, f;
            seed = seed * 1103515245u + 12345u;
            f = (float)((seed >> 8) & 1023u) / 1023.0f;
            if ((seed >> 20) % 4 == 0) {
                d = pd - 0.01f;
                c = 1.0f;
            }
            c *= nv2a_ao_comp_px(f, d, 1, t, pd, &t);
        }
        bad += fabsf(c - t) > 1e-5f;
    }
    check(bad == 0, "comp: the colour carries the record's total through top-ups");
}

/* A HUD in the middle of the frame, then the 3D again on some frames: the
 * memo fires at the HUD and the flip tops up the 3D drawn after it, so the
 * frame's last depth-writing draw always has a pass after it. */
static void test_hud_then_3d(void)
{
    struct nv2a_ao_trig t;
    unsigned f, i, bad = 0, extra = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 40; f++) {
        int more = f % 3 == 1, at = -2;
        for (i = 0; i < 70 + (more ? 20u : 0u); i++) {
            int zw = i < 60 || i >= 70;
            if (nv2a_ao_draw(&t, RT_3D, 640, 480, 1, zw, 0)) at = (int)i;
            if (zw) put_proj(&t, 2.0f + (float)(i * 53 % 900), 0.5f, 2000.0f, 0);
        }
        if (nv2a_ao_flip(&t, RT_3D)) at = -1;
        frame_done(&t);
        if (f < 4)
            continue;
        if (more)
            bad += at != -1 || g_passes != 2 || g_first != 1;
        else
            bad += at != 60 || g_passes != 1;
        extra += g_passes > 1;
    }
    check(bad == 0, "HUD then 3D: the HUD fires, the flip tops up the 3D after it");
    check(extra == 12, "HUD then 3D: a top-up only on the frames with 3D after the HUD");
}

static void test_dump_parse(void)
{
    char d[64];
    unsigned long from, n;
    nv2a_ao_dump_parse("D:/dumps/ao:3000:4", d, sizeof d, &from, &n);
    check(!strcmp(d, "D:/dumps/ao") && from == 3000 && n == 4, "ao_dump: drive letter kept");
    nv2a_ao_dump_parse("/tmp/ao:120", d, sizeof d, &from, &n);
    check(!strcmp(d, "/tmp/ao") && from == 120 && n == 300, "ao_dump: N defaults to 300");
    nv2a_ao_dump_parse("dump", d, sizeof d, &from, &n);
    check(!strcmp(d, "dump") && from == 0 && n == 300, "ao_dump: DIR only");
    nv2a_ao_dump_parse(NULL, d, sizeof d, &from, &n);
    check(!d[0], "ao_dump: unset");
    nv2a_ao_dump_parse("a:100", d, sizeof d, &from, &n);
    check(!strcmp(d, "a") && from == 100 && n == 300, "ao_dump: one-letter folder, not a drive");
    nv2a_ao_dump_parse("D:", d, sizeof d, &from, &n);
    check(!strcmp(d, "D:") && from == 0, "ao_dump: bare drive");
}

/* The backend frees the armed candidate mid-frame: neither the next draw
 * elsewhere nor the flip may hand it back. */
static void test_forget(void)
{
    struct nv2a_ao_trig t;
    const void *got;
    unsigned i;
    int stale = 0;
    nv2a_ao_reset(&t);
    frame_3d_hud(&t, 60, 10);
    frame_3d_hud(&t, 60, 10);
    for (i = 0; i < 60; i++) {
        nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
        put_proj(&t, 2.0f + (float)(i * 53 % 900), 0.5f, 2000.0f, 0);
    }
    check(t.cand == RT_3D && t.armed, "forget: candidate armed before the free");
    nv2a_ao_forget(&t, RT_SHADOW);
    check(t.cand == RT_3D, "forget: another target leaves it");
    check(t.prev_rt[0] == RT_3D, "forget: the last frame ended on the target");
    nv2a_ao_forget(&t, RT_3D);
    check(t.cand == NULL && !t.armed, "forget: freed candidate cleared");
    check(t.prev_rt[0] != RT_3D && t.prev_rt[1] != RT_3D,
          "forget: freed target no longer a last frame's end");
    got = nv2a_ao_draw(&t, RT_SHADOW, 512, 512, 1, 1, 0);
    stale |= got == RT_3D;
    check(got == NULL, "forget: a draw elsewhere fires nothing");
    got = nv2a_ao_flip(&t, RT_3D);
    stale |= got == RT_3D;
    check(got == NULL && !stale, "forget: the flip fires nothing");
    nv2a_ao_frame_end(&t, 640, 480);
}

/* A frame of 60 depth draws, a 2D draw, then a short chunk of tail depth
 * draws before the HUD. Returns t.late at the flip. */
static unsigned frame_late(struct nv2a_ao_trig *t, unsigned tail, char *line, size_t cap)
{
    unsigned i, late;
    for (i = 0; i < 60; i++) {
        nv2a_ao_draw(t, RT_3D, 640, 480, 1, 1, 0);
        put_proj(t, 2.0f + (float)(i * 53 % 900), 0.5f, 2000.0f, 0);
    }
    nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0);
    for (i = 0; i < tail; i++) {
        nv2a_ao_draw(t, RT_3D, 640, 480, 1, 1, 0);
        put_proj(t, 2.0f + (float)(i * 31 % 900), 0.5f, 2000.0f, 0);
    }
    for (i = 0; i < 10; i++)
        nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0);
    nv2a_ao_flip(t, RT_3D);
    late = t->late;
    if (line)
        nv2a_ao_frame_str(t, line, cap, NULL);
    frame_done(t);
    return late;
}

/* Depth draws after the frame's last pass are drawn with no occlusion: a
 * tail too short to re-arm (rearm_draws) gets no top-up, and the trace
 * counts it; a longer one is topped up and counts nothing. */
static void test_late(void)
{
    struct nv2a_ao_trig t;
    char line[400];
    unsigned i, l5, l20;
    nv2a_ao_reset(&t);
    for (i = 0; i < 3; i++)
        frame_late(&t, 0, NULL, 0);
    l5 = frame_late(&t, 5, line, sizeof line);
    check(l5 == 5, "late: a 5-draw tail after the pass is counted");
    check(strstr(line, " late=5 drop=0 fb=0 ev=") != NULL, "late: ao_frames shows late=5");
    check(t.late_frames == 1 && t.late_draws == 5, "late: totals count the frame");
    l20 = frame_late(&t, 20, NULL, 0);
    check(l20 == 0 && g_passes >= 2, "late: a 20-draw tail is topped up, none late");
    check(t.late == 0, "late: frame_end clears it");
}

/* The backend declines a pass (no readable depth of the target's size, a
 * record that no longer fits): a dropped first pass leaves the frame
 * unoccluded and the next event takes its place; a dropped top-up is not
 * counted, so the next one is the frame's second, not its third. */
static void test_dropped(void)
{
    struct nv2a_ao_trig t;
    char line[400];
    unsigned f, i, j;
    const void *got;
    int second = 0, third = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++)
        frame_chunks(&t, 1, &(int){0});
    /* One chunk per frame: the pass fires at the first event; drop it. */
    for (i = 0; i < 30; i++) {
        nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
        put_proj(&t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
    }
    got = nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0);
    check(got == RT_3D, "drop: the pass fires");
    nv2a_ao_dropped(&t);
    check(t.fired == 0 && t.fired_rt == NULL && t.drops == 1, "drop: a first pass undone");
    for (i = 0; i < 30; i++)
        nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
    got = nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0);
    check(got == RT_3D && t.fired == 1 && t.fired_at == 2, "drop: the next event is the first pass");
    nv2a_ao_frame_str(&t, line, sizeof line, NULL);
    check(strstr(line, " drop=1 fb=0 ev=") != NULL && t.ev_fired == 2u,
          "drop: ao_frames shows it, the dropped event unmarked");
    /* Two more chunks: the top-up of the first is dropped, the second's
     * runs as pass two. */
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 30; i++)
            nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
        got = nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0);
        if (!j && got) {
            nv2a_ao_dropped(&t);
            second = t.fired == 1;
        } else if (got) {
            third = t.fired == 2;
        }
    }
    check(second && third, "drop: a dropped top-up is not counted");
    check(t.drop_total == 2 && t.ev_fired == 0xAu, "drop: total, and the events that ran");
    frame_done(&t);
    check(t.drops == 0, "drop: frame_end clears the frame's count");
}

/* A decline that persists (no readable depth of that size for the whole
 * frame): one retry at the next event, then the frame goes without, so a
 * later depth chunk (a 3D element drawn after the HUD) never gets a pass
 * and the drops stay at two a frame. */
static void test_dropped_persistent(void)
{
    struct nv2a_ao_trig t;
    unsigned f, c, i, fires = 0;
    const void *got;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++)
        frame_chunks(&t, 1, &(int){0});
    for (f = 0; f < 3; f++) {
        for (c = 0; c < 4; c++) {
            for (i = 0; i < 30; i++) {
                nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
                put_proj(&t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
            }
            got = nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 0, 0);
            if (got) {
                fires++;
                nv2a_ao_dropped(&t);
            }
        }
        for (i = 0; i < 30; i++)
            nv2a_ao_draw(&t, RT_3D, 640, 480, 1, 1, 0);
        if (nv2a_ao_flip(&t, RT_3D)) {
            fires++;
            nv2a_ao_dropped(&t);
        }
        check(t.drops == 2 && t.gave_up && !t.fired, "persistent drop: one retry, then none");
        frame_done(&t);
    }
    check(fires == 6 && t.drop_total == 6, "persistent drop: two passes a frame, at most");
    check(!t.retried && !t.gave_up, "persistent drop: frame_end clears the retry");
}

/* Deck flips 1560-1780 (a Deck trace) */
static const char *const deck_intro[] = {
    "z548 z739 r1085 z1098",
    "z554 z756 r1102 z1115",
    "z554 z762 r1108 z1121",
    "z568 z778 r1132 z1145",
    "z568 z780 r1134 z1147",
    "z568 z780 r1134 z1147",
    "z605 z842 r1196 z1209",
    "z610 z841 z1193 z1208",
    "z610 z862 z1214 z1431 z1479 z1494",
    "z617 z879 z1231 z1448 z1495 z1510",
    "z617 z879 z1231 z1448 z1489 z1706 r1748 z1761",
    "z617 z879 z1231 z1448 z1488 z1705 r1747 z1760",
    "z617 z883 z1235 z1452 z1492 z1709 r1751 z1764",
    "z617 z892 z1244 z1461 z1501 z1718 r1760 z1773",
    "z618 z893 z1245 z1462 z1502 z1719 r1761 z1774",
    "z618 z894 z1246 z1463 z1503 z1720 r1762 z1775",
    "z637 z914 z1266 z1483 z1523 z1740 r1782 z1795",
    "z637 z921 z1273 z1490 r1532 z1545",
    "z637 z942 z1294 z1309", /* missed */
    "z637 z952 z1304 z1319",
    "z652 z971 z1323 z1540 z1580 z1595",
    "z652 z980 z1332 z1549 z1589 z1806 r1848 z1861",
    "z660 z1004 z1356 z1573 z1613 z1830 r1872 z1885",
    "z674 z1032 z1384 z1601 z1641 z1858 r1900 z1913",
    "z680 z1045 z1397 z1614 z1654 z1871 r1913 z1926",
    "z681 z1073 z1425 z1642 z1682 z1899 r1941 z1954",
    "z692 z1134 z1486 z1703 z1743 z1960 r2002 z2015",
    "z701 z1149 z1501 z1718 z1758 z1975 r2017 z2030",
    "z701 z1149 z1501 z1718 z1758 z1975 r2017 z2030",
    "z709 z1157 z1509 z1726 r1768 z1781",
    "z709 z1159 z1511 z1526", /* missed */
    "z716 z1171 z1523 z1538",
    "z716 z1173 z1525 z1742 z1782 z1797",
    "z716 z1175 z1519 z1736 z1776 z1993 r2035 z2048",
    "z716 z1177 z1521 z1738 z1778 z1995 r2037 z2050",
    "z716 z1195 z1539 z1756 z1796 z2013 r2055 z2068",
    "z720 z1199 z1543 z1760 z1800 z2017 r2059 z2072",
    "z758 z1241 z1585 z1802 z1842 z2059 r2101 z2114",
    "z758 z1241 z1585 z1802 z1842 z2059 r2101 z2114",
    "z762 z1272 z1616 z1833 z1873 z2090 r2132 z2145",
    "z766 z1281 z1625 z1842 z1882 z2099 r2141 z2154",
    "z777 z1317 z1661 z1878 r1920 z1933",
    "z782 z1323 z1667 z1682", /* missed */
    "z787 z1345 z1689 z1704",
    "z791 z1343 z1687 z1904 z1944 z1959",
    "z794 z1360 z1704 z1921 z1961 z2178 r2220 z2233",
    "z794 z1370 z1714 z1931 z1971 z2188 r2230 z2243",
    "z794 z1368 z1712 z1929 z1969 z2186 r2228 z2241",
    "z794 z1379 z1723 z1940 z1980 z2197 r2239 z2252",
    "z795 z1379 z1723 z1940 z1980 z2197 r2239 z2252",
    "z795 z1391 z1735 z1952 z1992 z2209 r2251 z2264",
    "z795 z1391 z1735 z1952 z1992 z2209 r2251 z2264",
    "z809 z1403 z1747 z1964 z2004 z2221 r2263 z2276",
    "z811 z1397 z1741 z1958 r2000 z2013",
    "z814 z1403 z1747 z1762", /* missed */
    "z814 z1411 z1755 z1770",
    "z815 z1412 z1756 z1973 z2013 z2028",
    "z814 z1418 z1762 z1979 z2019 z2236 r2278 z2291",
    "z814 z1435 z1779 z1996 z2036 z2253 r2295 z2308",
    "z814 z1453 z1797 z2014 z2054 z2271 r2313 z2326",
    "z815 z1456 z1800 z2017 z2057 z2274 r2316 z2329",
    "z815 z1431 z1775 z1992 z2032 z2249 r2291 z2304",
    "z815 z1416 z1760 z1977 z2017 z2234 r2276 z2289",
    "z815 z1548 z1892 z2109 z2149 z2366 r2408 z2421",
    "z815 z1548 z1611 z1955 z2172 z2212 z2429 r2471 z2484",
    "z815 z1562 z1625 z1969 z2186 r2228 z2241",
    "z827 z1574 z1637 z1981 z1996", /* missed */
    "z827 z1564 z1627 z1971 z1986",
    "z839 z1595 z1732 z2076 z2293 r2335 z2348",
    "z839 z1568 z1741 z2085 z2302 r2344 z2357",
    "z839 z1547 z1760 z2104 z2321 r2363 z2376",
    "z842 z1554 z1767 z2111 z2328 r2370 z2383",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2125", /* missed */
    "z841 z1553 z1766 z2110 z2125",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2110 z2327 r2369 z2382",
    "z841 z1553 z1766 z2118 z2133", /* missed */
    "z841 z1553 z1766 z2118 z2133",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z841 z1553 z1766 z2118 z2133", /* missed */
    "z841 z1553 z1766 z2118 z2133",
    "z841 z1553 z1766 z2118 z2335 r2377 z2390",
    "z752 z1229 z1581 z1798 z1838 z2055 r2097 z2110",
    "z752 z1229 z1581 z1798 r1840 z1853",
    "z752 z1229 z1581 z1596",
    "z752 z1229 z1581 z1596",
    "z752 z1229 z1581 z1798 z1838 z1853",
    "z752 z1229 z1581 z1798 z1838 z2055 r2097 z2110",
    "z752 z1229 z1581 z1798 z1838 z2055 r2097 z2110",
    "z752 z1229 z1581 z1798 z1838 z2055 r2097 z2110",
    "z752 z1229 z1581 z1798 z1838 z2055 r2097 z2110",
    "z752 z1227 z1579 z1796 z1836 z2053 r2095 z2108",
    "z752 z1227 z1579 z1796 z1836 z2053 r2095 z2108",
    "z752 z1227 z1579 z1796 z1836 z2053 r2095 z2108",
    "z753 z1228 z1580 z1797 z1837 z2054 r2096 z2109",
    "z752 z1227 z1579 z1796 r1838 z1851",
    "z752 z1227 z1579 z1594", /* missed */
    "z752 z1227 z1580 z1595",
    "z752 z1229 z1573 z1790 z1830 z1845",
    "z752 z1229 z1573 z1790 z1830 z2047 r2089 z2102",
    "z752 z1229 z1573 z1790 z1830 z2047 r2089 z2102",
    "z752 z1229 z1573 z1790 z1830 z2047 r2089 z2102",
    "z752 z1229 z1573 z1790 z1830 z2047 r2089 z2102",
    "z752 z1229 z1573 z1790 z1830 z2047 r2089 z2102",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 r1834 z1847",
    "z752 z1231 z1575 z1590", /* missed */
    "z752 z1231 z1575 z1590",
    "z752 z1231 z1575 z1792 z1832 z1847",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z752 z1231 z1575 z1792 z1832 z2049 r2091 z2104",
    "z736 z1215 z1559 z1776 r1818 z1831",
    "z738 z1193 z1537 z1552", /* missed */
    "z774 z1218 z1562 z1577",
    "z707 z1053 z1397 z1614 z1654 z1669",
    "z689 z1014 z1358 z1575 z1615 z1630",
    "z640 z963 z1307 z1524 z1564 z1781 r1823 z1836",
    "z612 z958 z1302 z1519 z1559 z1776 r1818 z1829",
    "z604 z921 z1265 z1482 z1522 z1739 r1781 z1792",
    "z605 z870 z1214 z1431 z1471 z1688 r1730 z1741",
    "z590 z837 z1181 z1398 z1438 z1655 r1697 z1708",
    "z590 z807 z1152 z1369 z1409 z1626 r1668 z1679",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 r1381 z1392",
    "z561 z778 z1122 z1135", /* missed */
    "z561 z778 z1122 z1135",
    "z561 z778 z1122 z1339 z1379 z1392",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 r1381 z1392",
    "z561 z778 z1122 z1135", /* missed */
    "z561 z778 z1122 z1135",
    "z561 z778 z1122 z1339 z1379 z1392",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 r1381 z1392",
    "z561 z778 z1122 z1135", /* missed */
    "z561 z778 z1122 z1135",
    "z561 z778 z1122 z1339 z1379 z1392",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 r1381 z1392",
    "z561 z778 z1122 z1135", /* missed */
    "z561 z778 z1122 z1135",
    "z561 z778 z1122 z1339 z1379 z1392",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 z1379 z1596 r1638 z1649",
    "z561 z778 z1122 z1339 r1381 z1392",
    "z561 z778 z1122 z1135", /* missed */
    "z561 z778 z1122 z1135",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
};
/* Deck flips 1780-1870 (a Deck trace) */
static const char *const deck_long9[] = {
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z580",
    "z226 z565 z582",
    "z226 z565 z582",
    "z226 z565 z583",
    "z226 z565 z583",
    "z226 z565 z583",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z585",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z584",
    "z226 z565 z583",
    "z226 z565 z583",
    "z226 z565 z583",
    "z564 z928 z1149 z1189 z1406 r1448 z1461",
    "z564 z928 z1150 z1369 z1590 z1630 z1847 r1889 z1902",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z564 z928 z1141 z1360 z1581 z1621 z1838 r1880 z1893",
    "z561 z778 z1015 z1252 z1469 z1509 z1726 r1768 z1779",
    "z561 z778 z1130 z1143", /* missed */
    "z554 z756 z1108 z1121",
    "z534 z734 z1086 z1303 z1343 z1356",
    "z534 z714 z1066 z1283 z1323 z1540 r1582 z1593",
    "z490 z653 z1005 z1222 z1262 z1479 r1521 z1532",
    "z482 z633 z985 z1202 z1242 z1459 r1501 z1512",
    "z466 z608 z960 z1177 z1217 z1434 r1476 z1487",
    "z460 z593 z945 z1162 z1202 z1419 r1461 z1472",
    "z459 z586 z938 z1155 z1195 z1412 r1454 z1465",
    "z430 z526 z1095 z1352 r1394 z1405",
    "z373 z480 r834 z844",
    "z359 z444 r798 z808",
    "z332 z380 r734 z744",
    "z329 z369 r723 z731",
    "z326 z354 r708 z716",
    "z294 z320 r674 z682",
    "z285 z311 r665 z673",
};
/* Deck flips 2380-2460 (a Deck trace) */
static const char *const deck_cave[] = {
    "z648 z909 z1028 z1147 z1526 z1743 r1785 z1794",
    "z626 z889 z1008 z1127 z1506 z1517", /* missed */
    "z626 z886 z1005 z1124 z1503 z1515",
    "z626 z886 z1005 z1124 z1503 z1720 z1760 z1772",
    "z632 z905 z1024 z1143 z1522 z1739 z1779 z1996 r2038 z2049",
    "z644 z917 z1036 z1155 z1534 z1751 z1791 z2008 r2050 z2061",
    "z644 z901 z1020 z1139 z1518 z1735 z1775 z1992 r2034 z2045",
    "z647 z896 z1015 z1134 z1513 z1730 z1770 z1987 r2029 z2040",
    "z649 z901 z1020 z1139 z1518 z1735 z1775 z1992 r2034 z2045",
    "z641 z891 z1010 z1129 z1508 z1725 z1765 z1982 r2024 z2035",
    "z655 z907 z1026 z1145 z1524 z1741 z1781 z1998 r2040 z2051",
    "z648 z898 z1017 z1136 z1515 z1732 z1772 z1989 r2031 z2042",
    "z648 z898 z1017 z1136 z1515 z1732 r1774 z1785",
    "z648 z898 z1017 z1136 z1515 z1528", /* missed */
    "z654 z904 z1023 z1142 z1521 z1534",
    "z654 z899 z1018 z1137 z1516 z1733 z1773 z1786",
    "z648 z893 z1012 z1131 z1511 z1728 z1768 z1985 r2027 z2038",
    "z634 z879 z998 z1117 z1496 z1713 z1753 z1970 r2012 z2023",
    "z634 z877 z996 z1115 z1494 z1711 z1751 z1968 r2010 z2021",
    "z634 z880 z999 z1118 z1497 z1714 z1754 z1971 r2013 z2024",
    "z634 z878 z997 z1116 z1495 z1712 z1752 z1969 r2011 z2022",
    "z634 z875 z994 z1113 z1492 z1709 z1749 z1966 r2008 z2019",
    "z634 z867 z986 z1105 z1484 z1701 z1741 z1958 r2000 z2011",
    "z631 z861 z980 z1099 z1478 z1695 z1735 z1952 r1994 z2005",
    "z634 z850 z969 z1088 z1467 z1684 r1726 z1737",
    "z636 z852 z971 z1090 z1469 z1482", /* missed */
    "z633 z841 z960 z1079 z1458 z1471",
    "z633 z839 z958 z1077 z1456 z1673 z1713 z1726",
    "z671 z877 z996 z1115 z1494 z1711 z1751 z1968 r2010 z2021",
    "z671 z866 z985 z1104 z1483 z1700 z1740 z1957 r1999 z2010",
    "z672 z861 z980 z1099 z1478 z1695 z1735 z1952 r1994 z2005",
    "z671 z855 z974 z1093 z1472 z1689 z1729 z1946 r1988 z1999",
    "z671 z855 z974 z1093 z1472 z1689 z1729 z1946 r1988 z1999",
    "z633 z817 z936 z1055 z1434 z1651 z1691 z1908 r1950 z1961",
    "z633 z817 z936 z1055 z1426 z1643 z1683 z1900 r1942 z1953",
    "z633 z817 z936 z1055 z1426 z1643 z1683 z1900 r1942 z1953",
    "z633 z819 z938 z1057 z1428 z1645 r1687 z1698",
    "z627 z818 z937 z1056 z1427 z1440", /* missed */
    "z593 z784 z903 z1022 z1393 z1406",
    "z575 z767 z886 z1005 z1376 z1593 z1633 z1646",
    "z575 z765 z884 z1003 z1382 z1599 z1639 z1856 r1898 z1909",
    "z571 z765 z884 z1003 z1382 z1599 z1639 z1856 r1898 z1909",
    "z567 z761 z880 z999 z1378 z1595 z1635 z1852 r1894 z1905",
    "z567 z781 z900 z1019 z1398 z1615 z1655 z1872 r1914 z1925",
    "z538 z749 z868 z987 z1366 z1583 z1623 z1840 r1882 z1893",
    "z538 z749 z868 z987 z1366 z1583 z1623 z1840 r1882 z1893",
    "z538 z749 z868 z987 z1366 z1583 z1623 z1840 r1882 z1893",
    "z538 z749 z868 z987 z1366 z1583 z1623 z1840 r1882 z1893",
    "z522 z733 z852 z971 z1350 z1567 r1609 z1620",
    "z522 z733 z852 z971 z1350 z1363", /* missed */
    "z522 z733 z852 z971 z1350 z1363",
    "z522 z730 z849 z968 z1347 z1564 z1604 z1617",
    "z504 z700 z819 z938 z1317 z1534 z1574 z1791 r1833 z1844",
    "z504 z680 z799 z918 z1297 z1514 z1554 z1771 r1813 z1824",
    "z504 z680 z799 z918 z1297 z1514 z1554 z1771 r1813 z1824",
    "z504 z680 z799 z918 z1297 z1514 z1554 z1771 r1813 z1824",
    "z504 z680 z799 z918 z1297 z1514 z1554 z1771 r1813 z1824",
    "z504 z680 z799 z1178 z1395 z1435 z1652 r1694 z1705",
    "z504 z680 z799 z1178 z1395 r1437 z1448",
    "z504 z677 z796 z1175 z1188", /* missed */
    "z504 z677 z796 z1175 z1188",
    "z504 z677 z796 z1175 z1392 z1432 z1445",
    "z498 z656 z775 z1154 z1371 z1411 z1628 r1670 z1681",
    "z512 z666 z785 z1164 z1381 z1421 z1638 r1680 z1691",
    "z515 z669 z788 z1167 z1384 z1424 z1641 r1683 z1694",
    "z508 z664 z783 z1162 z1379 z1419 z1636 r1678 z1689",
    "z489 z651 z770 z1149 z1366 z1406 z1623 r1665 z1676",
    "z519 z697 z816 z1195 z1412 z1452 z1669 r1711 z1722",
    "z521 z698 z817 z1196 z1413 z1453 z1670 r1712 z1723",
    "z538 z717 z836 z1215 z1432 z1472 z1689 r1731 z1742",
    "z536 z717 z836 z1207 z1424 r1466 z1477",
    "z521 z709 z828 z1199 z1212", /* missed */
    "z521 z709 z828 z1199 z1212",
    "z521 z711 z830 z1201 z1418 z1458 z1471",
    "z521 z711 z830 z1201 z1418 z1458 z1675 r1717 z1728",
    "z521 z709 z828 z1199 z1416 z1456 z1673 r1715 z1726",
    "z521 z709 z828 z1199 z1416 z1456 z1673 r1715 z1726",
    "z521 z709 z828 z1199 z1416 z1456 z1673 r1715 z1726",
    "z521 z709 z828 z1199 z1416 z1456 z1673 r1715 z1726",
    "z521 z709 z828 z1199 z1416 z1456 z1673 r1715 z1726",
    "z521 z709 z828 z1199 z1416 z1456 z1673 r1715 z1726",
};

/* The Deck's Stage 1 trace (RECOMP_TRACE=ao,ao_frames), replayed event for event. Each string
 * is one frame's events as the trace logs them: kind ('z' a draw with no
 * depth write, 'r' a draw on another target) and the frame's draw index it
 * came at. The scene's count cycles over 12 frames: 8 events (zzzzzzrz)
 * for 8 frames, then 6 (zzzzrz), 4 (zzzz), 4, 6. The 4 is 10 frames back
 * when it comes, past the memo's 8, and two short of a memo of 6; its kinds
 * have no 'r', so the last frame's tail ('rz') does not match it and the
 * depth-draw test is not asked. 68 of the trace's 74 missed frames are this
 * one. deck_long9: 9 events for 50 frames, then 4 (flip 1854). s_rp_snaps
 * and s_rp_early: the colour copies and early first passes of the last
 * replay, past its warm-up. */
static unsigned s_rp_snaps, s_rp_early;

/* One frame of a replay up to its flip (the caller runs frame_end): the
 * depth-writing draws up to each event's index, then the event, then five
 * draws of HUD. */
static void rp_frame(struct nv2a_ao_trig *t, const void *shown, const char *p)
{
    unsigned i = 0;
    while (*p) {
        char k = *p++;
        unsigned at = (unsigned)strtoul(p, (char **)&p, 10);
        while (*p == ' ') p++;
        for (; i < at; i++) {
            nv2a_ao_draw(t, shown, 640, 480, 1, 1, 0);
            nv2a_ao_snap(t);
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        if (k == 'r')
            nv2a_ao_draw(t, RT_SHADOW, 512, 512, 1, 1, 0);
        else
            nv2a_ao_draw(t, shown, 640, 480, 1, 0, 0);
        nv2a_ao_snap(t);
        i++;
    }
    for (unsigned j = 0; j < 5; j++)
        nv2a_ao_draw(t, shown, 640, 480, 1, 0, 0);
    nv2a_ao_flip(t, shown);
}

static void replay(const char *const *seq, unsigned n, unsigned *missed, unsigned *fb,
                   unsigned *passes)
{
    struct nv2a_ao_trig t;
    static int b[2];
    unsigned f;
    nv2a_ao_reset(&t);
    *missed = *fb = *passes = 0;
    s_rp_snaps = s_rp_early = 0;
    for (f = 0; f < n; f++) {
        uint64_t m0 = t.missed, s0 = t.snaps, e0 = t.earlies;
        rp_frame(&t, &b[f & 1], seq[f]);
        /* Test-only switch to print the replay frame by frame; no
         * recomp_env row, as nothing in the runtime reads it. */
        if (getenv("AO_REPLAY_TRACE"))
            printf("      f%u ord=%u want=%u at=%u fired=%u fb=%d ok=%d %s\n", f, t.ord, t.memo,
                   t.fired_at, t.fired, t.fallback, t.fit.ok, seq[f]);
        if (f >= 8) {
            *fb += (unsigned)t.fallback;
            *passes += t.fired;
        }
        nv2a_ao_frame_end(&t, 640, 480);
        if (f >= 8) {
            *missed += (unsigned)(t.missed - m0);
            s_rp_snaps += (unsigned)(t.snaps - s0);
            s_rp_early += (unsigned)(t.earlies - e0);
        }
    }
}

/* A shape is remembered for NV2A_AO_SHAPE_WIN frames with events, then
 * forgotten: the floor rises back to the count the scene draws now. */
static void test_shape_expires(void)
{
    struct nv2a_ao_trig t;
    static int b[2];
    unsigned f;
    nv2a_ao_reset(&t);
    for (f = 0; f < 10; f++) {
        rp_frame(&t, &b[f & 1], f == 5 ? "z30 z60 z90 z120" : "z30 z60 z90 z120 z150 z180");
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(t.floor == 4, "shape window: the one frame of 4 events sets the floor");
    for (; f < 5 + NV2A_AO_SHAPE_WIN; f++) {
        rp_frame(&t, &b[f & 1], "z30 z60 z90 z120 z150 z180");
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(t.floor == 4, "shape window: still remembered one frame short of the window");
    rp_frame(&t, &b[f & 1], "z30 z60 z90 z120 z150 z180");
    nv2a_ao_frame_end(&t, 640, 480);
    check(t.floor == 6, "shape window: forgotten after NV2A_AO_SHAPE_WIN frames");
}

/* A loading screen (no events, no samples) clears the shapes with the
 * history: the next scene's short frames are not matched to the old one's. */
static void test_loading_clears_shapes(void)
{
    struct nv2a_ao_trig t;
    static int b[2];
    unsigned f, i, any = 0;
    nv2a_ao_reset(&t);
    for (f = 0; f < 12; f++) {
        rp_frame(&t, &b[f & 1], f % 4 == 3 ? "z30 z60 z90" : "z30 z60 z90 z120 z150");
        nv2a_ao_frame_end(&t, 640, 480);
    }
    check(t.floor == 3, "loading: the scene's shapes are remembered");
    nv2a_ao_flip(&t, &b[0]);
    nv2a_ao_frame_end(&t, 640, 480);
    for (i = 0; i < NV2A_AO_EVENTS; i++)
        any |= t.shape[i].seen;
    check(!any && t.floor == 0 && t.nhist == 0, "loading: a frame with no events nor 3D clears the shapes");
}

/* One replay and its bounds, past the warm-up: no frame missed, and the
 * cost the fix may add capped (the fix's review). max_passes: the
 * passes before the fix (3afdaf07) plus 5%; at most 2.5 colour copies a
 * frame (2.33 to 2.37 when written, from 1 to 2 before) and max_fb
 * fallbacks. */
static void replay_bounded(const char *name, const char *const *seq, unsigned n,
                           unsigned max_passes, unsigned max_fb, const char *what)
{
    unsigned m, fb, p, k = n - 8;
    char msg[160];
    replay(seq, n, &m, &fb, &p);
    printf("    %s: %u frames, %u missed, %u fallbacks, %u passes, %u copies, %u early\n", name,
           k, m, fb, p, s_rp_snaps, s_rp_early);
    check(m == 0, what);
    snprintf(msg, sizeof msg, "%s: at most %u passes", name, max_passes);
    check(p <= max_passes, msg);
    snprintf(msg, sizeof msg, "%s: at most 2.5 colour copies a frame", name);
    check(2u * s_rp_snaps <= 5u * k, msg);
    snprintf(msg, sizeof msg, "%s: at most %u fallbacks", name, max_fb);
    check(fb <= max_fb, msg);
}

static void test_deck_stage1_cycle(void)
{
#define N(a) (unsigned)(sizeof a / sizeof a[0])
    /* passes before the fix: 669, 265, 159 */
    replay_bounded("intro", deck_intro, N(deck_intro), 702, 2,
                   "Deck Stage 1 fly-over (12-frame cycle): no frame missed");
    replay_bounded("cave", deck_cave, N(deck_cave), 278, 1,
                   "Deck Stage 1 cave (10, 8, 6 events): no frame missed");
    replay_bounded("long9", deck_long9, N(deck_long9), 167, 2,
                   "Deck Stage 1, 4 events after 50 frames of 9: not missed");
#undef N
}

int main(void)
{
    struct nv2a_ao_cfg c;
    nv2a_ao_cfg_default(&c);
    c.on = 1;
    nv2a_ao_cfg_set(&c);
    printf("nv2a_scene_ao\n");
    test_simple();
    test_chunks();
    test_sampled();
    test_hud_never_arms();
    test_shadow_and_flip();
    test_growing();
    test_cycle();
    test_one_in_k();
    test_deck_hub();
    test_topup_target();
    test_wander();
    test_lost_chunk();
    test_mac_hub();
    test_short_past_memo();
    test_short_tail_first();
    test_short_order_kind();
    test_short_offscreen();
    test_short_early_dropped();
    test_deck_stage1_cycle();
    test_shape_expires();
    test_loading_clears_shapes();
    test_fallback_edges();
    test_fallback_mask();
    test_jittered_chunks();
    test_scene_change();
    test_no_fit();
    test_fit_two_projections();
    test_wmode();
    test_consts();
    test_comp();
    test_hud_then_3d();
    test_dump_parse();
    test_forget();
    test_late();
    test_dropped();
    test_dropped_persistent();
    printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
