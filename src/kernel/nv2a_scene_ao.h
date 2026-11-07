/*
 * nv2a_scene_ao.h - the scene stage: ambient occlusion before the HUD.
 *
 * An opt-in enhancement (render.ao, off by default; after the post
 * effects spike). The present shader chain sees the final colour
 * after the HUD, with no depth: a depth effect has to run mid-frame, on the
 * target the 3D was drawn into, between the last 3D draw and the first
 * HUD draw, and on both GPU backends with no per-game code. This file is
 * the part both backends share and nothing in it touches a GPU, so
 * tests/nv2a_scene_ao drives it alone:
 *
 *  - The trigger: which target holds the frame's 3D and when it is done.
 *    The candidate is a target the size of the last one shown, with a depth
 *    buffer, that took depth-writing draws. Once it has taken arm_draws of
 *    them it is armed; an armed candidate fires an event when the next draw
 *    goes to another target, stops writing depth or samples it (a blur or
 *    glow pass reading the 3D), and at the flip. After an event it re-arms
 *    after rearm_draws more depth-writing draws. A frame drawn in chunks
 *    (BLiNX 2) or one whose world is split by a lone 2D draw (JSRF) has
 *    several events; the occlusion runs at the last one, whose ordinal the
 *    previous frame left (the memo), since the last is only known at the
 *    flip. "The first 2D draw after 3D" does not work: Burnout 3 draws its
 *    3D offscreen and its HUD is not classified as 2D.
 *  - The depth fit: the buffer stores z / zmax (oPos.z after the XDK
 *    epilogue's divide), d = A + B / w. FOV and planes are unknown, so the
 *    vertex programs' own output is sampled (the backend runs a few
 *    vertices of depth-writing draws through nv2a_vsh_run) and fitted by
 *    RANSAC, since a frame can draw with two projections (JSRF: the least
 *    squares fit came out with a negative far plane). A W-buffer batch
 *    (Z_PERSPECTIVE, Quantum Redshift) stores w / zmax: w is the depth
 *    times zmax and needs no fit. A poor fit (menus: JSRF's inlier share
 *    was 0.23) turns the effect off for the frame.
 *  - The constants of the two passes (struct nv2a_ao_consts), the same
 *    for the MSL and the HLSL (nv2a_scene_ao_shaders.h).
 *
 * The occlusion itself is scale-invariant: every distance is taken against
 * the pixel's own depth, so the fit needs A only up to B's scale, and the
 * radius is a screen-constant guest-pixel count times the render scale.
 */
#ifndef NV2A_SCENE_AO_H
#define NV2A_SCENE_AO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* render.ao_* (nv2a_ao_cfg_set): what the player and the game set. */
struct nv2a_ao_cfg {
    int      on;            /* render.ao: 0 = stock */
    float    strength;      /* 0..2, how dark full occlusion gets (default 1.5: the 3D of these games is low in detail, so a light touch reads as nothing) */
    float    radius;        /* guest pixels at 480 lines (default 20) */
    float    fade_lo;       /* fade starts at this share of the scene depth */
    float    fade_hi;       /* and is complete here */
    unsigned arm_draws;     /* depth-writing draws before a candidate arms */
    unsigned rearm_draws;   /* and after an event, before it arms again */
    float    min_conf;      /* the fit's inlier share below which AO is off */
    int      view;          /* debug: 1 shows the occlusion map in place of the scene */
};

/* The defaults above, render.ao off. */
void nv2a_ao_cfg_default(struct nv2a_ao_cfg *c);
/* The process-wide configuration both backends read at each flip. */
void nv2a_ao_cfg_set(const struct nv2a_ao_cfg *c);
const struct nv2a_ao_cfg *nv2a_ao_cfg(void);

/* One frame's depth model. */
struct nv2a_ao_fit {
    int   ok;       /* usable: the effect may run */
    int   wmode;    /* 1: W-buffer, w = D * zmax */
    float a, b;     /* Z: d = a + b / w */
    float conf;     /* inlier share, 0..1 */
    float zmax;     /* the zeta surface's (W mode) */
    float wref;     /* scene depth (90th percentile of sampled w), smoothed */
};

/* RANSAC over n samples (d, 1/w): fills fit's a, b, conf and ok (conf at
 * least min_conf, b < 0, a > 0, at least NV2A_AO_MIN_SAMPLES samples).
 * Deterministic for given samples (its own fixed seed). */
#define NV2A_AO_SAMPLES 256
#define NV2A_AO_MIN_SAMPLES 24
int nv2a_ao_fit_z(const float *d, const float *iw, unsigned n, float min_conf,
                  struct nv2a_ao_fit *fit);

/* The trigger and the frame's samples. Opaque target pointers: the
 * backend's own render targets. */
struct nv2a_ao_trig {
    uint32_t    disp_w, disp_h;   /* the last shown target's guest size */
    const void *cand;
    unsigned    zdraws, since;
    int         armed, fired;
    unsigned    ord, want;        /* this frame's events; the memo */
    /* Samples of this frame: d = z / zmax and 1 / w (Z), w (W). */
    float       sd[NV2A_AO_SAMPLES], siw[NV2A_AO_SAMPLES], sw[NV2A_AO_SAMPLES];
    uint8_t     sm[NV2A_AO_SAMPLES];  /* the sample's draw was W-buffered */
    unsigned    ns, seen;
    int         cur_disp, cur_wmode;  /* the last nv2a_ao_draw: sampling allowed */
    unsigned    wdraws, zdraws_all;
    float       zmax;
    uint32_t    rng;
    struct nv2a_ao_fit fit;       /* the previous frame's, used by this one */
    /* Totals for the trace: frames, frames that fired, fit refusals. */
    uint64_t    frames, fired_frames, no_fit, missed;
};

void nv2a_ao_reset(struct nv2a_ao_trig *t);

/* The backend frees or evicts rt: if it is the candidate, drop it unarmed,
 * so the trigger never hands back a freed target. */
void nv2a_ao_forget(struct nv2a_ao_trig *t, const void *rt);

/* Before a draw is encoded. rt and its guest size, whether it has a depth
 * buffer attached, whether this draw writes depth (depth test and mask on)
 * and whether one of its stages samples the current candidate. Returns the
 * target to occlude now, before this draw, or NULL. */
const void *nv2a_ao_draw(struct nv2a_ao_trig *t, const void *rt, uint32_t w, uint32_t h,
                         int has_depth, int zwrite, int samples_cand);

/* A depth-writing draw on a display-sized target: the sample slot its
 * vertex should fill, or -1 (a reservoir of NV2A_AO_SAMPLES per frame, so
 * a busy frame runs the interpreter on at most that many vertices). wmode is
 * the draw's W-buffer state, counted either way. */
int nv2a_ao_sample_slot(struct nv2a_ao_trig *t, int wmode);
/* The candidate test both the trigger and the sampler use: w x h has the
 * shown target's aspect and at least three quarters of its width (a shadow
 * map is square, a reflection pass small), or, before anything was shown,
 * is at least 320 wide. */
int nv2a_ao_display_like(const struct nv2a_ao_trig *t, uint32_t w, uint32_t h);
/* Fill it: oPos z and w as the vertex program wrote them, zmax of the zeta
 * surface. A sample with w <= 0 or not finite is dropped. */
void nv2a_ao_sample_put(struct nv2a_ao_trig *t, int slot, float z, float w, float zmax);

/* At the flip, before the frame is shown: the target to occlude now (an
 * armed candidate whose event is the memo's), or NULL. Then the backend
 * calls nv2a_ao_frame_end, which fits this frame's samples for the next one
 * and resets the per-frame state; shown_w x shown_h is the shown target's
 * guest size (0 x 0: none). */
const void *nv2a_ao_flip(struct nv2a_ao_trig *t);
void nv2a_ao_frame_end(struct nv2a_ao_trig *t, uint32_t shown_w, uint32_t shown_h);

/* The two passes' constants: float4 only, so the MSL and HLSL layouts are
 * the C one. p0 = (a, b, wmode, zmax); p1 = (radius in host pixels,
 * 1 / wref, fade_lo, fade_hi); p2 = (host w, host h, tan x, tan y) of the
 * assumed 60 degree vertical field; p3 = (strength, bias, view, 0). */
struct nv2a_ao_consts {
    float p0[4];
    float p1[4];
    float p2[4];
    float p3[4];
};

/* Fill for a host hw x hh target at render scale n (guest h lines). */
void nv2a_ao_consts_fill(const struct nv2a_ao_trig *t, const struct nv2a_ao_cfg *c,
                         uint32_t hw, uint32_t hh, uint32_t guest_h, unsigned n,
                         struct nv2a_ao_consts *o);

/* The ao_dump value DIR[:FROM[:N]] into dir (cap bytes), *from and *n
 * (default 300). A drive letter (D:/x) stays in DIR: the PC's dumps go to
 * D:, and the first colon there is not the separator. */
void nv2a_ao_dump_parse(const char *v, char *dir, size_t cap, unsigned long *from,
                        unsigned long *n);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_SCENE_AO_H */
