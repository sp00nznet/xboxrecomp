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
 * the ao_dump parse and a freed candidate (never handed back).
 */
#include "nv2a_scene_ao.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

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
    nv2a_ao_frame_end(t, 640, 480);
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
    if (nv2a_ao_flip(t)) at = -1;
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
        if (nv2a_ao_flip(&t) && at == -2) at = -1;
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
        if (nv2a_ao_flip(&t) && at == -2) at = -1;
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
        fired += nv2a_ao_flip(&u) != NULL;
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
        if (nv2a_ao_flip(&t) && at == -2) at = -1;
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
        if (nv2a_ao_flip(&t)) a1 = a1 == 30 ? -1 : -3;
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
    unsigned c, j;
    for (c = 0; c < k; c++) {
        for (j = 0; j < 30; j++, i++) {
            if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 1, 0)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        for (j = 0; j < 2; j++, i++)
            if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0)) at = i;
    }
    for (j = 0; j < 5; j++, i++)
        if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0)) at = i;
    if (nv2a_ao_flip(t)) at = -1;
    frame_done(t);
    return at;
}

static int frame_sizes(struct nv2a_ao_trig *t, const unsigned *sz, unsigned k, int *last);

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
            nv2a_ao_flip(&t);
            frame_done(&t);
            continue;
        }
        /* Fired on RT_3D at its event; then 40 depth draws on B. */
        for (i = 0; i < 40; i++)
            got |= nv2a_ao_draw(&t, (const void *)&B, 640, 480, 1, 1, 0) != NULL;
        got |= nv2a_ao_draw(&t, (const void *)&B, 640, 480, 1, 0, 0) != NULL;
        got |= nv2a_ao_flip(&t) != NULL;
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
    got |= nv2a_ao_flip(&t) != NULL;
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
    unsigned c, j;
    for (c = 0; c <= k; c++) {
        for (j = 0; j < (c < k ? 30u : 12u); j++, i++) {
            if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 1, 0)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        for (j = 0; j < 2; j++, i++)
            if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0)) at = i;
    }
    for (j = 0; j < 5; j++, i++)
        if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0)) at = i;
    if (nv2a_ao_flip(t)) at = -1;
    frame_done(t);
    return at;
}

/* A frame that loses a chunk now and then (the PC's misses): one
 * event short of the memo, an event that looks like the memo frame's last
 * one (12 depth draws after a chunk) fires. Like chunks with no distinct
 * tail never fire early that way (test_chunks, test_cycle). */
static void test_lost_chunk(void)
{
    struct nv2a_ao_trig t;
    unsigned f, bad = 0;
    uint64_t missed;
    nv2a_ao_reset(&t);
    for (f = 0; f < 4; f++) {
        int last;
        frame_tail(&t, 3, &last);
    }
    missed = t.missed;
    for (f = 0; f < 20; f++) {
        int last, at = frame_tail(&t, f % 5 == 4 ? 2 : 3, &last);
        if (at != last) bad++;
    }
    check(t.missed == missed, "a frame one chunk short is not missed");
    check(bad == 0, "and the frames after a short one fire early and are topped up");
}

/* Frames of chunks of the given sizes, each ended by 2 non-depth draws,
 * then a HUD of 5. Returns as frame_chunks. */
static int frame_sizes(struct nv2a_ao_trig *t, const unsigned *sz, unsigned k, int *last)
{
    int at = -2, i = 0;
    unsigned c, j;
    for (c = 0; c < k; c++) {
        for (j = 0; j < sz[c]; j++, i++) {
            if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 1, 0)) at = i;
            put_proj(t, 3.0f + (float)(i * 71 % 700), 0.5f, 2000.0f, 0);
        }
        *last = i;
        for (j = 0; j < 2; j++, i++)
            if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0)) at = i;
    }
    for (j = 0; j < 5; j++, i++)
        if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, 0, 0)) at = i;
    if (nv2a_ao_flip(t)) at = -1;
    frame_done(t);
    return at;
}

/* A steady count whose last two chunks are close in size and jitter by a
 * few draws (review of the memo fix): the event before the last falls in the
 * tail's band now and then, and must not fire a chunk early. */
static void test_tail_jitter(void)
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
    nv2a_ao_flip(&t);
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
        fired += nv2a_ao_flip(&t) != NULL;
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
        nv2a_ao_flip(&t);
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
        if (nv2a_ao_flip(&t)) at = -1;
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
    nv2a_ao_forget(&t, RT_3D);
    check(t.cand == NULL && !t.armed, "forget: freed candidate cleared");
    got = nv2a_ao_draw(&t, RT_SHADOW, 512, 512, 1, 1, 0);
    stale |= got == RT_3D;
    check(got == NULL, "forget: a draw elsewhere fires nothing");
    got = nv2a_ao_flip(&t);
    stale |= got == RT_3D;
    check(got == NULL && !stale, "forget: the flip fires nothing");
    nv2a_ao_frame_end(&t, 640, 480);
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
    test_tail_jitter();
    test_scene_change();
    test_no_fit();
    test_fit_two_projections();
    test_wmode();
    test_consts();
    test_comp();
    test_hud_then_3d();
    test_dump_parse();
    test_forget();
    printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
