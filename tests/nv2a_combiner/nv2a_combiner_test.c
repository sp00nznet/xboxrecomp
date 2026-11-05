/* Register combiner programs as D3D writes them, checked against the result
 * the hardware would give; then the per-batch plan against the reference
 * evaluator on random programs, bit for bit. */
#include "nv2a_combiner.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)

enum { ZERO = 0, C0 = 1, V0 = 4, V1 = 5, T0 = 8, T1 = 9, R0 = 12, SUM = 14 };
#define ALPHA 0x10
#define INVERT 0x20        /* unsigned invert: 1 - x */
#define EXPAND 0x40        /* 2x - 1 */
#define ICW(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

/* Values as the rasteriser feeds them: mostly [0,1], some outside. */
static float rval(void)
{
    uint32_t r = rnd();
    if ((r & 15) == 0) return ((int)(r >> 8) % 4000) / 1000.0f - 2.0f;
    return (float)((r >> 8) & 0xFF) / 255.0f;
}

/* nv2a_rc_plan_eval against nv2a_rc_eval_regs. */
static int plan_matches_reference(void)
{
    long it, bad = 0;
    for (it = 0; it < 200000; it++) {
        Nv2aCombiner rc;
        Nv2aRcPlan plan;
        float fog[4], r[16][4], rp[16][4], o1[4], o2[4];
        int fog_var = (int)(rnd() & 1), i, k, reg;

        memset(&rc, 0, sizeof rc);
        for (i = 0; i < 8; i++) {
            rc.color_icw[i] = rnd(); rc.alpha_icw[i] = rnd();
            rc.color_ocw[i] = rnd() & 0xFFFFFu; rc.alpha_ocw[i] = rnd() & 0x3CFFFu;
            rc.factor0[i] = rnd(); rc.factor1[i] = rnd();
        }
        rc.final0 = rnd(); rc.final1 = rnd();
        rc.final_c0 = rnd(); rc.final_c1 = rnd();
        rc.control = (rnd() % 9) | (rnd() & 0x11100u);
        for (k = 0; k < 4; k++) fog[k] = rval();
        fog[3] = 1.0f;
        nv2a_rc_plan_build(&plan, &rc, fog, fog_var);

        /* The pixel's inputs, as the rasteriser fills them. */
        memset(r, 0, sizeof r);
        memcpy(r[3], fog, sizeof fog);
        if (fog_var) r[3][3] = rval();
        for (k = 0; k < 4; k++) { r[4][k] = rval(); r[5][k] = rval(); }
        for (i = 0; i < 4; i++) for (k = 0; k < 4; k++) r[8 + i][k] = rval();
        r[12][3] = r[8][3];

        /* The plan starts from tmpl with its dirty registers reset, after a
         * previous pixel left garbage in them. */
        memcpy(rp, plan.tmpl, sizeof rp);
        for (reg = 0; reg < 16; reg++) for (k = 0; k < 4; k++)
            if (plan.dirty & (1u << reg)) rp[reg][k] = 7.0f;
        for (reg = 0; reg < 16; reg++)
            if (plan.dirty & (1u << reg))
                memcpy(rp[reg], plan.tmpl[reg], sizeof rp[reg]);
        memcpy(rp[4], r[4], sizeof r[4]); memcpy(rp[5], r[5], sizeof r[5]);
        if (fog_var) rp[3][3] = r[3][3];
        for (i = 0; i < 4; i++) memcpy(rp[8 + i], r[8 + i], sizeof r[8]);
        rp[12][3] = r[12][3];

        nv2a_rc_eval_regs(&rc, r, o1, NULL);
        nv2a_rc_plan_eval(&plan, rp, o2);
        if (memcmp(o1, o2, sizeof o1) && bad++ < 5)
            printf("plan mismatch at %ld: %g %g %g %g vs %g %g %g %g\n", it,
                   o1[0], o1[1], o1[2], o1[3], o2[0], o2[1], o2[2], o2[3]);
    }
    return bad != 0;
}

int main(void)
{
    Nv2aCombiner rc;
    float v0[4] = {0.5f, 1.0f, 0.25f, 0.5f}, v1[4] = {0};
    float fog[4] = {0, 0, 0, 1}, t[4][4] = {{0}}, out[4];

    /* D3D MODULATE, one stage: r0 = t0 * v0 (AB into sum via +0), final
     * passes r0 through as D with alpha from r0. */
    memset(&rc, 0, sizeof rc);
    rc.control = 1;
    rc.stage_program = 1;                        /* stage 0: 2D texture */
    rc.color_icw[0] = ICW(T0, V0, ZERO, ZERO);
    rc.alpha_icw[0] = ICW(T0 | ALPHA, V0 | ALPHA, ZERO, ZERO);
    rc.color_ocw[0] = R0 << 8;                   /* sum -> r0 */
    rc.alpha_ocw[0] = R0 << 8;
    rc.final0 = ICW(ZERO, ZERO, ZERO, R0);
    rc.final1 = ICW(ZERO, ZERO, R0 | ALPHA, 0);
    t[0][0] = 0.5f; t[0][1] = 0.5f; t[0][2] = 1.0f; t[0][3] = 1.0f;
    nv2a_rc_eval(&rc, v0, v1, fog, t, out);
    CHECK(NEAR(out[0], 0.25f) && NEAR(out[1], 0.5f) && NEAR(out[2], 0.25f));
    CHECK(NEAR(out[3], 0.5f));

    /* A scene-plus-glow composite: final = lerp(t1, t0, c0.a) with the
     * factor from SPECULAR_FOG_FACTOR, which is c0 in the final stage. */
    memset(&rc, 0, sizeof rc);
    rc.control = 0;
    rc.final0 = ICW(C0 | ALPHA, T0, T1, ZERO);
    rc.final1 = ICW(ZERO, ZERO, ZERO | INVERT, 0);   /* alpha = 1 */
    rc.final_c0 = 0x40000000u;                        /* a = 64/255 */
    t[0][0] = 1.0f; t[1][0] = 0.0f;
    nv2a_rc_eval(&rc, v0, v1, fog, t, out);
    CHECK(NEAR(out[0], 64.0f / 255.0f) && NEAR(out[3], 1.0f));

    /* Expand-normal dot product (bump lighting): dot(2t0-1, 2v0-1) into r0,
     * which the final combiner passes through. */
    memset(&rc, 0, sizeof rc);
    rc.control = 1;
    rc.color_icw[0] = ICW(T0 | EXPAND, V0 | EXPAND, ZERO, ZERO);
    rc.color_ocw[0] = (R0 << 4) | (2u << 12);          /* AB dot -> r0 */
    rc.final0 = ICW(ZERO, ZERO, ZERO, R0);
    t[0][0] = 1.0f; t[0][1] = 0.5f; t[0][2] = 0.5f;    /* (1,0,0) */
    v0[0] = 1.0f; v0[1] = 0.5f; v0[2] = 0.5f;          /* (1,0,0) */
    nv2a_rc_eval(&rc, v0, v1, fog, t, out);
    CHECK(NEAR(out[0], 1.0f) && NEAR(out[1], 1.0f) && NEAR(out[2], 1.0f));

    /* The final combiner runs even when both words are zero (as the HLSL
     * and MSL generators emit it): everything reads zero. */
    rc.final0 = rc.final1 = 0;
    nv2a_rc_eval(&rc, v0, v1, fog, t, out);
    CHECK(out[0] == 0.0f && out[3] == 0.0f);

    CHECK(plan_matches_reference() == 0);

    puts("ok");
    return 0;
}
