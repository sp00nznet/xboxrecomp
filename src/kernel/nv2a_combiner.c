/**
 * NV2A register combiners. See nv2a_combiner.h.
 */
#include "nv2a_combiner.h"

#include <string.h>

void nv2a_rc_unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* PS_INPUTMAPPING, psh.c get_input_var. */
static inline float rc_map(float x, uint32_t map)
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

/* PS_COMBINEROUTPUT scale/bias (psh.c get_output), then the [-1,1] clamp. */
static inline float rc_op(float x, uint32_t op)
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

static void rc_in_rgb(float r[16][4], uint32_t b, float v[3])
{
    const float *s = r[NV2A_RC_IN_REG(b)];
    uint32_t map = NV2A_RC_IN_MAP(b);
    int k;
    for (k = 0; k < 3; k++)
        v[k] = rc_map(NV2A_RC_IN_ALPHA(b) ? s[3] : s[k], map);
}

static float rc_in_a(float r[16][4], uint32_t b)
{
    const float *s = r[NV2A_RC_IN_REG(b)];
    return rc_map(NV2A_RC_IN_ALPHA(b) ? s[3] : s[2], NV2A_RC_IN_MAP(b));
}

/* A final-combiner input: clamped, optionally inverted. */
static void fc_in(float r[16][4], uint32_t b, float v[3])
{
    const float *s = r[NV2A_RC_IN_REG(b)];
    int k;
    for (k = 0; k < 3; k++) {
        float x = clamp01(NV2A_RC_IN_ALPHA(b) ? s[3] : s[k]);
        v[k] = NV2A_RC_FC_IN_INV(b) ? 1.0f - x : x;
    }
}

static float fc_in_g(float r[16][4], uint32_t b)
{
    const float *s = r[NV2A_RC_IN_REG(b)];
    float G = clamp01(NV2A_RC_IN_ALPHA(b) ? s[3] : s[2]);
    return NV2A_RC_FC_IN_INV(b) ? 1.0f - G : G;
}

/* The mux select: r0.a's MSB, or its LSB as an 8-bit value. */
static inline int rc_sel(int mux_msb, float r0a)
{
    return mux_msb ? r0a >= 0.5f
                   : (int)(((uint32_t)(clamp01(r0a) * 255.0f + 0.5f)) & 1u);
}

/* The V1+R0 and E*F registers. */
static inline void fc_specials(float r[16][4], const float E[3], const float F[3],
                               uint32_t c1)
{
    int k;
    for (k = 0; k < 3; k++) {
        float v1 = clamp01(r[NV2A_RC_REG_V1][k]);
        float r0 = clamp01(r[NV2A_RC_REG_R0][k]);
        if (c1 & NV2A_RC_FC1_V1_INV) v1 = 1.0f - v1;
        if (c1 & NV2A_RC_FC1_R0_INV) r0 = 1.0f - r0;
        r[NV2A_RC_REG_SUM][k] = v1 + r0;
        if (c1 & NV2A_RC_FC1_SUM_CLAMP)
            r[NV2A_RC_REG_SUM][k] = clamp01(r[NV2A_RC_REG_SUM][k]);
        r[NV2A_RC_REG_EF][k] = E[k] * F[k];
    }
    r[NV2A_RC_REG_SUM][3] = r[NV2A_RC_REG_EF][3] = 0.0f;
}

void nv2a_rc_eval_regs(const Nv2aCombiner *rc, float r[16][4], float out[4],
                       FILE *trace)
{
    uint32_t stages = NV2A_RC_CTL_STAGES(rc->control), s;
    int mux_msb = (int)NV2A_RC_CTL_MUX_MSB(rc->control);
    int uniq0 = (int)NV2A_RC_CTL_UNIQ_C0(rc->control);
    int uniq1 = (int)NV2A_RC_CTL_UNIQ_C1(rc->control);
    float nr[16][4], A[3], B[3], C[3], D[3], E[3], F[3];
    int k;

    if (stages > 8)
        stages = 8;
    for (s = 0; s < stages; s++) {
        uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
        uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
        uint32_t op = NV2A_RC_OUT_MAP(ocw), aop = NV2A_RC_OUT_MAP(aocw);
        float ab[3], cd[3], sum[3], aab, acd, asum;
        int sel;

        nv2a_rc_unpack(rc->factor0[uniq0 ? s : 0], r[NV2A_RC_REG_C0]);
        nv2a_rc_unpack(rc->factor1[uniq1 ? s : 0], r[NV2A_RC_REG_C1]);
        memcpy(nr, r, sizeof nr);
        sel = rc_sel(mux_msb, r[NV2A_RC_REG_R0][3]);

        /* colour */
        rc_in_rgb(r, NV2A_RC_IN(icw, 0), A); rc_in_rgb(r, NV2A_RC_IN(icw, 1), B);
        rc_in_rgb(r, NV2A_RC_IN(icw, 2), C); rc_in_rgb(r, NV2A_RC_IN(icw, 3), D);
        if (NV2A_RC_OUT_AB_DOT(ocw)) {
            float d = A[0]*B[0] + A[1]*B[1] + A[2]*B[2];
            ab[0] = ab[1] = ab[2] = d;
        } else {
            for (k = 0; k < 3; k++) ab[k] = A[k] * B[k];
        }
        if (NV2A_RC_OUT_CD_DOT(ocw)) {
            float d = C[0]*D[0] + C[1]*D[1] + C[2]*D[2];
            cd[0] = cd[1] = cd[2] = d;
        } else {
            for (k = 0; k < 3; k++) cd[k] = C[k] * D[k];
        }
        for (k = 0; k < 3; k++) {
            sum[k] = NV2A_RC_OUT_MUX(ocw) ? (sel ? cd[k] : ab[k]) : ab[k] + cd[k];
            ab[k]  = rc_op(ab[k],  op);
            cd[k]  = rc_op(cd[k],  op);
            sum[k] = rc_op(sum[k], op);
        }
        if (NV2A_RC_OUT_AB(ocw))  memcpy(nr[NV2A_RC_OUT_AB(ocw)], ab, sizeof ab);
        if (NV2A_RC_OUT_CD(ocw))  memcpy(nr[NV2A_RC_OUT_CD(ocw)], cd, sizeof cd);
        if (NV2A_RC_OUT_SUM(ocw)) memcpy(nr[NV2A_RC_OUT_SUM(ocw)], sum, sizeof sum);

        /* alpha */
        aab = rc_in_a(r, NV2A_RC_IN(aicw, 0)) * rc_in_a(r, NV2A_RC_IN(aicw, 1));
        acd = rc_in_a(r, NV2A_RC_IN(aicw, 2)) * rc_in_a(r, NV2A_RC_IN(aicw, 3));
        asum = NV2A_RC_OUT_MUX(aocw) ? (sel ? acd : aab) : aab + acd;
        if (NV2A_RC_OUT_AB(aocw))  nr[NV2A_RC_OUT_AB(aocw)][3]  = rc_op(aab,  aop);
        if (NV2A_RC_OUT_CD(aocw))  nr[NV2A_RC_OUT_CD(aocw)][3]  = rc_op(acd,  aop);
        if (NV2A_RC_OUT_SUM(aocw)) nr[NV2A_RC_OUT_SUM(aocw)][3] = rc_op(asum, aop);

        /* blue to alpha overrides the alpha portion's write */
        if (NV2A_RC_OUT_AB_B2A(ocw) && NV2A_RC_OUT_AB(ocw)) nr[NV2A_RC_OUT_AB(ocw)][3] = ab[2];
        if (NV2A_RC_OUT_CD_B2A(ocw) && NV2A_RC_OUT_CD(ocw)) nr[NV2A_RC_OUT_CD(ocw)][3] = cd[2];
        memcpy(r, nr, sizeof nr);
        if (trace)
            fprintf(trace, "  [WHITE]   stage %u icw %08X ocw %08X aicw %08X aocw %08X"
                           " c0 %08X c1 %08X | r0 %.3f %.3f %.3f %.3f  r1 %.3f %.3f %.3f %.3f"
                           "  t3 %.3f %.3f %.3f %.3f\n", s, icw, ocw, aicw, aocw,
                    rc->factor0[uniq0 ? s : 0], rc->factor1[uniq1 ? s : 0],
                    r[12][0], r[12][1], r[12][2], r[12][3],
                    r[13][0], r[13][1], r[13][2], r[13][3],
                    r[11][0], r[11][1], r[11][2], r[11][3]);
    }

    /* Final combiner: A*B + (1-A)*C + D, alpha from G. */
    {
        uint32_t c0 = rc->final0, c1 = rc->final1;
        float G;
        nv2a_rc_unpack(rc->final_c0, r[NV2A_RC_REG_C0]);
        nv2a_rc_unpack(rc->final_c1, r[NV2A_RC_REG_C1]);
        fc_in(r, NV2A_RC_IN(c1, 0), E);
        fc_in(r, NV2A_RC_IN(c1, 1), F);
        fc_specials(r, E, F, c1);
        fc_in(r, NV2A_RC_IN(c0, 0), A); fc_in(r, NV2A_RC_IN(c0, 1), B);
        fc_in(r, NV2A_RC_IN(c0, 2), C); fc_in(r, NV2A_RC_IN(c0, 3), D);
        G = fc_in_g(r, NV2A_RC_IN(c1, 2));
        for (k = 0; k < 3; k++)
            out[k] = clamp01(A[k] * B[k] + (1.0f - A[k]) * C[k] + D[k]);
        out[3] = G;
        if (trace)
            fprintf(trace, "  [WHITE]   final cw0 %08X cw1 %08X sf %08X %08X | A %.3f %.3f %.3f"
                           " B %.3f %.3f %.3f C %.3f %.3f %.3f D %.3f %.3f %.3f G %.3f"
                           " | fog %.3f %.3f %.3f %.3f v1 %.3f %.3f %.3f -> %.3f %.3f %.3f %.3f\n",
                    c0, c1, rc->final_c0, rc->final_c1, A[0], A[1], A[2], B[0], B[1], B[2],
                    C[0], C[1], C[2], D[0], D[1], D[2], G,
                    r[3][0], r[3][1], r[3][2], r[3][3], r[5][0], r[5][1], r[5][2],
                    out[0], out[1], out[2], out[3]);
    }
}

void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4])
{
    float r[16][4];
    int n;

    memset(r, 0, sizeof r);
    memcpy(r[NV2A_RC_REG_FOG], fog, sizeof r[0]);
    memcpy(r[NV2A_RC_REG_V0], v0, sizeof r[0]);
    memcpy(r[NV2A_RC_REG_V1], v1, sizeof r[0]);
    for (n = 0; n < 4; n++)
        memcpy(r[NV2A_RC_REG_T0 + n], t[n], sizeof r[0]);
    r[NV2A_RC_REG_R0][3] = t[0][3];
    nv2a_rc_eval_regs(rc, r, out, NULL);
}

/* ---- Per-batch plan ---------------------------------------------------- */

/* Registers whose value at a stage's input is the same for every pixel. */
static const float *plan_const_reg(const Nv2aRcPlan *p, uint32_t reg,
                                   const float f0[4], const float f1[4],
                                   uint32_t written)
{
    if (reg == NV2A_RC_REG_ZERO) return p->tmpl[0];
    if (reg == NV2A_RC_REG_C0) return f0;
    if (reg == NV2A_RC_REG_C1) return f1;
    if (reg == NV2A_RC_REG_FOG && !(written & (1u << 3)) && !p->fog_var)
        return p->tmpl[3];
    return NULL;
}

static Nv2aRcPlanIn plan_in(const Nv2aRcPlan *p, uint32_t b, int alpha_port,
                            const float f0[4], const float f1[4], uint32_t written)
{
    Nv2aRcPlanIn i;
    const float *s;
    int k;
    memset(&i, 0, sizeof i);
    i.reg = (uint8_t)NV2A_RC_IN_REG(b);
    i.alpha = (uint8_t)NV2A_RC_IN_ALPHA(b);
    i.map = (uint8_t)NV2A_RC_IN_MAP(b);
    s = plan_const_reg(p, i.reg, f0, f1, written);
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

/* A final-combiner input; `map` holds the invert bit. */
static Nv2aRcPlanIn plan_fc_in(const Nv2aRcPlan *p, uint32_t b, const float sf0[4],
                               const float sf1[4], uint32_t written, int g_port)
{
    Nv2aRcPlanIn i;
    const float *s;
    int k;
    memset(&i, 0, sizeof i);
    i.reg = (uint8_t)NV2A_RC_IN_REG(b);
    i.alpha = (uint8_t)NV2A_RC_IN_ALPHA(b);
    i.map = (uint8_t)NV2A_RC_FC_IN_INV(b);
    s = plan_const_reg(p, i.reg, sf0, sf1, written);
    if (s) {
        i.cst = 1;
        if (g_port) {
            float G = clamp01(i.alpha ? s[3] : s[2]);
            i.cv[0] = i.map ? 1.0f - G : G;
        } else {
            for (k = 0; k < 3; k++) {
                float x = clamp01(i.alpha ? s[3] : s[k]);
                i.cv[k] = i.map ? 1.0f - x : x;
            }
        }
    }
    return i;
}

void nv2a_rc_plan_build(Nv2aRcPlan *p, const Nv2aCombiner *rc,
                        const float fog[4], int fog_var)
{
    uint32_t s, k, written = 0;
    int uniq0 = (int)NV2A_RC_CTL_UNIQ_C0(rc->control);
    int uniq1 = (int)NV2A_RC_CTL_UNIQ_C1(rc->control);
    float f0[8][4], f1[8][4], sf0[4], sf1[4];

    memset(p, 0, sizeof *p);
    memcpy(p->tmpl[NV2A_RC_REG_FOG], fog, sizeof p->tmpl[0]);
    p->fog_var = fog_var;

    p->stages = NV2A_RC_CTL_STAGES(rc->control);
    if (p->stages > 8)
        p->stages = 8;
    p->mux_msb = (int)NV2A_RC_CTL_MUX_MSB(rc->control);
    /* Everything any stage writes, first, so fog is only folded if it is
     * never overwritten. */
    for (s = 0; s < p->stages; s++) {
        uint32_t ocw = rc->color_ocw[s], aocw = rc->alpha_ocw[s];
        written |= (1u << NV2A_RC_OUT_AB(ocw)) | (1u << NV2A_RC_OUT_CD(ocw))
                 | (1u << NV2A_RC_OUT_SUM(ocw)) | (1u << NV2A_RC_OUT_AB(aocw))
                 | (1u << NV2A_RC_OUT_CD(aocw)) | (1u << NV2A_RC_OUT_SUM(aocw));
        nv2a_rc_unpack(rc->factor0[uniq0 ? s : 0], f0[s]);
        nv2a_rc_unpack(rc->factor1[uniq1 ? s : 0], f1[s]);
    }
    written &= ~1u;
    p->dirty = written | (1u << NV2A_RC_REG_R0) | (1u << NV2A_RC_REG_SUM)
             | (1u << NV2A_RC_REG_EF);

    for (s = 0; s < p->stages; s++) {
        Nv2aRcPlanStage *st = &p->st[s];
        uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
        uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
        for (k = 0; k < 4; k++) {
            st->rgb[k] = plan_in(p, NV2A_RC_IN(icw, k), 0, f0[s], f1[s], written);
            st->a[k]   = plan_in(p, NV2A_RC_IN(aicw, k), 1, f0[s], f1[s], written);
        }
        st->dot_ab = (uint8_t)NV2A_RC_OUT_AB_DOT(ocw);
        st->dot_cd = (uint8_t)NV2A_RC_OUT_CD_DOT(ocw);
        st->mux    = (uint8_t)NV2A_RC_OUT_MUX(ocw);
        st->op     = (uint8_t)NV2A_RC_OUT_MAP(ocw);
        st->ab     = (uint8_t)NV2A_RC_OUT_AB(ocw);
        st->cd     = (uint8_t)NV2A_RC_OUT_CD(ocw);
        st->sum    = (uint8_t)NV2A_RC_OUT_SUM(ocw);
        st->b2a_ab = (uint8_t)(NV2A_RC_OUT_AB_B2A(ocw) && st->ab);
        st->b2a_cd = (uint8_t)(NV2A_RC_OUT_CD_B2A(ocw) && st->cd);
        st->amux   = (uint8_t)NV2A_RC_OUT_MUX(aocw);
        st->aop    = (uint8_t)NV2A_RC_OUT_MAP(aocw);
        st->aab    = (uint8_t)NV2A_RC_OUT_AB(aocw);
        st->acd    = (uint8_t)NV2A_RC_OUT_CD(aocw);
        st->asum   = (uint8_t)NV2A_RC_OUT_SUM(aocw);
        st->need_ab  = st->ab || st->sum;
        st->need_cd  = st->cd || st->sum;
        st->need_aab = st->aab || st->asum;
        st->need_acd = st->acd || st->asum;
        st->need_sel = (st->sum && st->mux) || (st->asum && st->amux);
    }
    {
        uint32_t c0 = rc->final0, c1 = rc->final1;
        nv2a_rc_unpack(rc->final_c0, sf0);
        nv2a_rc_unpack(rc->final_c1, sf1);
        p->e = plan_fc_in(p, NV2A_RC_IN(c1, 0), sf0, sf1, written, 0);
        p->f = plan_fc_in(p, NV2A_RC_IN(c1, 1), sf0, sf1, written, 0);
        p->g = plan_fc_in(p, NV2A_RC_IN(c1, 2), sf0, sf1, written, 1);
        p->a = plan_fc_in(p, NV2A_RC_IN(c0, 0), sf0, sf1, written, 0);
        p->b = plan_fc_in(p, NV2A_RC_IN(c0, 1), sf0, sf1, written, 0);
        p->c = plan_fc_in(p, NV2A_RC_IN(c0, 2), sf0, sf1, written, 0);
        p->d = plan_fc_in(p, NV2A_RC_IN(c0, 3), sf0, sf1, written, 0);
        p->v1_inv = (c1 & NV2A_RC_FC1_V1_INV) != 0;
        p->r0_inv = (c1 & NV2A_RC_FC1_R0_INV) != 0;
        p->sum_clamp = (c1 & NV2A_RC_FC1_SUM_CLAMP) != 0;
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

static inline const float *plan_rgb(float r[16][4], const Nv2aRcPlanIn *in, float v[3])
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

static inline float plan_a(float r[16][4], const Nv2aRcPlanIn *in)
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

static inline const float *plan_fc(float r[16][4], const Nv2aRcPlanIn *in, float v[3])
{
    const float *s;
    int k;
    if (in->cst)
        return in->cv;
    s = r[in->reg];
    for (k = 0; k < 3; k++) {
        float x = clamp01(in->alpha ? s[3] : s[k]);
        v[k] = in->map ? 1.0f - x : x;
    }
    return v;
}

void nv2a_rc_plan_eval(const Nv2aRcPlan *p, float r[16][4], float out[4])
{
    float Ab[3], Bb[3], Cb[3], Db[3], Eb[3], Fb[3];
    const float *A, *B, *C, *D, *E, *F;
    uint32_t s;
    int k;

    for (s = 0; s < p->stages; s++) {
        const Nv2aRcPlanStage *st = &p->st[s];
        float ab[3], cd[3], sum[3], aab = 0.0f, acd = 0.0f, asum = 0.0f;
        int sel = 0;

        if (st->need_sel)
            sel = rc_sel(p->mux_msb, r[NV2A_RC_REG_R0][3]);
        if (st->need_ab) {
            A = plan_rgb(r, &st->rgb[0], Ab);
            B = plan_rgb(r, &st->rgb[1], Bb);
            if (st->dot_ab) {
                float d = A[0]*B[0] + A[1]*B[1] + A[2]*B[2];
                ab[0] = ab[1] = ab[2] = d;
            } else {
                for (k = 0; k < 3; k++) ab[k] = A[k] * B[k];
            }
        }
        if (st->need_cd) {
            C = plan_rgb(r, &st->rgb[2], Cb);
            D = plan_rgb(r, &st->rgb[3], Db);
            if (st->dot_cd) {
                float d = C[0]*D[0] + C[1]*D[1] + C[2]*D[2];
                cd[0] = cd[1] = cd[2] = d;
            } else {
                for (k = 0; k < 3; k++) cd[k] = C[k] * D[k];
            }
        }
        if (st->sum) {
            for (k = 0; k < 3; k++)
                sum[k] = st->mux ? (sel ? cd[k] : ab[k]) : ab[k] + cd[k];
            rc_op3(sum, st->op);
        }
        if (st->ab) rc_op3(ab, st->op);
        if (st->cd) rc_op3(cd, st->op);

        /* Alpha inputs are read before any output lands. */
        if (st->need_aab)
            aab = plan_a(r, &st->a[0]) * plan_a(r, &st->a[1]);
        if (st->need_acd)
            acd = plan_a(r, &st->a[2]) * plan_a(r, &st->a[3]);
        if (st->asum)
            asum = st->amux ? (sel ? acd : aab) : aab + acd;

        if (st->ab)  memcpy(r[st->ab], ab, sizeof ab);
        if (st->cd)  memcpy(r[st->cd], cd, sizeof cd);
        if (st->sum) memcpy(r[st->sum], sum, sizeof sum);
        if (st->aab)  r[st->aab][3]  = rc_op(aab,  st->aop);
        if (st->acd)  r[st->acd][3]  = rc_op(acd,  st->aop);
        if (st->asum) r[st->asum][3] = rc_op(asum, st->aop);
        if (st->b2a_ab) r[st->ab][3] = ab[2];
        if (st->b2a_cd) r[st->cd][3] = cd[2];
    }

    {
        float v1[3], r0[3], G;
        E = plan_fc(r, &p->e, Eb);
        F = plan_fc(r, &p->f, Fb);
        for (k = 0; k < 3; k++) {
            v1[k] = clamp01(r[5][k]);  if (p->v1_inv) v1[k] = 1.0f - v1[k];
            r0[k] = clamp01(r[12][k]); if (p->r0_inv) r0[k] = 1.0f - r0[k];
            r[14][k] = v1[k] + r0[k];
            if (p->sum_clamp) r[14][k] = clamp01(r[14][k]);
            r[15][k] = E[k] * F[k];
        }
        r[14][3] = r[15][3] = 0.0f;
        A = plan_fc(r, &p->a, Ab); B = plan_fc(r, &p->b, Bb);
        C = plan_fc(r, &p->c, Cb); D = plan_fc(r, &p->d, Db);
        if (p->g.cst) {
            G = p->g.cv[0];
        } else {
            const float *sg = r[p->g.reg];
            G = clamp01(p->g.alpha ? sg[3] : sg[2]);
            if (p->g.map) G = 1.0f - G;
        }
        for (k = 0; k < 3; k++)
            out[k] = clamp01(A[k] * B[k] + (1.0f - A[k]) * C[k] + D[k]);
        out[3] = G;
    }
}
