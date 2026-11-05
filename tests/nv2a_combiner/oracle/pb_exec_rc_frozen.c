/* Frozen oracle for nv2a_combiner_fuzz: the register-combiner evaluators
 * that lived in src/kernel/nv2a_pb_exec.c before nv2a_combiner.c (rc_eval, the
 * reference, and rc_plan_build/rc_eval_fast, the per-batch plan), extracted
 * with a minimal s_vsh shim and the old_* entry points at the end. The fuzz
 * requires nv2a_combiner.c to match both bit for bit. Not built into any
 * library. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"
static struct { uint32_t rc_cicw[8], rc_aicw[8], rc_cocw[8], rc_aocw[8], rc_f0[8], rc_f1[8], rc_ctl, rc_fcw0, rc_fcw1, rc_sf[2], fog_color; int fog_enable; } s_vsh;
static void rc_unpack(uint32_t c, float o[4])
{
    nv2a_argb_to_float4(c, o);
}

static float rc_map(float x, uint32_t map)
{
    switch (map) {
    case 0: return x < 0.0f ? 0.0f : x;                     /* unsigned */
    case 1: return 1.0f - (x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x);
    case 2: return 2.0f * (x < 0.0f ? 0.0f : x) - 1.0f;     /* expand */
    case 3: return 1.0f - 2.0f * (x < 0.0f ? 0.0f : x);
    case 4: return (x < 0.0f ? 0.0f : x) - 0.5f;            /* half bias */
    case 5: return 0.5f - (x < 0.0f ? 0.0f : x);
    case 6: return x;                                       /* signed */
    default: return -x;
    }
}

static void rc_in_rgb(float r[16][4], uint32_t b, float v[3])
{
    const float *s = r[b & 0xF];
    uint32_t map = (b >> 5) & 7;
    int k;
    for (k = 0; k < 3; k++)
        v[k] = rc_map((b & 0x10) ? s[3] : s[k], map);
}

static float rc_in_a(float r[16][4], uint32_t b)
{
    const float *s = r[b & 0xF];
    return rc_map((b & 0x10) ? s[3] : s[2], (b >> 5) & 7);
}

static float rc_op(float x, uint32_t op)
{
    switch (op) {
    case 1: x -= 0.5f; break;
    case 2: x *= 2.0f; break;
    case 3: x = (x - 0.5f) * 2.0f; break;
    case 4: x *= 4.0f; break;
    case 6: x *= 0.5f; break;
    default: break;
    }
    return x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static int s_rc_trace;                  /* RECOMP_WHITE_TRACE, one pixel */

/* General combiners then the final combiner, on r[] (register-code indexed);
 * the result is written to out[4] as r,g,b,a. */
static void rc_eval(float r[16][4], float out[4])
{
    uint32_t stages = s_vsh.rc_ctl & 0xFF, s;
    int mux_msb = (s_vsh.rc_ctl >> 8) & 1;
    int uniq0 = (s_vsh.rc_ctl >> 12) & 1, uniq1 = (s_vsh.rc_ctl >> 16) & 1;
    float nr[16][4], A[3], B[3], C[3], D[3], E[3], F[3];
    int k;

    if (stages > 8)
        stages = 8;
    for (s = 0; s < stages; s++) {
        uint32_t icw = s_vsh.rc_cicw[s], ocw = s_vsh.rc_cocw[s];
        uint32_t aicw = s_vsh.rc_aicw[s], aocw = s_vsh.rc_aocw[s];
        float ab[3], cd[3], sum[3], aab, acd, asum;
        int sel;

        rc_unpack(s_vsh.rc_f0[uniq0 ? s : 0], r[1]);
        rc_unpack(s_vsh.rc_f1[uniq1 ? s : 0], r[2]);
        memcpy(nr, r, sizeof nr);
        sel = mux_msb ? r[12][3] >= 0.5f
                      : (((uint32_t)(clamp01(r[12][3]) * 255.0f + 0.5f)) & 1u);

        /* colour */
        rc_in_rgb(r, icw >> 24, A); rc_in_rgb(r, icw >> 16, B);
        rc_in_rgb(r, icw >>  8, C); rc_in_rgb(r, icw,       D);
        if (ocw & (1u << 13)) {
            float d = A[0]*B[0] + A[1]*B[1] + A[2]*B[2];
            ab[0] = ab[1] = ab[2] = d;
        } else {
            for (k = 0; k < 3; k++) ab[k] = A[k] * B[k];
        }
        if (ocw & (1u << 12)) {
            float d = C[0]*D[0] + C[1]*D[1] + C[2]*D[2];
            cd[0] = cd[1] = cd[2] = d;
        } else {
            for (k = 0; k < 3; k++) cd[k] = C[k] * D[k];
        }
        for (k = 0; k < 3; k++) {
            sum[k] = (ocw & (1u << 14)) ? (sel ? cd[k] : ab[k]) : ab[k] + cd[k];
            ab[k]  = rc_op(ab[k],  (ocw >> 15) & 7);
            cd[k]  = rc_op(cd[k],  (ocw >> 15) & 7);
            sum[k] = rc_op(sum[k], (ocw >> 15) & 7);
        }
        if ((ocw >> 4) & 0xF) memcpy(nr[(ocw >> 4) & 0xF], ab, sizeof ab);
        if ( ocw       & 0xF) memcpy(nr[ ocw       & 0xF], cd, sizeof cd);
        if ((ocw >> 8) & 0xF) memcpy(nr[(ocw >> 8) & 0xF], sum, sizeof sum);

        /* alpha */
        aab = rc_in_a(r, aicw >> 24) * rc_in_a(r, aicw >> 16);
        acd = rc_in_a(r, aicw >>  8) * rc_in_a(r, aicw);
        /* the alpha word is laid out as the RGB one: mux [14], mapping [17:15] */
        asum = (aocw & (1u << 14)) ? (sel ? acd : aab) : aab + acd;
        if ((aocw >> 4) & 0xF) nr[(aocw >> 4) & 0xF][3] = rc_op(aab,  (aocw >> 15) & 7);
        if ( aocw       & 0xF) nr[ aocw       & 0xF][3] = rc_op(acd,  (aocw >> 15) & 7);
        if ((aocw >> 8) & 0xF) nr[(aocw >> 8) & 0xF][3] = rc_op(asum, (aocw >> 15) & 7);

        /* blue to alpha overrides the alpha portion's write */
        if ((ocw & (1u << 19)) && ((ocw >> 4) & 0xF)) nr[(ocw >> 4) & 0xF][3] = ab[2];
        if ((ocw & (1u << 18)) && (ocw & 0xF))        nr[ocw & 0xF][3] = cd[2];
        memcpy(r, nr, sizeof nr);
        if (s_rc_trace)
            fprintf(stderr, "  [WHITE]   stage %u icw %08X ocw %08X aicw %08X aocw %08X"
                            " c0 %08X c1 %08X | r0 %.3f %.3f %.3f %.3f  r1 %.3f %.3f %.3f %.3f"
                            "  t3 %.3f %.3f %.3f %.3f\n", s, icw, ocw, aicw, aocw,
                    s_vsh.rc_f0[uniq0 ? s : 0], s_vsh.rc_f1[uniq1 ? s : 0],
                    r[12][0], r[12][1], r[12][2], r[12][3],
                    r[13][0], r[13][1], r[13][2], r[13][3],
                    r[11][0], r[11][1], r[11][2], r[11][3]);
    }

    /* Final combiner: unsigned inputs, A*B + (1-A)*C + D, alpha from G. */
    {
        uint32_t c0 = s_vsh.rc_fcw0, c1 = s_vsh.rc_fcw1;
        float v1[3], r0[3], G;
        rc_unpack(s_vsh.rc_sf[0], r[1]);
        rc_unpack(s_vsh.rc_sf[1], r[2]);
#define FC_IN(b, v) do { const float *s_ = r[(b) & 0xF]; int k_; \
        for (k_ = 0; k_ < 3; k_++) { float x_ = clamp01(((b) & 0x10) ? s_[3] : s_[k_]); \
            (v)[k_] = ((b) & 0x20) ? 1.0f - x_ : x_; } } while (0)
        FC_IN(c1 >> 24, E);
        FC_IN(c1 >> 16, F);
        for (k = 0; k < 3; k++) {
            /* COMPLEMENT_V1 is 0x40, COMPLEMENT_R0 0x20 (xemu psh_regs.h). */
            v1[k] = clamp01(r[5][k]);  if (c1 & 0x40) v1[k] = 1.0f - v1[k];
            r0[k] = clamp01(r[12][k]); if (c1 & 0x20) r0[k] = 1.0f - r0[k];
            r[14][k] = v1[k] + r0[k];
            if (c1 & 0x80) r[14][k] = clamp01(r[14][k]);
            r[15][k] = E[k] * F[k];
        }
        r[14][3] = r[15][3] = 0.0f;
        FC_IN(c0 >> 24, A); FC_IN(c0 >> 16, B);
        FC_IN(c0 >>  8, C); FC_IN(c0,       D);
        {
            uint32_t g = c1 >> 8;
            const float *sg = r[g & 0xF];
            G = clamp01((g & 0x10) ? sg[3] : sg[2]);
            if (g & 0x20) G = 1.0f - G;
        }
#undef FC_IN
        for (k = 0; k < 3; k++)
            out[k] = clamp01(A[k] * B[k] + (1.0f - A[k]) * C[k] + D[k]);
        out[3] = G;
        if (s_rc_trace)
            fprintf(stderr, "  [WHITE]   final cw0 %08X cw1 %08X sf %08X %08X | A %.3f %.3f %.3f"
                            " B %.3f %.3f %.3f C %.3f %.3f %.3f D %.3f %.3f %.3f G %.3f"
                            " | fog %.3f %.3f %.3f %.3f v1 %.3f %.3f %.3f -> %.3f %.3f %.3f %.3f\n",
                    c0, c1, s_vsh.rc_sf[0], s_vsh.rc_sf[1], A[0], A[1], A[2], B[0], B[1], B[2],
                    C[0], C[1], C[2], D[0], D[1], D[2], G,
                    r[3][0], r[3][1], r[3][2], r[3][3], r[5][0], r[5][1], r[5][2],
                    out[0], out[1], out[2], out[3]);
    }
}

/* (b) -------------------------------------------------------------------
 *
 * Inputs that read a register no pixel can change -- zero, the stage's two
 * factor constants, fog when nothing writes it -- are mapped once, here, by
 * the same rc_map, and become constants. The rest dispatch on their mapping
 * once per input rather than once per component, and the output mapping once
 * per stage. Values that no output and no sum consumes are not computed. */
typedef struct {
    uint8_t reg, alpha, map, cst;
    float   cv[3];                       /* mapped value when cst */
} RcIn;
typedef struct {
    uint8_t reg, alpha, inv, cst;
    float   cv[3];
} FcIn;
typedef struct {
    RcIn  rgb[4], a[4];
    uint8_t ab, cd, sum, dot_ab, dot_cd, mux, op;
    uint8_t aab, acd, asum, amux, aop, b2a_ab, b2a_cd;
    uint8_t need_ab, need_cd, need_aab, need_acd, need_sel;
} RcStage;

static struct {
    uint32_t stages;
    int      mux_msb;
    RcStage  st[8];
    FcIn     e, f, a, b, c, d, g;
    int      v1_inv, r0_inv, sum_clamp;
    float    tmpl[16][4];                /* register file at pixel start */
    uint32_t dirty;                      /* registers reset between pixels */
    int      fog_var;                    /* FOG alpha varies per pixel */
} s_rcp;

/* Registers whose value at a stage's input is the same for every pixel. */
static const float *rc_const_reg(uint32_t reg, const float f0[4], const float f1[4],
                                 uint32_t written)
{
    if (reg == 0) return s_rcp.tmpl[0];
    if (reg == 1) return f0;
    if (reg == 2) return f1;
    if (reg == 3 && !(written & (1u << 3)) && !s_rcp.fog_var) return s_rcp.tmpl[3];
    return NULL;
}

static RcIn rc_dec_in(uint32_t b, int alpha_port, const float f0[4],
                      const float f1[4], uint32_t written)
{
    RcIn i;
    const float *s;
    int k;
    memset(&i, 0, sizeof i);
    i.reg = (uint8_t)(b & 0xF);
    i.alpha = (uint8_t)((b & 0x10) != 0);
    i.map = (uint8_t)((b >> 5) & 7);
    s = rc_const_reg(i.reg, f0, f1, written);
    if (s) {
        i.cst = 1;
        if (alpha_port)
            i.cv[0] = rc_map(i.alpha ? s[3] : s[2], i.map);
        else
            for (k = 0; k < 3; k++)
                i.cv[k] = rc_map(i.alpha ? s[3] : s[k], i.map);
    }
    return i;
}

static FcIn fc_dec_in(uint32_t b, const float sf0[4], const float sf1[4],
                      uint32_t written, int g_port)
{
    FcIn i;
    const float *s;
    int k;
    memset(&i, 0, sizeof i);
    i.reg = (uint8_t)(b & 0xF);
    i.alpha = (uint8_t)((b & 0x10) != 0);
    i.inv = (uint8_t)((b & 0x20) != 0);
    s = rc_const_reg(i.reg, sf0, sf1, written);
    if (s) {
        i.cst = 1;
        if (g_port) {
            float G = clamp01(i.alpha ? s[3] : s[2]);
            if (i.inv) G = 1.0f - G;
            i.cv[0] = G;
        } else {
            for (k = 0; k < 3; k++) {
                float x = clamp01(i.alpha ? s[3] : s[k]);
                i.cv[k] = i.inv ? 1.0f - x : x;
            }
        }
    }
    return i;
}

static void rc_plan_build(void)
{
    uint32_t s, k, written = 0;
    int uniq0 = (s_vsh.rc_ctl >> 12) & 1, uniq1 = (s_vsh.rc_ctl >> 16) & 1;
    float f0[8][4], f1[8][4], sf0[4], sf1[4];

    memset(&s_rcp, 0, sizeof s_rcp);
    /* What the reference loop builds before sampling: zeros, and fog in r3. */
    rc_unpack(s_vsh.fog_color, s_rcp.tmpl[3]);
    { float t_ = s_rcp.tmpl[3][0]; s_rcp.tmpl[3][0] = s_rcp.tmpl[3][2]; s_rcp.tmpl[3][2] = t_; }
    s_rcp.tmpl[3][3] = 1.0f;
    s_rcp.fog_var = s_vsh.fog_enable;

    s_rcp.stages = s_vsh.rc_ctl & 0xFF;
    if (s_rcp.stages > 8)
        s_rcp.stages = 8;
    s_rcp.mux_msb = (s_vsh.rc_ctl >> 8) & 1;
    /* Everything any stage writes, first, so fog is only folded if it is
     * never overwritten. */
    for (s = 0; s < s_rcp.stages; s++) {
        uint32_t ocw = s_vsh.rc_cocw[s], aocw = s_vsh.rc_aocw[s];
        written |= (1u << ((ocw >> 4) & 0xF)) | (1u << (ocw & 0xF))
                 | (1u << ((ocw >> 8) & 0xF)) | (1u << ((aocw >> 4) & 0xF))
                 | (1u << (aocw & 0xF)) | (1u << ((aocw >> 8) & 0xF));
        rc_unpack(s_vsh.rc_f0[uniq0 ? s : 0], f0[s]);
        rc_unpack(s_vsh.rc_f1[uniq1 ? s : 0], f1[s]);
    }
    written &= ~1u;
    s_rcp.dirty = written | (1u << 12) | (1u << 14) | (1u << 15);

    for (s = 0; s < s_rcp.stages; s++) {
        RcStage *p = &s_rcp.st[s];
        uint32_t icw = s_vsh.rc_cicw[s], ocw = s_vsh.rc_cocw[s];
        uint32_t aicw = s_vsh.rc_aicw[s], aocw = s_vsh.rc_aocw[s];
        for (k = 0; k < 4; k++) {
            p->rgb[k] = rc_dec_in((icw >> (24 - 8 * k)) & 0xFF, 0, f0[s], f1[s], written);
            p->a[k]   = rc_dec_in((aicw >> (24 - 8 * k)) & 0xFF, 1, f0[s], f1[s], written);
        }
        p->dot_ab = (uint8_t)((ocw >> 13) & 1);
        p->dot_cd = (uint8_t)((ocw >> 12) & 1);
        p->mux    = (uint8_t)((ocw >> 14) & 1);
        p->op     = (uint8_t)((ocw >> 15) & 7);
        p->ab     = (uint8_t)((ocw >> 4) & 0xF);
        p->cd     = (uint8_t)( ocw       & 0xF);
        p->sum    = (uint8_t)((ocw >> 8) & 0xF);
        p->b2a_ab = (uint8_t)(((ocw >> 19) & 1) && p->ab);
        p->b2a_cd = (uint8_t)(((ocw >> 18) & 1) && p->cd);
        p->amux   = (uint8_t)((aocw >> 14) & 1);   /* as the RGB word */
        p->aop    = (uint8_t)((aocw >> 15) & 7);
        p->aab    = (uint8_t)((aocw >> 4) & 0xF);
        p->acd    = (uint8_t)( aocw       & 0xF);
        p->asum   = (uint8_t)((aocw >> 8) & 0xF);
        p->need_ab  = p->ab || p->sum;
        p->need_cd  = p->cd || p->sum;
        p->need_aab = p->aab || p->asum;
        p->need_acd = p->acd || p->asum;
        p->need_sel = (p->sum && p->mux) || (p->asum && p->amux);
    }
    {
        uint32_t c0 = s_vsh.rc_fcw0, c1 = s_vsh.rc_fcw1;
        rc_unpack(s_vsh.rc_sf[0], sf0);
        rc_unpack(s_vsh.rc_sf[1], sf1);
        s_rcp.e = fc_dec_in((c1 >> 24) & 0xFF, sf0, sf1, written, 0);
        s_rcp.f = fc_dec_in((c1 >> 16) & 0xFF, sf0, sf1, written, 0);
        s_rcp.g = fc_dec_in((c1 >> 8) & 0xFF, sf0, sf1, written, 1);
        s_rcp.a = fc_dec_in((c0 >> 24) & 0xFF, sf0, sf1, written, 0);
        s_rcp.b = fc_dec_in((c0 >> 16) & 0xFF, sf0, sf1, written, 0);
        s_rcp.c = fc_dec_in((c0 >> 8) & 0xFF, sf0, sf1, written, 0);
        s_rcp.d = fc_dec_in( c0       & 0xFF, sf0, sf1, written, 0);
        s_rcp.v1_inv = (c1 & 0x40) != 0;
        s_rcp.r0_inv = (c1 & 0x20) != 0;
        s_rcp.sum_clamp = (c1 & 0x80) != 0;
    }
}

/* rc_map with the mapping known: the switch inside it folds away. */
#define RC_MAP_ALL(map, X, N, OUT) do { int k_; switch (map) { \
    case 0: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 0); break; \
    case 1: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 1); break; \
    case 2: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 2); break; \
    case 3: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 3); break; \
    case 4: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 4); break; \
    case 5: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 5); break; \
    case 6: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 6); break; \
    default: for (k_ = 0; k_ < (N); k_++) (OUT)[k_] = rc_map((X)[k_], 7); break; \
    } } while (0)

static inline const float *rc_in_rgb_fast(float r[16][4], const RcIn *in, float v[3])
{
    const float *s;
    float x[3];
    if (in->cst)
        return in->cv;
    s = r[in->reg];
    if (in->alpha) x[0] = x[1] = x[2] = s[3];
    else { x[0] = s[0]; x[1] = s[1]; x[2] = s[2]; }
    RC_MAP_ALL(in->map, x, 3, v);
    return v;
}

static inline float rc_in_a_fast(float r[16][4], const RcIn *in)
{
    const float *s;
    float x, v;
    if (in->cst)
        return in->cv[0];
    s = r[in->reg];
    x = in->alpha ? s[3] : s[2];
    RC_MAP_ALL(in->map, &x, 1, &v);
    return v;
}

/* rc_op on three values, the output mapping known. */
static inline void rc_op3(float v[3], uint32_t op)
{
    int k;
    switch (op) {
    case 1:  for (k = 0; k < 3; k++) v[k] = rc_op(v[k], 1); break;
    case 2:  for (k = 0; k < 3; k++) v[k] = rc_op(v[k], 2); break;
    case 3:  for (k = 0; k < 3; k++) v[k] = rc_op(v[k], 3); break;
    case 4:  for (k = 0; k < 3; k++) v[k] = rc_op(v[k], 4); break;
    case 6:  for (k = 0; k < 3; k++) v[k] = rc_op(v[k], 6); break;
    default: for (k = 0; k < 3; k++) v[k] = rc_op(v[k], 0); break;
    }
}

static inline const float *fc_in_fast(float r[16][4], const FcIn *in, float v[3])
{
    const float *s;
    int k;
    if (in->cst)
        return in->cv;
    s = r[in->reg];
    for (k = 0; k < 3; k++) {
        float x = clamp01(in->alpha ? s[3] : s[k]);
        v[k] = in->inv ? 1.0f - x : x;
    }
    return v;
}

/* rc_eval, on the plan. The same operations on the same values, in place:
 * every input of a stage is read before any of its outputs is written. */
static void rc_eval_fast(float r[16][4], float out[4])
{
    float Ab[3], Bb[3], Cb[3], Db[3], Eb[3], Fb[3];
    const float *A, *B, *C, *D, *E, *F;
    uint32_t s;
    int k;

    for (s = 0; s < s_rcp.stages; s++) {
        const RcStage *p = &s_rcp.st[s];
        float ab[3], cd[3], sum[3], aab = 0.0f, acd = 0.0f, asum = 0.0f;
        int sel = 0;

        if (p->need_sel)
            sel = s_rcp.mux_msb ? r[12][3] >= 0.5f
                                : (((uint32_t)(clamp01(r[12][3]) * 255.0f + 0.5f)) & 1u);
        if (p->need_ab) {
            A = rc_in_rgb_fast(r, &p->rgb[0], Ab);
            B = rc_in_rgb_fast(r, &p->rgb[1], Bb);
            if (p->dot_ab) {
                float d = A[0]*B[0] + A[1]*B[1] + A[2]*B[2];
                ab[0] = ab[1] = ab[2] = d;
            } else {
                for (k = 0; k < 3; k++) ab[k] = A[k] * B[k];
            }
        }
        if (p->need_cd) {
            C = rc_in_rgb_fast(r, &p->rgb[2], Cb);
            D = rc_in_rgb_fast(r, &p->rgb[3], Db);
            if (p->dot_cd) {
                float d = C[0]*D[0] + C[1]*D[1] + C[2]*D[2];
                cd[0] = cd[1] = cd[2] = d;
            } else {
                for (k = 0; k < 3; k++) cd[k] = C[k] * D[k];
            }
        }
        if (p->sum) {
            for (k = 0; k < 3; k++)
                sum[k] = p->mux ? (sel ? cd[k] : ab[k]) : ab[k] + cd[k];
            rc_op3(sum, p->op);
        }
        if (p->ab) rc_op3(ab, p->op);
        if (p->cd) rc_op3(cd, p->op);

        /* Alpha inputs are read before any output lands. */
        if (p->need_aab)
            aab = rc_in_a_fast(r, &p->a[0]) * rc_in_a_fast(r, &p->a[1]);
        if (p->need_acd)
            acd = rc_in_a_fast(r, &p->a[2]) * rc_in_a_fast(r, &p->a[3]);
        if (p->asum)
            asum = p->amux ? (sel ? acd : aab) : aab + acd;

        if (p->ab)  memcpy(r[p->ab], ab, sizeof ab);
        if (p->cd)  memcpy(r[p->cd], cd, sizeof cd);
        if (p->sum) memcpy(r[p->sum], sum, sizeof sum);
        if (p->aab)  r[p->aab][3]  = rc_op(aab,  p->aop);
        if (p->acd)  r[p->acd][3]  = rc_op(acd,  p->aop);
        if (p->asum) r[p->asum][3] = rc_op(asum, p->aop);
        if (p->b2a_ab) r[p->ab][3] = ab[2];
        if (p->b2a_cd) r[p->cd][3] = cd[2];
    }

    {
        float v1[3], r0[3], G;
        E = fc_in_fast(r, &s_rcp.e, Eb);
        F = fc_in_fast(r, &s_rcp.f, Fb);
        for (k = 0; k < 3; k++) {
            v1[k] = clamp01(r[5][k]);  if (s_rcp.v1_inv) v1[k] = 1.0f - v1[k];
            r0[k] = clamp01(r[12][k]); if (s_rcp.r0_inv) r0[k] = 1.0f - r0[k];
            r[14][k] = v1[k] + r0[k];
            if (s_rcp.sum_clamp) r[14][k] = clamp01(r[14][k]);
            r[15][k] = E[k] * F[k];
        }
        r[14][3] = r[15][3] = 0.0f;
        A = fc_in_fast(r, &s_rcp.a, Ab); B = fc_in_fast(r, &s_rcp.b, Bb);
        C = fc_in_fast(r, &s_rcp.c, Cb); D = fc_in_fast(r, &s_rcp.d, Db);
        if (s_rcp.g.cst) {
            G = s_rcp.g.cv[0];
        } else {
            const float *sg = r[s_rcp.g.reg];
            G = clamp01(s_rcp.g.alpha ? sg[3] : sg[2]);
            if (s_rcp.g.inv) G = 1.0f - G;
        }
        for (k = 0; k < 3; k++)
            out[k] = clamp01(A[k] * B[k] + (1.0f - A[k]) * C[k] + D[k]);
        out[3] = G;
    }
}


void old_load(const uint32_t *ci,const uint32_t *ai,const uint32_t *co,const uint32_t *ao,const uint32_t *f0,const uint32_t *f1,uint32_t ctl,uint32_t fc0,uint32_t fc1,uint32_t sf0,uint32_t sf1,uint32_t fogc,int fogv){
 memcpy(s_vsh.rc_cicw,ci,32);memcpy(s_vsh.rc_aicw,ai,32);memcpy(s_vsh.rc_cocw,co,32);memcpy(s_vsh.rc_aocw,ao,32);memcpy(s_vsh.rc_f0,f0,32);memcpy(s_vsh.rc_f1,f1,32);
 s_vsh.rc_ctl=ctl;s_vsh.rc_fcw0=fc0;s_vsh.rc_fcw1=fc1;s_vsh.rc_sf[0]=sf0;s_vsh.rc_sf[1]=sf1;s_vsh.fog_color=fogc;s_vsh.fog_enable=fogv; rc_plan_build();}
void old_eval(float r[16][4], float o[4]){ rc_eval(r,o); }
void old_fast(float r[16][4], float o[4]){ rc_eval_fast(r,o); }
const float *old_tmpl(void){ return &s_rcp.tmpl[0][0]; }
uint32_t old_dirty(void){ return s_rcp.dirty; }
