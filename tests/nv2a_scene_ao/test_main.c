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

/* A frame: 3D (n draws), then HUD (h non-depth draws). Returns the index
 * of the draw that fired (n = first HUD draw), -1 for the flip, -2 none. */
static int frame_3d_hud(struct nv2a_ao_trig *t, unsigned n, unsigned h)
{
    int at = -2;
    unsigned i;
    for (i = 0; i < n + h; i++) {
        int zw = i < n;
        if (nv2a_ao_draw(t, RT_3D, 640, 480, 1, zw, 0) && at == -2) at = (int)i;
        if (zw) put_proj(t, 2.0f + (float)(i * 53 % 900), 0.5f, 2000.0f, 0);
    }
    if (nv2a_ao_flip(t) && at == -2) at = -1;
    nv2a_ao_frame_end(t, 640, 480);
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
        if (nv2a_ao_flip(&t) && at == -2) a1 = -1;
        nv2a_ao_frame_end(&t, 640, 480);
    }
    a2 = frame_3d_hud(&t, 60, 10);
    a3 = frame_3d_hud(&t, 60, 10);
    check(a1 == 30, "a new early event fires on the stale memo (one frame)");
    check(a2 == -2 && t.missed == 1, "fewer events than the memo: one frame missed, counted");
    check(a3 == 60, "then back on the memo");
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
    test_no_fit();
    test_fit_two_projections();
    test_wmode();
    test_consts();
    test_dump_parse();
    test_forget();
    printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
