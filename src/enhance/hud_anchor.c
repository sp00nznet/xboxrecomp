/*
 * hud_anchor.c - the HUD layer (hud_anchor.h; openspec change hud-anchors,
 * design A1 to A15).
 *
 * Every rule runs in guest pixels of the canvas's target: the table's x
 * values are converted from canvas units on the way in and back on the way
 * out, so a game whose HUD units are not pixels (or map onto a smaller
 * view of the target) gets the same window, offsets and stretch as one
 * whose units are. The window is the target's 4:3 columns, because the
 * backends clip pretransformed 2D to them (H6), whatever the view.
 */
#include "hud_anchor.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend_common.h"
#include "recomp_cfg.h"
#include "recomp_env.h"

#define LOGP "[hud] "

#if defined(_MSC_VER)
#define HUD_TLS __declspec(thread)
#else
#define HUD_TLS _Thread_local
#endif

#define HUD_NAME_MAX   48
#define HUD_DIM_MAX    4096.0f
#define HUD_MARGIN_MAX 64
#define HUD_BBOX_MAX   4096u   /* vertices a capture's bbox reads */

enum { SRC_FIXED = 0, SRC_MEMORY, SRC_TARGET };

struct hud_group {
    char name[HUD_NAME_MAX];
    int kind;
    int implicit;               /* a site's own group: no shared reference */
    int has_ref, has_park;
    int full_stretch;           /* full = "stretch": full-width primitives mapped wide */
    float ref_x, park;          /* canvas units */
    uint64_t ref;               /* proportional: epoch << 32 | float bits */
};

struct hud_site {
    uint32_t ret;
    int group;
    char element[HUD_NAME_MAX];
    int last_dx;                /* the trace's last [hud.site] dx, INT_MIN */
};

struct hud_canvas {
    float w, h;
    int top_left, y_up;
    int has_view;
    float view[4];
    int target_frame;
    uint32_t tw, th;            /* fixed target */
    int source;
    uint32_t w_va, h_va, view_va;
    int has_view_va, type_u32;
    float mem_scale;
};

/* What hud_layer_apply reads: one guest frame's canvas and target. */
struct hud_snap {
    float w, h, vx, vy, vw, vh;
    uint32_t tw, th, pad;
    int top_left;
};

static struct hud_canvas s_cv;
static uint32_t s_stride, s_x_off, s_prim;
static int s_u_off;
static struct hud_group *s_groups;
static int s_ngroups;
static struct hud_site *s_sites;
static int s_nsites;
static int s_nwrappers;
static int s_table;             /* atomic: a table is registered */
static int s_on;                /* atomic: the active flag (mod or capture) */
static int s_mod;               /* atomic: its mod part, which alone changes vertices */
static int s_cap;               /* atomic: a capture runs (hud_layer_set_capture) */
static int s_table_cap;         /* atomic: the table came from hud_capture_table, not a mod */
static hud_record_fn s_rec;     /* atomic: the capture's recorder */
static struct hud_draw s_draws[HUD_DRAWS_MAX];
static int s_ndraws;
static const uintptr_t *s_memp;
static uint32_t s_epoch = 1;    /* atomic */
static unsigned s_aw, s_ah;     /* atomic: display.hud_aspect, 0:0 = display */
static int s_margin;            /* atomic: display.hud_margin */

/* The snapshot, under a sequence count: hud_layer_frame writes it on the
 * guest's frame thread, apply reads it on any guest thread. */
static struct hud_snap s_snap;
static uint32_t s_snap_seq;

/* hud_layer_frame's own state, for its change lines. */
static struct hud_snap s_shown;
static char s_shown_why[64];
static int s_shown_valid;
static int s_lift_shown = -1, s_lift_pad = -1, s_lift_hud = -1;

static HUD_TLS struct {
    uint32_t ret, esp, epoch;
} t_chain[HUD_CHAIN_MAX];
static HUD_TLS unsigned t_nchain;

/* The XBOX_HUD_RESTORE saves, innermost last: a RESTORE call can reach
 * another one below it (a queue's add calling itself, or a hooked helper),
 * and the outer one's post must still find its own. */
static HUD_TLS struct {
    uint32_t esp, va, n, epoch;
    float x[HUD_RESTORE_MAX], u[HUD_RESTORE_MAX];
} t_save[HUD_SAVE_MAX];
static HUD_TLS unsigned t_nsave;

/* ---- trace ------------------------------------------------------------- */

static int trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_HUD_TRACE) != NULL;
    return on;
}

/* Once per key: an open-addressed set; full, it stops saying anything. */
enum { SEEN = 1024 };
static uint64_t s_seen[SEEN];

static int seen_first(uint32_t what, uint32_t ret)
{
    uint64_t key = ((uint64_t)what << 32 | ret) + 1u;
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 54) & (SEEN - 1u);
    unsigned probes;

    for (probes = 0; probes < SEEN / 2u; probes++) {
        uint64_t cur = __atomic_load_n(&s_seen[h], __ATOMIC_RELAXED);
        if (cur == key)
            return 0;
        if (!cur) {
            uint64_t zero = 0;
            if (__atomic_compare_exchange_n(&s_seen[h], &zero, key, 0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
                return 1;
            if (zero == key)
                return 0;
        }
        h = (h + 1u) & (SEEN - 1u);
    }
    return 0;
}

enum { SEEN_UNLISTED = 1, SEEN_SKEW, SEEN_SKIP_PRIM, SEEN_SKIP_CAP, SEEN_DEEP, SEEN_NEST };

static const char *kind_name(int k)
{
    static const char *const names[] = { "centre", "left", "right", "proportional", "world",
                                         "stretch" };
    return k >= 0 && k <= XBOX_HUD_STRETCH ? names[k] : "?";
}

static int kind_parse(const char *s)
{
    if (!s)
        return -1;
    if (!strcmp(s, "centre") || !strcmp(s, "center"))
        return XBOX_HUD_CENTRE;
    if (!strcmp(s, "left"))
        return XBOX_HUD_LEFT;
    if (!strcmp(s, "right"))
        return XBOX_HUD_RIGHT;
    if (!strcmp(s, "proportional"))
        return XBOX_HUD_PROPORTIONAL;
    if (!strcmp(s, "world"))
        return XBOX_HUD_WORLD;
    if (!strcmp(s, "stretch"))
        return XBOX_HUD_STRETCH;
    return -1;
}

/* ---- guest memory -------------------------------------------------------- */

static uint8_t *mem_base(void)
{
    return s_memp ? (uint8_t *)__atomic_load_n(s_memp, __ATOMIC_ACQUIRE) : NULL;
}

static uint32_t rd32(uint8_t *b, uint32_t va)
{
    uint32_t v;
    memcpy(&v, b + va, 4);
    return v;
}

static float rdf(uint8_t *b, uint32_t va)
{
    float v;
    memcpy(&v, b + va, 4);
    return v;
}

static void wrf(uint8_t *b, uint32_t va, float v)
{
    memcpy(b + va, &v, 4);
}

/* ---- keys ---------------------------------------------------------------- */

void hud_layer_set_mem(const uintptr_t *base)
{
    s_memp = base;
}

static int clamp_margin(int m)
{
    return m < 0 ? 0 : m > HUD_MARGIN_MAX ? HUD_MARGIN_MAX : m;
}

void hud_layer_set_keys(unsigned aw, unsigned ah, int margin)
{
    /* Narrower than 4:3 reads as 4:3: the HUD keeps the 4:3 columns. */
    if (aw && ah && (unsigned long long)aw * 3u < (unsigned long long)ah * 4u)
        aw = 4, ah = 3;
    if (!aw || !ah)
        aw = ah = 0;
    __atomic_store_n(&s_aw, aw, __ATOMIC_RELAXED);
    __atomic_store_n(&s_ah, ah, __ATOMIC_RELAXED);
    __atomic_store_n(&s_margin, clamp_margin(margin), __ATOMIC_RELAXED);
}

void hud_layer_set_margin(int margin)
{
    __atomic_store_n(&s_margin, clamp_margin(margin), __ATOMIC_RELAXED);
}

int hud_layer_margin(void)
{
    return __atomic_load_n(&s_margin, __ATOMIC_RELAXED);
}

/* The edge kinds' pad (design A3): the target's pad, capped by the HUD's
 * own ratio, less the margin. roundf as H1: halves away from zero. */
static int pad_hud_of(uint32_t tw, uint32_t th, uint32_t pad)
{
    unsigned aw = __atomic_load_n(&s_aw, __ATOMIC_RELAXED);
    unsigned ah = __atomic_load_n(&s_ah, __ATOMIC_RELAXED);
    int p = (int)pad;

    if (aw && ah) {
        int at = (int)roundf((float)(((double)th * aw / ah - (double)tw) / 2.0));
        if (at < 0)
            at = 0;
        if (at < p)
            p = at;
    }
    p -= hud_layer_margin();
    return p < 0 ? 0 : p;
}

/* ---- the snapshot ------------------------------------------------------- */

static void snap_write(const struct hud_snap *sn)
{
    uint32_t q = __atomic_load_n(&s_snap_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_snap_seq, q + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_snap = *sn;
    __atomic_store_n(&s_snap_seq, q + 2u, __ATOMIC_RELEASE);
}

static void snap_read(struct hud_snap *out)
{
    for (;;) {
        uint32_t q = __atomic_load_n(&s_snap_seq, __ATOMIC_ACQUIRE);
        if (q & 1u)
            continue;
        *out = s_snap;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_snap_seq, __ATOMIC_RELAXED) == q)
            return;
    }
}

static int dim_ok(float v)
{
    return isfinite(v) && v > 0.0f && v <= HUD_DIM_MAX;
}

static int pos_ok(float v)
{
    return isfinite(v) && v >= 0.0f && v <= HUD_DIM_MAX;
}

static float rd_num(uint8_t *b, uint32_t va)
{
    float v = s_cv.type_u32 ? (float)rd32(b, va) : rdf(b, va);
    return v * s_cv.mem_scale;
}

/* The canvas and the target for this frame (design A11). `why` gets the
 * reason a run-time read fell back to the declared values, or "". */
static void canvas_now(struct hud_snap *sn, char *why, size_t whylen)
{
    uint32_t tw = 0, th = 0;

    why[0] = 0;
    if (!s_cv.target_frame) {
        tw = s_cv.tw;
        th = s_cv.th;
    } else if (!nv2a_aspect_frame(&tw, &th)) {
        /* No frame presented yet: the declared view's far corner, or the
         * canvas's size (the whole target, as BLiNX 2's 640x480). */
        if (s_cv.has_view) {
            tw = (uint32_t)lroundf(s_cv.view[0] + s_cv.view[2]);
            th = (uint32_t)lroundf(s_cv.view[1] + s_cv.view[3]);
        } else {
            tw = (uint32_t)lroundf(s_cv.w > 0 ? s_cv.w : 640.0f);
            th = (uint32_t)lroundf(s_cv.h > 0 ? s_cv.h : 480.0f);
        }
    }
    sn->tw = tw;
    sn->th = th;
    sn->pad = nv2a_host_pad(tw, th);
    sn->top_left = s_cv.top_left;
    sn->w = s_cv.w;
    sn->h = s_cv.h;
    if (s_cv.has_view) {
        sn->vx = s_cv.view[0];
        sn->vy = s_cv.view[1];
        sn->vw = s_cv.view[2];
        sn->vh = s_cv.view[3];
    } else {
        sn->vx = sn->vy = 0.0f;
        sn->vw = (float)tw;
        sn->vh = (float)th;
    }
    if (s_cv.source == SRC_TARGET) {
        sn->w = sn->vw = (float)tw;
        sn->h = sn->vh = (float)th;
        sn->vx = sn->vy = 0.0f;
    } else if (s_cv.source == SRC_MEMORY) {
        uint8_t *b = mem_base();
        float w, h, v[4] = { sn->vx, sn->vy, sn->vw, sn->vh };
        if (!b) {
            snprintf(why, whylen, "guest memory not mapped");
            return;
        }
        w = rd_num(b, s_cv.w_va);
        h = rd_num(b, s_cv.h_va);
        if (s_cv.has_view_va)
            for (int k = 0; k < 4; k++)
                v[k] = rd_num(b, s_cv.view_va + 4u * (uint32_t)k);
        if (!dim_ok(w) || !dim_ok(h)) {
            snprintf(why, whylen, "size %gx%g", (double)w, (double)h);
            return;
        }
        if (!pos_ok(v[0]) || !pos_ok(v[1]) || !dim_ok(v[2]) || !dim_ok(v[3])) {
            snprintf(why, whylen, "view %g,%g,%g,%g", (double)v[0], (double)v[1], (double)v[2],
                     (double)v[3]);
            return;
        }
        sn->w = w;
        sn->h = h;
        sn->vx = v[0];
        sn->vy = v[1];
        sn->vw = v[2];
        sn->vh = v[3];
    }
}

static void canvas_line(const struct hud_snap *sn, const char *why)
{
    static const char *const src[] = { "fixed", "memory", "target" };
    fprintf(stderr, "[hud.canvas] %gx%g origin %s view %g,%g,%g,%g target %ux%u source %s%s%s\n",
            (double)sn->w, (double)sn->h, sn->top_left ? "top-left" : "centre", (double)sn->vx,
            (double)sn->vy, (double)sn->vw, (double)sn->vh, sn->tw, sn->th, src[s_cv.source],
            *why ? " fallback " : "", why);
}

/* ---- the active flag ------------------------------------------------------ */

int hud_layer_frame(int owner_on)
{
    struct hud_snap sn;
    char why[64];
    int on, ph;

    __atomic_add_fetch(&s_epoch, 1u, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&s_table, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&s_on, __ATOMIC_RELAXED)) {
            __atomic_store_n(&s_on, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&s_mod, 0, __ATOMIC_RELEASE);
            nv2a_aspect_hud_set(0, 0, 0);
        }
        return 0;
    }
    canvas_now(&sn, why, sizeof why);
    snap_write(&sn);
    if (trace_on() && (!s_shown_valid || memcmp(&sn, &s_shown, sizeof sn)
                       || strcmp(why, s_shown_why))) {
        canvas_line(&sn, why);
        s_shown = sn;
        snprintf(s_shown_why, sizeof s_shown_why, "%s", why);
        s_shown_valid = 1;
    }
    on = owner_on && sn.pad > 0;
    ph = on ? pad_hud_of(sn.tw, sn.th, sn.pad) : 0;
    if (trace_on() && (on != s_lift_shown || (int)sn.pad != s_lift_pad || ph != s_lift_hud)) {
        fprintf(stderr, LOGP "lift %s pad %u hud_pad %d\n", on ? "on" : "off", sn.pad, ph);
        s_lift_shown = on;
        s_lift_pad = (int)sn.pad;
        s_lift_hud = ph;
    }
    nv2a_aspect_hud_set(on, sn.tw, sn.th);
    /* A capture widens the flag for recording only (design A15): the
     * wrappers keep their chain and apply reports its calls, while the
     * lift, the shifts and the clips follow the mod part alone. */
    __atomic_store_n(&s_mod, on, __ATOMIC_RELEASE);
    __atomic_store_n(&s_on, on || __atomic_load_n(&s_cap, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
    return on;
}

int hud_layer_active(void)
{
    return __atomic_load_n(&s_mod, __ATOMIC_ACQUIRE);
}

void hud_layer_set_capture(int on)
{
    __atomic_store_n(&s_cap, on != 0, __ATOMIC_RELEASE);
}

void hud_layer_set_recorder(hud_record_fn fn)
{
    __atomic_store_n(&s_rec, fn, __ATOMIC_RELEASE);
}

int hud_layer_has_table(void)
{
    return __atomic_load_n(&s_table, __ATOMIC_ACQUIRE);
}

void hud_layer_table_is_capture(void)
{
    __atomic_store_n(&s_table_cap, 1, __ATOMIC_RELEASE);
}

int hud_layer_has_mod_table(void)
{
    return hud_layer_has_table() && !__atomic_load_n(&s_table_cap, __ATOMIC_ACQUIRE);
}

int hud_layer_pad(void)
{
    struct hud_snap sn;
    if (!hud_layer_has_table())
        return 0;
    snap_read(&sn);
    return (int)sn.pad;
}

int hud_layer_pad_hud(void)
{
    struct hud_snap sn;
    if (!hud_layer_has_table())
        return 0;
    snap_read(&sn);
    return pad_hud_of(sn.tw, sn.th, sn.pad);
}

/* ---- the call chain (design A5) ------------------------------------------ */

static int chain_live(uint32_t e, uint32_t now)
{
    return now - e < 2u;
}

static void wrap_pre(struct xbox_mod_call *c, void *user)
{
    uint32_t now, ret;
    unsigned i, k;
    uint8_t *b;

    (void)user;
    if (!__atomic_load_n(&s_on, __ATOMIC_ACQUIRE) || !(b = mem_base()))
        return;
    now = __atomic_load_n(&s_epoch, __ATOMIC_RELAXED);
    /* A wrapper left by a longjmp never popped: drop what is two frames
     * old, so one still open across one frame boundary keeps its entry. */
    for (i = k = 0; i < t_nchain; i++)
        if (chain_live(t_chain[i].epoch, now))
            t_chain[k++] = t_chain[i];
    t_nchain = k;
    ret = rd32(b, c->esp);
    if (t_nchain >= HUD_CHAIN_MAX) {
        if (trace_on() && seen_first(SEEN_DEEP, ret))
            fprintf(stderr, LOGP "chain full at 0x%08X: push dropped\n", (unsigned)ret);
        return;
    }
    t_chain[t_nchain].ret = ret;
    t_chain[t_nchain].esp = c->esp;
    t_chain[t_nchain].epoch = now;
    t_nchain++;
}

static void wrap_post(struct xbox_mod_call *c, void *user)
{
    (void)user;
    /* Only the entry this call pushed: a dropped push (chain full, or the
     * flag clear at its pre) leaves another wrapper's on top. */
    if (t_nchain && t_chain[t_nchain - 1u].esp == c->esp)
        t_nchain--;
}

static const struct hud_site *site_find(uint32_t ret)
{
    int lo = 0, hi = s_nsites - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_sites[mid].ret == ret)
            return &s_sites[mid];
        if (s_sites[mid].ret < ret)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ---- the shift, the clip and the stretch (design A2, A3, A13) ----------- */

/* A primitive's x in guest pixels and u. */
struct prim {
    float gx[64], u[64];
    float lo, hi;
};

static void prim_bounds(struct prim *p, uint32_t n)
{
    p->lo = p->hi = p->gx[0];
    for (uint32_t k = 1; k < n; k++) {
        if (p->gx[k] < p->lo)
            p->lo = p->gx[k];
        if (p->gx[k] > p->hi)
            p->hi = p->gx[k];
    }
}

static void prim_shift(struct prim *p, uint32_t n, float d)
{
    for (uint32_t k = 0; k < n; k++)
        p->gx[k] += d;
    p->lo += d;
    p->hi += d;
}

/* Every vertex at the primitive's min or max x, and u one value per side
 * (a function of x alone): an axis-aligned sprite, whose clip is exact. */
static int prim_axis(const struct prim *p, uint32_t n, int has_u, float *u_lo, float *u_hi)
{
    int got_lo = 0, got_hi = 0;
    const float eps = 1e-3f;

    *u_lo = *u_hi = 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        int at_lo = fabsf(p->gx[k] - p->lo) <= eps, at_hi = fabsf(p->gx[k] - p->hi) <= eps;
        if (!at_lo && !at_hi)
            return 0;
        if (!has_u)
            continue;
        if (at_lo) {
            if (got_lo && fabsf(p->u[k] - *u_lo) > 1e-5f)
                return 0;
            *u_lo = p->u[k];
            got_lo = 1;
        } else {
            if (got_hi && fabsf(p->u[k] - *u_hi) > 1e-5f)
                return 0;
            *u_hi = p->u[k];
            got_hi = 1;
        }
    }
    return 1;
}

/* Past the wide frame's edge on its side, so a parked piece stays hidden
 * whatever it is anchored to. One already past stays where it is. */
static void prim_park(struct prim *p, uint32_t n, int left, uint32_t tw, uint32_t pad)
{
    if (left) {
        float edge = -(float)pad - 1.0f;
        if (p->hi > edge)
            prim_shift(p, n, edge - p->hi);
    } else {
        float edge = (float)(tw + pad) + 1.0f;
        if (p->lo < edge)
            prim_shift(p, n, edge - p->lo);
    }
}

static void prim_stretch(struct prim *p, uint32_t n, uint32_t tw, uint32_t pad)
{
    float cx = (float)tw * 0.5f, f = (float)(tw + 2u * pad) / (float)tw;

    for (uint32_t k = 0; k < n; k++)
        p->gx[k] = cx + (p->gx[k] - cx) * f;
    prim_bounds(p, n);
    /* Covering the 4:3 columns with an edge inside the already-wide band:
     * H5 takes a triangle past one edge only as full width (nv2a_aspect_full)
     * and would stretch it again. Each edge inside the band goes just past
     * it, u kept (at most 9 px of stretch, no texel outside its own u
     * range); an edge already past stays. With a small pad a one-sided
     * overhang (KUF's right fade, 0..680, at pad 4: -4..684.5) moves its
     * left edge only.
     * Not covered: a strip's degenerate triangles can bridge two sprites
     * into one that spans the columns while neither sprite does (a review
     * finding; no case seen yet). */
    if (nv2a_aspect_full_width(p->lo, p->hi, tw)) {
        int mlo = p->lo >= -NV2A_HUD_WIDE_BAND, mhi = p->hi <= (float)tw + NV2A_HUD_WIDE_BAND;
        float nlo = -NV2A_HUD_WIDE_BAND - 1.0f, nhi = (float)tw + NV2A_HUD_WIDE_BAND + 1.0f;
        if (mlo || mhi) {
            float olo = p->lo, ohi = p->hi;
            for (uint32_t k = 0; k < n; k++) {
                if (mlo && fabsf(p->gx[k] - olo) <= 1e-3f)
                    p->gx[k] = nlo;
                else if (mhi && fabsf(p->gx[k] - ohi) <= 1e-3f)
                    p->gx[k] = nhi;
            }
            prim_bounds(p, n);
        }
    }
}

static int dx_proportional(float cx, uint32_t tw, int ph)
{
    int dx = (int)roundf(cx / ((float)tw * 0.5f) * (float)ph);
    return dx < -ph ? -ph : dx > ph ? ph : dx;
}

static float bits_f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t f_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* The group's reference x (guest pixels) for this frame: the first member
 * primitive added in the epoch sets it, every thread then reads it. */
static float group_ref(struct hud_group *g, float centre, uint32_t now)
{
    uint64_t v = __atomic_load_n(&g->ref, __ATOMIC_ACQUIRE);
    if ((uint32_t)(v >> 32) != now) {
        uint64_t want = (uint64_t)now << 32 | f_bits(centre);
        if (__atomic_compare_exchange_n(&g->ref, &v, want, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return centre;
        if ((uint32_t)(v >> 32) != now)
            return centre;
    }
    return bits_f((uint32_t)v);
}

/* ---- the capture's view (design A15) --------------------------------------- */

/* Guest reads for the capture's view stop here: retail RAM, where the
 * vertices live (xbox_memory_layout.h). Level 1 reads a mod's vtx_va even
 * with the mod off, so a bad one must not reach past the guest map. */
#define HUD_VA_TOP (64u * 1024u * 1024u)

/* The vertices' bounds in canvas units, as the game wrote them: x at
 * layout.x_off and y right after it (pretransformed x, y, z, rhw), when
 * the stride holds it. Capped, and non-finite values skipped; none read
 * when the last one would reach past HUD_VA_TOP. */
static int vtx_bbox(uint8_t *b, uint32_t va, uint32_t n, uint32_t stride, uint32_t x_off,
                    float out[4])
{
    int has_y = x_off + 8u <= stride, any = 0;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;

    if (n > HUD_BBOX_MAX)
        n = HUD_BBOX_MAX;
    if (!n || (uint64_t)va + (uint64_t)(n - 1u) * stride + x_off + (has_y ? 8u : 4u)
                  > HUD_VA_TOP)
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = va + i * stride + x_off;
        float x = rdf(b, at), y = has_y ? rdf(b, at + 4u) : 0.0f;
        if (!isfinite(x) || !isfinite(y))
            continue;
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0;
        y1 = y > y1 ? y : y1;
        any = 1;
    }
    if (!any)
        return 0;
    out[0] = x0;
    out[1] = y0;
    out[2] = x1;
    out[3] = y1;
    return 1;
}

int hud_layer_bbox(uint32_t va, uint32_t n, uint32_t stride, uint32_t x_off, float out[4])
{
    uint8_t *b = mem_base();
    if (!b || !n || stride < x_off + 4u)
        return 0;
    return vtx_bbox(b, va, n, stride, x_off, out);
}

static void record(hud_record_fn rec, uint8_t *b, uint32_t own, const struct hud_site *site,
                   const struct hud_group *grp, const struct xbox_hud_call *d, uint32_t now)
{
    struct hud_record r;
    unsigned i;

    memset(&r, 0, sizeof r);
    for (i = 0; i < t_nchain; i++)
        if (chain_live(t_chain[i].epoch, now))
            r.chain[r.nchain++] = t_chain[i].ret;
    r.ret = own;
    if (site) {
        r.site = site->ret;
        r.element = site->element;
        r.group = grp && !grp->implicit ? grp->name : NULL;
    }
    r.n = d->n;
    r.prims = d->n / s_prim;
    r.has_bbox = vtx_bbox(b, d->vtx_va, d->n, s_stride, s_x_off, r.bbox);
    r.queue = d->queue;
    r.tex = d->tex;
    rec(&r);
}

int hud_layer_canvas(struct hud_layer_canvas *out)
{
    struct hud_snap sn;
    if (!hud_layer_has_table() || !out)
        return 0;
    snap_read(&sn);
    out->w = sn.w;
    out->h = sn.h;
    out->view[0] = sn.vx;
    out->view[1] = sn.vy;
    out->view[2] = sn.vw;
    out->view[3] = sn.vh;
    out->tw = sn.tw;
    out->th = sn.th;
    out->pad = sn.pad;
    out->top_left = sn.top_left;
    out->y_up = s_cv.y_up;
    return 1;
}

int hud_layer_site(uint32_t ret, const char **element, const char **group)
{
    const struct hud_site *st;
    if (!hud_layer_has_table() || !(st = site_find(ret)))
        return 0;
    if (element)
        *element = st->element;
    if (group)
        *group = s_groups[st->group].implicit ? NULL : s_groups[st->group].name;
    return 1;
}

void hud_layer_layout(uint32_t *stride, uint32_t *x_off, uint32_t *prim)
{
    if (stride)
        *stride = s_stride;
    if (x_off)
        *x_off = s_x_off;
    if (prim)
        *prim = s_prim;
}

int hud_layer_draws(const struct hud_draw **out)
{
    if (out)
        *out = s_draws;
    return hud_layer_has_table() ? s_ndraws : 0;
}

static void skip_line(uint32_t what, uint32_t ret, const char *why)
{
    if (trace_on() && seen_first(what, ret))
        fprintf(stderr, "[hud.skip] 0x%08X %s\n", (unsigned)ret, why);
}

int hud_layer_apply(const struct xbox_mod_call *c, const struct xbox_hud_call *d)
{
    const struct hud_site *site = NULL;
    struct hud_group *grp = NULL;
    struct hud_snap sn;
    struct prim p;
    uint8_t *b;
    uint32_t own, name_ret, now, i, n, k;
    float s, x0, ctr;
    int kind, ph, first_dx = INT_MIN, unlisted_said = 0, skew_said = 0;

    if (!__atomic_load_n(&s_on, __ATOMIC_ACQUIRE) || !c || !d || !(b = mem_base()))
        return -1;
    snap_read(&sn);
    now = __atomic_load_n(&s_epoch, __ATOMIC_RELAXED);
    /* A save at or below this call's esp, or two frames old, belongs to a
     * call that has returned without its post (the guest unwound past it):
     * dropped, so a later post at that esp cannot write it back. A save
     * above it is an outer RESTORE call still open. */
    while (t_nsave && (t_save[t_nsave - 1u].esp <= c->esp
                       || !chain_live(t_save[t_nsave - 1u].epoch, now)))
        t_nsave--;
    own = rd32(b, c->esp);
    /* The element: the outermost wrapper's caller first (a string's font
     * routine names its caller), then inwards, then the call's own. */
    for (i = 0; i < t_nchain && !site; i++)
        if (chain_live(t_chain[i].epoch, now))
            site = site_find(t_chain[i].ret);
    if (!site)
        site = site_find(own);
    name_ret = site ? site->ret : t_nchain ? t_chain[0].ret : own;
    kind = site ? s_groups[site->group].kind : XBOX_HUD_CENTRE;
    if (site)
        grp = &s_groups[site->group];
    n = d->n;
    {
        hud_record_fn rec = __atomic_load_n(&s_rec, __ATOMIC_ACQUIRE);
        if (rec && n)
            record(rec, b, own, site, grp, d, now);
    }
    /* A capture alone records and changes nothing. */
    if (!__atomic_load_n(&s_mod, __ATOMIC_ACQUIRE))
        return -1;
    if (!n)
        return kind;
    if (n % s_prim) {
        skip_line(SEEN_SKIP_PRIM, name_ret, "vertex count not a multiple of prim");
        return -1;
    }
    if ((d->flags & XBOX_HUD_RESTORE) && n > HUD_RESTORE_MAX) {
        skip_line(SEEN_SKIP_CAP, name_ret, "over the restore cap");
        return -1;
    }
    if ((d->flags & XBOX_HUD_RESTORE) && t_nsave >= HUD_SAVE_MAX) {
        skip_line(SEEN_NEST, name_ret, "restore calls nested too deep");
        return -1;
    }
    if (d->flags & XBOX_HUD_RESTORE) {
        unsigned top = t_nsave;
        for (i = 0; i < n; i++) {
            uint32_t va = d->vtx_va + i * s_stride;
            t_save[top].x[i] = rdf(b, va + s_x_off);
            t_save[top].u[i] = s_u_off >= 0 ? rdf(b, va + (uint32_t)s_u_off) : 0.0f;
        }
        t_save[top].esp = c->esp;
        t_save[top].va = d->vtx_va;
        t_save[top].n = n;
        t_save[top].epoch = now;
        t_nsave = top + 1u;
    }
    s = sn.vw / sn.w;
    x0 = sn.top_left ? 0.0f : -sn.w * 0.5f;
    ctr = (float)sn.tw * 0.5f;
    ph = pad_hud_of(sn.tw, sn.th, sn.pad);

    for (i = 0; i < n; i += s_prim) {
        float before[64];
        int dx = 0;
        for (k = 0; k < s_prim; k++) {
            uint32_t va = d->vtx_va + (i + k) * s_stride;
            before[k] = rdf(b, va + s_x_off);
            p.gx[k] = sn.vx + (before[k] - x0) * s;
            p.u[k] = s_u_off >= 0 ? rdf(b, va + (uint32_t)s_u_off) : 0.0f;
        }
        prim_bounds(&p, s_prim);
        if (!isfinite(p.lo) || !isfinite(p.hi))
            continue;
        if (kind == XBOX_HUD_STRETCH
            || (grp && grp->full_stretch && nv2a_aspect_full_width(p.lo, p.hi, sn.tw))) {
            /* A group's full-width primitive under full = "stretch": a dim
             * or backing the game draws with a picture texture, which H5
             * would centre as a movie, while the same call's smaller
             * panels keep the group's kind. */
            prim_stretch(&p, s_prim, sn.tw, sn.pad);
        } else if (kind == XBOX_HUD_WORLD) {
            /* Projected by the game: left alone, but a piece parked past
             * the group's bound is moved past the wide frame's edge. */
            if (grp && grp->has_park) {
                float mid = sn.vx + sn.vw * 0.5f;
                if ((p.lo - mid) / s > grp->park)
                    prim_park(&p, s_prim, 0, sn.tw, sn.pad);
                else if ((p.hi - mid) / s < -grp->park)
                    prim_park(&p, s_prim, 1, sn.tw, sn.pad);
            }
        } else if (!nv2a_aspect_full_width(p.lo, p.hi, sn.tw)) {
            float wlo, whi, ulo, uhi;
            if (kind == XBOX_HUD_LEFT)
                dx = -ph;
            else if (kind == XBOX_HUD_RIGHT)
                dx = ph;
            else if (kind == XBOX_HUD_PROPORTIONAL) {
                float c0 = (p.lo + p.hi) * 0.5f, ref;
                if (grp->has_ref)
                    ref = sn.vx + (grp->ref_x - x0) * s;
                else if (grp->implicit)
                    ref = c0;
                else
                    ref = group_ref(grp, c0, now);
                dx = dx_proportional(ref - ctr, sn.tw, ph);
            }
            if (first_dx == INT_MIN)
                first_dx = dx;
            prim_shift(&p, s_prim, (float)dx);
            wlo = (float)dx;
            whi = (float)sn.tw + (float)dx;
            if (p.lo >= wlo && p.hi <= whi) {
                /* inside its window */
            } else {
                if (!site && trace_on() && !unlisted_said && seen_first(SEEN_UNLISTED, name_ret)) {
                    fprintf(stderr, "[hud.unlisted] 0x%08X x %g..%g\n", (unsigned)name_ret,
                            (double)p.lo, (double)p.hi);
                    unlisted_said = 1;
                }
                if (p.hi <= wlo || p.lo >= whi) {
                    prim_park(&p, s_prim, (p.lo + p.hi) * 0.5f < ctr + (float)dx, sn.tw,
                              sn.pad);
                } else if (prim_axis(&p, s_prim, s_u_off >= 0, &ulo, &uhi)) {
                    float lo = p.lo, span = p.hi - p.lo;
                    for (k = 0; k < s_prim; k++) {
                        float nx = p.gx[k] < wlo ? wlo : p.gx[k] > whi ? whi : p.gx[k];
                        if (nx != p.gx[k] && s_u_off >= 0 && span > 0.0f)
                            p.u[k] = ulo + (nx - lo) / span * (uhi - ulo);
                        p.gx[k] = nx;
                    }
                    prim_bounds(&p, s_prim);
                } else if (trace_on() && !skew_said && seen_first(SEEN_SKEW, name_ret)) {
                    fprintf(stderr, "[hud.skew] 0x%08X\n", (unsigned)name_ret);
                    skew_said = 1;
                }
            }
        }
        for (k = 0; k < s_prim; k++) {
            uint32_t va = d->vtx_va + (i + k) * s_stride;
            float nx = before[k] + (p.gx[k] - (sn.vx + (before[k] - x0) * s)) / s;
            if (nx != before[k])
                wrf(b, va + s_x_off, nx);
            if (s_u_off >= 0)
                wrf(b, va + (uint32_t)s_u_off, p.u[k]);
        }
    }
    if (site && trace_on()) {
        struct hud_site *ms = (struct hud_site *)site;
        int shown = first_dx == INT_MIN ? 0 : first_dx;
        if (__atomic_exchange_n(&ms->last_dx, shown, __ATOMIC_RELAXED) != shown)
            fprintf(stderr, "[hud.site] 0x%08X %s %s %s dx %d %u\n", (unsigned)site->ret,
                    site->element, grp->name, kind_name(kind), shown, (unsigned)n);
    }
    return kind;
}

void hud_layer_restore(const struct xbox_mod_call *c)
{
    uint8_t *b;
    unsigned top;

    if (!t_nsave || !c)
        return;
    /* This esp's save, if any: the saves above it in the stack (lower
     * esp) are inner calls that returned without their post, dropped with
     * it. A post at an esp with no save writes nothing back. */
    for (top = t_nsave; top-- > 0;)
        if (t_save[top].esp == c->esp)
            break;
    if (top >= t_nsave)
        return;
    t_nsave = top;
    b = mem_base();
    for (uint32_t i = 0; b && i < t_save[top].n; i++) {
        uint32_t va = t_save[top].va + i * s_stride;
        wrf(b, va + s_x_off, t_save[top].x[i]);
        if (s_u_off >= 0)
            wrf(b, va + (uint32_t)s_u_off, t_save[top].u[i]);
    }
}

void hud_layer_post_off(const struct xbox_mod_call *c)
{
    /* The owner switched off between a pre and its post: the mod's post
     * is not called, so the layer ends what its pre began. A wrapper's
     * entry is popped, and a RESTORE save is written back, or a buffer
     * the game keeps would keep the shift and the next apply add to it. */
    if (!c)
        return;
    if (t_nchain && t_chain[t_nchain - 1u].esp == c->esp)
        t_nchain--;
    hud_layer_restore(c);
}

/* ---- the table (design A5, A11, A14; schema in docs/runtime/config.md) --- */

static int fail(const char *fmt, ...)
{
    va_list ap;
    fputs(LOGP "table: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

static int in_list(const char *k, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(k, *list))
            return 1;
    return 0;
}

/* "name.N.key" -> key, or NULL when it is not one of name's entries. */
static const char *aot_key(const char *key, const char *name)
{
    size_t l = strlen(name);
    const char *p;
    if (strncmp(key, name, l) || key[l] != '.')
        return NULL;
    p = key + l + 1;
    if (*p < '0' || *p > '9')
        return NULL;
    while (*p >= '0' && *p <= '9')
        p++;
    return *p == '.' ? p + 1 : NULL;
}

static int check_keys(const recomp_cfg *cfg)
{
    static const char *const canvas[] = { "w", "h", "origin", "y", "view", "target", "source",
                                          "w_va", "h_va", "view_va", "type", "mem_scale", NULL };
    static const char *const layout[] = { "stride", "x_off", "u_off", "prim", NULL };
    static const char *const wrapper[] = { "va", "name", NULL };
    static const char *const group[] = { "name", "kind", "ref_x", "park", "full", NULL };
    static const char *const site[] = { "ret_va", "element", "group", "kind", "note", NULL };
    size_t i, n = recomp_cfg_count(cfg);

    for (i = 0; i < n; i++) {
        const char *k = recomp_cfg_key_at(cfg, i), *sub;
        if (!strcmp(k, "version"))
            continue;
        /* [capture] is the mapping capture's (hud-anchors 2.1): its
         * level-0 draw entries, "draws", read there; nothing else. */
        if (!strcmp(k, "capture.draws") || !strncmp(k, "capture.draws.", 14))
            continue;
        if (!strncmp(k, "canvas.", 7) && in_list(k + 7, canvas))
            continue;
        if (!strncmp(k, "layout.", 7) && in_list(k + 7, layout))
            continue;
        if ((sub = aot_key(k, "wrapper")) && in_list(sub, wrapper))
            continue;
        if ((sub = aot_key(k, "group")) && in_list(sub, group))
            continue;
        if ((sub = aot_key(k, "site")) && in_list(sub, site))
            continue;
        return fail("unknown key %s (line %d)", k, recomp_cfg_line(cfg, k));
    }
    return 0;
}

static int num(const recomp_cfg *cfg, const char *key, float *out)
{
    recomp_cfg_type t = recomp_cfg_type_of(cfg, key);
    if (t != RCFG_INT && t != RCFG_FLOAT)
        return 0;
    *out = (float)recomp_cfg_float(cfg, key, 0.0);
    return 1;
}

static int u32_key(const recomp_cfg *cfg, const char *key, uint32_t *out)
{
    long long v;
    if (recomp_cfg_type_of(cfg, key) != RCFG_INT)
        return 0;
    v = recomp_cfg_int(cfg, key, -1);
    if (v < 0 || v > 0xFFFFFFFFll)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_canvas(const recomp_cfg *cfg, struct hud_canvas *cv)
{
    static const char *const src[] = { "fixed", "memory", "target", NULL };
    static const char *const origin[] = { "centre", "center", "top-left", NULL };
    static const char *const ydir[] = { "down", "up", NULL };
    static const char *const type[] = { "f32", "u32", NULL };
    const char *s;
    size_t i;

    memset(cv, 0, sizeof *cv);
    cv->target_frame = 1;
    cv->mem_scale = 1.0f;
    s = recomp_cfg_string(cfg, "canvas.source", "fixed");
    if ((cv->source = recomp_cfg_choice(cfg, "canvas.source", src, -1)) < 0
        && recomp_cfg_find(cfg, "canvas.source") >= 0)
        return fail("canvas.source=%s is not fixed, memory or target", s);
    if (cv->source < 0)
        cv->source = SRC_FIXED;
    if (cv->source != SRC_TARGET) {
        if (!num(cfg, "canvas.w", &cv->w) || !num(cfg, "canvas.h", &cv->h) || !dim_ok(cv->w)
            || !dim_ok(cv->h))
            return fail("canvas: w and h must be numbers in (0, 4096]");
    } else {
        cv->w = cv->h = 0.0f;
    }
    if (recomp_cfg_find(cfg, "canvas.origin") >= 0) {
        int o = recomp_cfg_choice(cfg, "canvas.origin", origin, -1);
        if (o < 0)
            return fail("canvas.origin is not centre or top-left");
        cv->top_left = o == 2;
    }
    if (recomp_cfg_find(cfg, "canvas.y") >= 0) {
        int y = recomp_cfg_choice(cfg, "canvas.y", ydir, -1);
        if (y < 0)
            return fail("canvas.y is not down or up");
        cv->y_up = y == 1;
    }
    if (recomp_cfg_find(cfg, "canvas.view") >= 0) {
        if (recomp_cfg_array_len(cfg, "canvas.view") != 4)
            return fail("canvas.view is not [x, y, w, h]");
        for (i = 0; i < 4; i++)
            cv->view[i] = (float)recomp_cfg_array_float(cfg, "canvas.view", i, NAN);
        if (!pos_ok(cv->view[0]) || !pos_ok(cv->view[1]) || !dim_ok(cv->view[2])
            || !dim_ok(cv->view[3]))
            return fail("canvas.view needs x, y in [0, 4096] and w, h in (0, 4096]");
        cv->has_view = 1;
    }
    if (recomp_cfg_find(cfg, "canvas.target") >= 0) {
        if (recomp_cfg_type_of(cfg, "canvas.target") == RCFG_STRING
            && !strcmp(recomp_cfg_string(cfg, "canvas.target", ""), "frame")) {
            cv->target_frame = 1;
        } else if (recomp_cfg_array_len(cfg, "canvas.target") == 2) {
            long long tw = recomp_cfg_array_int(cfg, "canvas.target", 0, 0);
            long long th = recomp_cfg_array_int(cfg, "canvas.target", 1, 0);
            if (tw <= 0 || th <= 0 || tw > 4096 || th > 4096)
                return fail("canvas.target is not [w, h] in (0, 4096]");
            cv->target_frame = 0;
            cv->tw = (uint32_t)tw;
            cv->th = (uint32_t)th;
        } else {
            return fail("canvas.target is not \"frame\" or [w, h]");
        }
    }
    if (cv->source == SRC_MEMORY) {
        if (!u32_key(cfg, "canvas.w_va", &cv->w_va) || !u32_key(cfg, "canvas.h_va", &cv->h_va))
            return fail("canvas: source memory needs w_va and h_va");
        if (recomp_cfg_find(cfg, "canvas.view_va") >= 0) {
            if (!u32_key(cfg, "canvas.view_va", &cv->view_va))
                return fail("canvas.view_va is not an address");
            cv->has_view_va = 1;
        }
        if (recomp_cfg_find(cfg, "canvas.type") >= 0) {
            int t = recomp_cfg_choice(cfg, "canvas.type", type, -1);
            if (t < 0)
                return fail("canvas.type is not f32 or u32");
            cv->type_u32 = t == 1;
        }
        if (recomp_cfg_find(cfg, "canvas.mem_scale") >= 0
            && (!num(cfg, "canvas.mem_scale", &cv->mem_scale) || !dim_ok(cv->mem_scale)))
            return fail("canvas.mem_scale is not a number in (0, 4096]");
    } else if (recomp_cfg_find(cfg, "canvas.w_va") >= 0 || recomp_cfg_find(cfg, "canvas.h_va") >= 0
               || recomp_cfg_find(cfg, "canvas.view_va") >= 0) {
        return fail("canvas: w_va, h_va and view_va need source = \"memory\"");
    }
    return 0;
}

static int parse_layout(const recomp_cfg *cfg)
{
    long long stride = recomp_cfg_int(cfg, "layout.stride", -1);
    long long x_off = recomp_cfg_int(cfg, "layout.x_off", -1);
    long long u_off = recomp_cfg_int(cfg, "layout.u_off", -1);
    long long prim = recomp_cfg_int(cfg, "layout.prim", 4);

    if (stride < 4 || stride > 256)
        return fail("layout.stride must be 4..256");
    if (x_off < 0 || x_off + 4 > stride)
        return fail("layout.x_off must lie in the stride");
    if (recomp_cfg_find(cfg, "layout.u_off") >= 0 && (u_off < 0 || u_off + 4 > stride))
        return fail("layout.u_off must lie in the stride");
    if (prim < 1 || prim > 64)
        return fail("layout.prim must be 1..64");
    s_stride = (uint32_t)stride;
    s_x_off = (uint32_t)x_off;
    s_u_off = (int)u_off;
    s_prim = (uint32_t)prim;
    return 0;
}

static int group_find(const char *name)
{
    for (int i = 0; i < s_ngroups; i++)
        if (!s_groups[i].implicit && !strcmp(s_groups[i].name, name))
            return i;
    return -1;
}

static int site_cmp(const void *a, const void *b)
{
    uint32_t x = ((const struct hud_site *)a)->ret, y = ((const struct hud_site *)b)->ret;
    return x < y ? -1 : x > y;
}

static void drop_table(void)
{
    free(s_groups);
    free(s_sites);
    s_groups = NULL;
    s_sites = NULL;
    s_ngroups = s_nsites = s_nwrappers = s_ndraws = 0;
}

/* ---- [capture] draws: lenient (design A15) --------------------------------
 *
 * Read with every table but used only by the capture: a row the runtime
 * cannot use keeps va 0 with the reason, and never fails the table. */

static int draw_va(const char *s, uint32_t *va, const char **why)
{
    char *end;
    unsigned long v;

    if (!strncmp(s, "sig:", 4)) {
        /* Hook slots carry no names and guest code is not mapped at init:
         * the CLI writes a copy of the table with the VA resolved. */
        *why = "a sig: draw needs its VA (xbr hud-map writes it)";
        return 0;
    }
    v = strtoul(s, &end, 0);
    if (end == s || *end || !v || v > 0xFFFFFFFFul) {
        *why = "fn is not an address";
        return 0;
    }
    *va = (uint32_t)v;
    return 1;
}

/* "ecx", "edx", "stack:N" (bytes past the return address, N a multiple of
 * 4 below 4096), or an integer constant when `cst` allows one. */
static int draw_loc(const recomp_cfg *cfg, const char *key, int cst, struct hud_loc *loc)
{
    recomp_cfg_type t = recomp_cfg_type_of(cfg, key);
    const char *s;

    memset(loc, 0, sizeof *loc);
    if (recomp_cfg_find(cfg, key) < 0)
        return 1;
    if (t == RCFG_INT && cst) {
        long long v = recomp_cfg_int(cfg, key, -1);
        if (v < 0 || v > 0xFFFF)
            return 0;
        loc->kind = HUD_LOC_CONST;
        loc->v = (uint32_t)v;
        return 1;
    }
    if (t != RCFG_STRING)
        return 0;
    s = recomp_cfg_string(cfg, key, "");
    if (!strcmp(s, "ecx")) {
        loc->kind = HUD_LOC_ECX;
        return 1;
    }
    if (!strcmp(s, "edx")) {
        loc->kind = HUD_LOC_EDX;
        return 1;
    }
    if (!strncmp(s, "stack:", 6)) {
        char *end;
        unsigned long v = strtoul(s + 6, &end, 0);
        if (end == s + 6 || *end || v % 4u || v >= 4096u)
            return 0;
        loc->kind = HUD_LOC_STACK;
        loc->v = (uint32_t)v;
        return 1;
    }
    return 0;
}

static void parse_draws(const recomp_cfg *cfg)
{
    size_t nt = recomp_cfg_table_count(cfg, "capture.draws");
    size_t na = nt ? 0 : recomp_cfg_array_len(cfg, "capture.draws");
    char key[96];
    size_t i;

    s_ndraws = 0;
    memset(s_draws, 0, sizeof s_draws);
    /* The short form: a list of VAs (or "0x..." / "sig:" strings), each
     * recorded with its chain only. */
    for (i = 0; i < na && s_ndraws < (int)HUD_DRAWS_MAX; i++) {
        struct hud_draw *dr = &s_draws[s_ndraws++];
        recomp_cfg_type t = recomp_cfg_array_type(cfg, "capture.draws", i);
        dr->prim = s_prim;
        if (t == RCFG_INT) {
            long long v = recomp_cfg_array_int(cfg, "capture.draws", i, 0);
            snprintf(dr->spec, sizeof dr->spec, "0x%08llX", (unsigned long long)v);
            if (v <= 0 || v > 0xFFFFFFFFll)
                dr->why = "fn is not an address";
            else
                dr->va = (uint32_t)v;
        } else if (t == RCFG_STRING) {
            const char *v = recomp_cfg_array_string(cfg, "capture.draws", i, "");
            snprintf(dr->spec, sizeof dr->spec, "%s", v);
            if (!draw_va(v, &dr->va, &dr->why))
                dr->va = 0;
        } else {
            dr->why = "not an address or a string";
        }
    }
    for (i = 0; i < nt && s_ndraws < (int)HUD_DRAWS_MAX; i++) {
        struct hud_draw *dr = &s_draws[s_ndraws++];
        const char *fk = "fn";
        long long prim;

        dr->prim = s_prim;
        snprintf(key, sizeof key, "capture.draws.%zu.fn", i);
        if (recomp_cfg_find(cfg, key) < 0) {
            snprintf(key, sizeof key, "capture.draws.%zu.va", i);
            fk = "va";
        }
        snprintf(key, sizeof key, "capture.draws.%zu.%s", i, fk);
        if (recomp_cfg_type_of(cfg, key) == RCFG_INT) {
            long long v = recomp_cfg_int(cfg, key, 0);
            snprintf(dr->spec, sizeof dr->spec, "0x%08llX", (unsigned long long)v);
            if (v > 0 && v <= 0xFFFFFFFFll)
                dr->va = (uint32_t)v;
            else
                dr->why = "fn is not an address";
        } else if (recomp_cfg_type_of(cfg, key) == RCFG_STRING) {
            const char *v = recomp_cfg_string(cfg, key, "");
            snprintf(dr->spec, sizeof dr->spec, "%s", v);
            if (!draw_va(v, &dr->va, &dr->why))
                dr->va = 0;
        } else {
            snprintf(key, sizeof key, "capture.draws.%zu.sig", i);
            snprintf(dr->spec, sizeof dr->spec, "sig:%s", recomp_cfg_string(cfg, key, "?"));
            dr->why = recomp_cfg_find(cfg, key) >= 0
                          ? "a sig: draw needs its VA (xbr hud-map writes it)"
                          : "no fn";
        }
        snprintf(key, sizeof key, "capture.draws.%zu.name", i);
        snprintf(dr->name, sizeof dr->name, "%s", recomp_cfg_string(cfg, key, ""));
        snprintf(key, sizeof key, "capture.draws.%zu.vtx", i);
        if (!draw_loc(cfg, key, 0, &dr->vtx))
            dr->why = dr->why ? dr->why : "vtx is not ecx, edx or stack:N";
        snprintf(key, sizeof key, "capture.draws.%zu.count", i);
        if (!draw_loc(cfg, key, 1, &dr->count))
            dr->why = dr->why ? dr->why : "count is not ecx, edx, stack:N or a number";
        snprintf(key, sizeof key, "capture.draws.%zu.stride", i);
        if (!draw_loc(cfg, key, 1, &dr->stride))
            dr->why = dr->why ? dr->why : "stride is not ecx, edx, stack:N or a number";
        if ((dr->vtx.kind != HUD_LOC_NONE) != (dr->count.kind != HUD_LOC_NONE))
            dr->why = dr->why ? dr->why : "vtx and count go together";
        snprintf(key, sizeof key, "capture.draws.%zu.prim", i);
        prim = recomp_cfg_int(cfg, key, (long long)s_prim);
        if (prim < 1 || prim > 64)
            dr->why = dr->why ? dr->why : "prim is not 1..64";
        else
            dr->prim = (uint32_t)prim;
        if (dr->why)
            dr->va = 0;
    }
}

static int parse_table(const recomp_cfg *cfg, hud_hook_fn hook)
{
    size_t ng = recomp_cfg_table_count(cfg, "group");
    size_t ns = recomp_cfg_table_count(cfg, "site");
    size_t nw = recomp_cfg_table_count(cfg, "wrapper");
    char key[96];
    size_t i;

    if (recomp_cfg_type_of(cfg, "version") != RCFG_INT || recomp_cfg_int(cfg, "version", 0) != 1)
        return fail("version must be 1");
    if (check_keys(cfg) < 0 || parse_canvas(cfg, &s_cv) < 0 || parse_layout(cfg) < 0)
        return -1;
    parse_draws(cfg);
    if (ng + ns > 65536)
        return fail("too many groups and sites");
    s_groups = calloc(ng + ns + 1, sizeof *s_groups);
    s_sites = calloc(ns + 1, sizeof *s_sites);
    if (!s_groups || !s_sites)
        return fail("out of memory");
    for (i = 0; i < ng; i++) {
        struct hud_group *g = &s_groups[s_ngroups];
        const char *name, *kind;
        snprintf(key, sizeof key, "group.%zu.name", i);
        name = recomp_cfg_string(cfg, key, NULL);
        if (!name || !*name || strlen(name) >= HUD_NAME_MAX)
            return fail("group %zu: a name of 1..%d characters is needed", i, HUD_NAME_MAX - 1);
        if (group_find(name) >= 0)
            return fail("group %s: listed twice", name);
        snprintf(key, sizeof key, "group.%zu.kind", i);
        kind = recomp_cfg_string(cfg, key, NULL);
        if ((g->kind = kind_parse(kind)) < 0)
            return fail("group %s: unknown kind %s", name, kind ? kind : "(none)");
        snprintf(g->name, sizeof g->name, "%s", name);
        snprintf(key, sizeof key, "group.%zu.ref_x", i);
        if (recomp_cfg_find(cfg, key) >= 0) {
            if (!num(cfg, key, &g->ref_x) || !isfinite(g->ref_x))
                return fail("group %s: ref_x is not a number", name);
            g->has_ref = 1;
        }
        snprintf(key, sizeof key, "group.%zu.park", i);
        if (recomp_cfg_find(cfg, key) >= 0) {
            if (g->kind != XBOX_HUD_WORLD)
                return fail("group %s: park is for world groups", name);
            if (!num(cfg, key, &g->park) || !isfinite(g->park) || g->park <= 0.0f)
                return fail("group %s: park is not a positive number", name);
            g->has_park = 1;
        }
        snprintf(key, sizeof key, "group.%zu.full", i);
        if (recomp_cfg_find(cfg, key) >= 0) {
            const char *full = recomp_cfg_string(cfg, key, NULL);
            if (g->kind == XBOX_HUD_STRETCH || g->kind == XBOX_HUD_WORLD)
                return fail("group %s: full is for groups that move or keep", name);
            if (!full || (strcmp(full, "stretch") && strcmp(full, "keep")))
                return fail("group %s: full must be \"stretch\" or \"keep\"", name);
            g->full_stretch = !strcmp(full, "stretch");
        }
        s_ngroups++;
    }
    for (i = 0; i < ns; i++) {
        struct hud_site *st = &s_sites[s_nsites];
        const char *el, *gn, *kind;
        snprintf(key, sizeof key, "site.%zu.ret_va", i);
        if (!u32_key(cfg, key, &st->ret) || !st->ret)
            return fail("site %zu: ret_va is not an address", i);
        snprintf(key, sizeof key, "site.%zu.element", i);
        el = recomp_cfg_string(cfg, key, NULL);
        if (!el || !*el || strlen(el) >= HUD_NAME_MAX)
            return fail("site 0x%08X: an element of 1..%d characters is needed",
                        (unsigned)st->ret, HUD_NAME_MAX - 1);
        snprintf(st->element, sizeof st->element, "%s", el);
        snprintf(key, sizeof key, "site.%zu.group", i);
        gn = recomp_cfg_string(cfg, key, NULL);
        snprintf(key, sizeof key, "site.%zu.kind", i);
        kind = recomp_cfg_string(cfg, key, NULL);
        if (gn) {
            if (kind)
                return fail("site 0x%08X: kind belongs to group %s", (unsigned)st->ret, gn);
            if ((st->group = group_find(gn)) < 0)
                return fail("site 0x%08X: unknown group %s", (unsigned)st->ret, gn);
        } else {
            /* Its own group, named after its element. */
            struct hud_group *g = &s_groups[s_ngroups];
            if ((g->kind = kind_parse(kind)) < 0)
                return fail("site 0x%08X: no group, and %s %s", (unsigned)st->ret,
                            kind ? "unknown kind" : "no kind", kind ? kind : "");
            snprintf(g->name, sizeof g->name, "%s", el);
            g->implicit = 1;
            st->group = s_ngroups++;
        }
        st->last_dx = INT_MIN;
        s_nsites++;
    }
    qsort(s_sites, (size_t)s_nsites, sizeof *s_sites, site_cmp);
    for (i = 1; i < (size_t)s_nsites; i++)
        if (s_sites[i].ret == s_sites[i - 1].ret)
            return fail("site 0x%08X: listed twice", (unsigned)s_sites[i].ret);
    /* Hooked last, once everything else holds: a hook cannot be taken
     * back, and with no table registered its callbacks do nothing. A
     * wrapper without a slot fails the table after the ones before it
     * are hooked: those stay in the hook service, inert, since the
     * dropped table leaves the layer inactive (wrap_pre's first test)
     * and their posts pop nothing. */
    for (i = 0; i < nw; i++) {
        uint32_t va;
        snprintf(key, sizeof key, "wrapper.%zu.va", i);
        if (!u32_key(cfg, key, &va) || !va)
            return fail("wrapper %zu: va is not an address", i);
        if (!hook || !hook(va, wrap_pre, wrap_post, 0, NULL))
            return fail("wrapper 0x%08X: no hook slot", (unsigned)va);
        s_nwrappers++;
    }
    return s_nsites;
}

int hud_layer_table(const char *toml, size_t len, hud_hook_fn hook)
{
    char err[256];
    recomp_cfg *cfg;
    char *text;
    int rc;

    if (hud_layer_has_table())
        return fail("a table is already registered");
    if (!toml)
        return fail("no text");
    text = malloc(len + 1);
    if (!text)
        return fail("out of memory");
    memcpy(text, toml, len);
    text[len] = 0;
    cfg = recomp_cfg_load_string_aot(text, "hud table", err, sizeof err);
    free(text);
    if (!cfg)
        return fail("%s", err);
    rc = parse_table(cfg, hook);
    recomp_cfg_free(cfg);
    if (rc < 0) {
        drop_table();
        return rc;
    }
    __atomic_store_n(&s_table, 1, __ATOMIC_RELEASE);
    fprintf(stderr, LOGP "table: %d sites, %d groups, %d wrappers\n", s_nsites, s_ngroups,
            s_nwrappers);
    if (trace_on()) {
        struct hud_snap sn;
        char why[64];
        canvas_now(&sn, why, sizeof why);
        canvas_line(&sn, why);
        s_shown = sn;
        snprintf(s_shown_why, sizeof s_shown_why, "%s", why);
        s_shown_valid = 1;
    }
    return rc;
}

void hud_layer_reset(void)
{
    __atomic_store_n(&s_on, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mod, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cap, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_table_cap, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_rec, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s_table, 0, __ATOMIC_RELEASE);
    nv2a_aspect_hud_set(0, 0, 0);
    drop_table();
    memset(&s_cv, 0, sizeof s_cv);
    memset(s_seen, 0, sizeof s_seen);
    memset(&s_snap, 0, sizeof s_snap);
    s_shown_valid = 0;
    s_lift_shown = s_lift_pad = s_lift_hud = -1;
    t_nchain = 0;
    t_nsave = 0;
}
