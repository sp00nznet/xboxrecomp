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
 *    several events; the occlusion runs at the last one, which is only
 *    known at the flip, so it runs first at the ordinal the previous
 *    frames left (the memo: the fewest events of the last 8 frames) and
 *    again at each later event of the same target and at its flip (a
 *    top-up: the backend occludes the pixels whose depth changed since its
 *    last pass and corrects the others for the depth drawn since). Early costs a
 *    pass, late a frame with no occlusion: no event follows the HUD. A
 *    game whose count cycles or wanders (N, N, N + 1 on the Deck;
 *    3, 2, 2 with slips at BLiNX 2's hub) so stays covered. A
 *    loading screen (a frame with no events and no 3D) clears the
 *    history. A frame with one event fewer than the memo (BLiNX 2's hub on
 *    the Mac: 4 events for 45 frames, then 3 for 30) is only
 *    known short at the flip, after its HUD: the backend copies the
 *    candidate's colour at the event one short of the memo, and the flip
 *    occludes the pixels that changed little since (nv2a_ao_snap,
 *    nv2a_ao_fb_keep), if that target is the one shown. "The first 2D
 *    draw after 3D" does not work: Burnout 3 draws its 3D offscreen and
 *    its HUD is not classified as 2D.
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

/* Why an event fired: the next draw went to another target, did not write
 * depth, had no depth buffer, sampled the candidate; or the flip. */
enum { NV2A_AO_WHY_RT = 1, NV2A_AO_WHY_NOZ, NV2A_AO_WHY_NODEPTH, NV2A_AO_WHY_SAMPLE,
       NV2A_AO_WHY_FLIP };
#define NV2A_AO_EVENTS 16
/* Event counts of the last frames that had any, for the memo
 * (nv2a_ao_frame_end). */
#define NV2A_AO_HIST 16

/* The trigger and the frame's samples. Opaque target pointers: the
 * backend's own render targets. */
struct nv2a_ao_trig {
    uint32_t    disp_w, disp_h;   /* the last shown target's guest size */
    const void *cand;
    unsigned    zdraws, since;
    int         armed;
    unsigned    fired;            /* passes this frame: above 1, a top-up */
    const void *fired_rt;         /* the target the first pass occluded */
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
    /* Totals for the trace: frames, frames that fired, fit refusals,
     * frames with none, top-up passes. */
    uint64_t    frames, fired_frames, no_fit, missed, topups;
    /* Depth-writing draws on the occluded target after its last pass this
     * frame (late: drawn with no occlusion, a part of the picture that
     * comes and goes), and the totals: frames that ended with any, and
     * their draws. */
    unsigned    late;
    uint64_t    late_frames, late_draws;
    /* Passes the trigger fired and the backend declined (nv2a_ao_dropped)
     * this frame, and the total. retried: a first pass was
     * declined and the next event takes its place; gave_up: that one was
     * declined too, so the frame runs no first pass (one retry a frame). */
    unsigned    drops;
    uint64_t    drop_total;
    int         retried, gave_up;
    /* This frame's events for the ao_frames trace (nv2a_ao_frame_str):
     * why each fired (NV2A_AO_WHY_*), the candidate's depth-writing draws
     * by then, the ordinal the first pass ran at (0: none) and a bit per
     * event that ran a pass. */
    uint8_t     ev_why[NV2A_AO_EVENTS];
    uint16_t    ev_zd[NV2A_AO_EVENTS];
    const void *ev_rt[NV2A_AO_EVENTS];
    unsigned    fired_at;
    uint16_t    ev_fired;
    uint8_t     hist[NV2A_AO_HIST];  /* ring of event counts, newest at nhist - 1 */
    unsigned    nhist;
    unsigned    memo;             /* want as frame_end set it (the flip moves want) */
    /* The fallback: at the event one short of the memo the
     * backend copies the candidate's colour (nv2a_ao_snap); a frame that
     * then ends with no pass gets one at the flip that occludes the
     * pixels that changed little since that copy (fallback 1), so the
     * HUD and any 2D drawn since are left alone (nv2a_ao_fb_keep).
     * snap_req: a copy is due now. */
    const void *snap_rt;
    int         snap_req;
    int         fallback;
    uint64_t    fallbacks;
};

void nv2a_ao_reset(struct nv2a_ao_trig *t);

/* The backend frees or evicts rt: if it is the candidate, drop it unarmed,
 * so the trigger never hands back a freed target. */
void nv2a_ao_forget(struct nv2a_ao_trig *t, const void *rt);

/* Before a draw is encoded. rt and its guest size, whether it has a depth
 * buffer attached, whether this draw writes depth (depth test and mask on)
 * and whether one of its stages samples the current candidate. Returns the
 * target to occlude now, before this draw, or NULL; with t->fired above 1
 * the pass is a top-up of the same target (the backend kept the depth and
 * occlusion of its last pass to tell what changed). */
const void *nv2a_ao_draw(struct nv2a_ao_trig *t, const void *rt, uint32_t w, uint32_t h,
                         int has_depth, int zwrite, int samples_cand);

/* The backend did not run the pass nv2a_ao_draw or nv2a_ao_flip just
 * returned (no readable depth of the target's size, a top-up whose record
 * no longer fits): it is not counted as run, so the next pass knows what
 * the target carries, and a dropped first pass waits for the next event,
 * once a frame (t->gave_up). */
void nv2a_ao_dropped(struct nv2a_ao_trig *t);

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
 * armed candidate that has not fired, a top-up as nv2a_ao_draw's, or with
 * nothing armed nor fired the copied target as a fallback, only if it is
 * shown, the target the flip presents), or NULL. Then the backend calls
 * nv2a_ao_frame_end, which fits this frame's samples for the next one and
 * resets the per-frame state; shown_w x shown_h is the shown target's
 * guest size (0 x 0: none). */
const void *nv2a_ao_flip(struct nv2a_ao_trig *t, const void *shown);
void nv2a_ao_frame_end(struct nv2a_ao_trig *t, uint32_t shown_w, uint32_t shown_h);

/* After nv2a_ao_draw: the target whose colour the backend copies now,
 * before this draw (the event one short of the memo), or NULL. The flip
 * may hand it back as a fallback pass (t->fallback): occlude the pixels
 * whose colour changed little since that copy (nv2a_ao_fb_keep). A
 * backend that cannot copy declines that pass (nv2a_ao_dropped). */
const void *nv2a_ao_snap(struct nv2a_ao_trig *t);

/* The ao_frames trace's body for the frame between nv2a_ao_flip and
 * nv2a_ao_frame_end: "ord=5 want=5 at=5 ok=1 conf=1.000 ns=256 late=0
 * drop=0 fb=0 ev=z16,z24,r40,f0" (late, drop and fb: t->late, t->drops,
 * t->fallback), one letter per event (r target, z no
 * depth write, d no depth buffer, s sampled, f flip) and the candidate's
 * depth-writing draws by then; a '*' marks the one occluded. id(rt)
 * names a target (the backend's guest address), or NULL for the
 * pointer's low bits. */
void nv2a_ao_frame_str(const struct nv2a_ao_trig *t, char *buf, size_t cap,
                       uint32_t (*id)(const void *rt));

/* The two passes' constants: float4 only, so the MSL and HLSL layouts are
 * the C one. p0 = (a, b, wmode, zmax); p1 = (radius in host pixels,
 * 1 / wref, fade_lo, fade_hi); p2 = (host w, host h, tan x, tan y) of the
 * assumed 60 degree vertical field; p3 = (strength, bias, view, mode):
 * mode 0 a first pass, 1 a top-up, 2 a fallback. */
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

/* One pixel of ao_comp_fs after its filter (nv2a_scene_ao_shaders.h; the
 * shaders do the same, this is for the tests): filt is this pass's
 * occlusion, d the pixel's depth now, top a top-up outside ao_view, and
 * (prev_t, prev_d) the pixel's record from the frame's last pass. Returns
 * the factor the colour is multiplied by and sets *t to the new record's
 * total, the occlusion the pixel now carries. */
float nv2a_ao_comp_px(float filt, float d, int top, float prev_t, float prev_d, float *t);

/* ao_comp_fs's fallback mask in C (the shaders do the same, this is for
 * the tests): how much of the occlusion a fallback pass applies to a pixel
 * whose colour changed by change (the largest channel's, 0..1) between the
 * copy at the event before and the flip. 1 up to NV2A_AO_FB_LO (a glow
 * drawn over the scene), 0 from NV2A_AO_FB_HI (the HUD), linear between. */
#define NV2A_AO_FB_LO 0.06f
#define NV2A_AO_FB_HI 0.20f
float nv2a_ao_fb_keep(float change);

/* The ao_dump value DIR[:FROM[:N]] into dir (cap bytes), *from and *n
 * (default 300). A drive letter (D:/x) stays in DIR: the PC's dumps go to
 * D:, and the first colon there is not the separator. */
void nv2a_ao_dump_parse(const char *v, char *dir, size_t cap, unsigned long *from,
                        unsigned long *n);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_SCENE_AO_H */
