/*
 * nv2a_combiner_fuzz - the combiner decoders against frozen oracles.
 *
 * nv2a_combiner.c replaced the evaluators that lived in pb_exec, and the
 * parser the HLSL and MSL generators share (d3d8_combiners_parse.c) now
 * decodes through nv2a_combiner.h's field table. The previous versions are
 * kept under oracle/; every random register set must give:
 *
 *   - evaluator: the same output and register file from
 *     nv2a_rc_eval_regs as the old reference, and from nv2a_rc_plan_eval as
 *     the old plan (and the same plan template and dirty set), bit for bit;
 *   - parser: the same NV2ACombinerState.
 *
 * FUZZ_ITERS overrides the iteration count (default 300000 / 200000).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_combiner.h"
#include "d3d8_combiners_parse.h"

void old_load(const uint32_t *ci, const uint32_t *ai, const uint32_t *co,
              const uint32_t *ao, const uint32_t *f0, const uint32_t *f1,
              uint32_t ctl, uint32_t fc0, uint32_t fc1, uint32_t sf0,
              uint32_t sf1, uint32_t fogc, int fogv);
void old_eval(float r[16][4], float o[4]);
void old_fast(float r[16][4], float o[4]);
const float *old_tmpl(void);
uint32_t old_dirty(void);
void oracle_combiners_from_regs(const uint32_t cicw[8], const uint32_t aicw[8],
                                const uint32_t cocw[8], const uint32_t aocw[8],
                                uint32_t control, uint32_t fcw0, uint32_t fcw1,
                                uint32_t stage_modes, NV2ACombinerState *state);

static uint64_t s_rng = 0x1234567ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

/* Colours in [0,1], and now and then a signed value outside it. */
static float rval(void)
{
    uint32_t r = rnd();
    if ((r & 15) == 0)
        return (float)((int)(r >> 8) % 4000) / 1000.0f - 2.0f;
    return (float)((r >> 8) & 0xFF) / 255.0f;
}

static long iters(long dflt)
{
    const char *e = getenv("FUZZ_ITERS");
    return e ? atol(e) : dflt;
}

static long fuzz_eval(void)
{
    long it, n_it = iters(300000), bad = 0;

    for (it = 0; it < n_it; it++) {
        Nv2aCombiner rc;
        Nv2aRcPlan p;
        float fog[4], r[16][4], r1[16][4], r2[16][4], r3[16][4], r4[16][4];
        float o1[4], o2[4], o3[4], o4[4], t;
        int i, k, fv = (int)(rnd() & 1);
        uint32_t fogc = rnd();

        memset(&rc, 0, sizeof rc);
        for (i = 0; i < 8; i++) {
            rc.color_icw[i] = rnd();
            rc.alpha_icw[i] = rnd();
            rc.color_ocw[i] = rnd() & 0xFFFFF;
            rc.alpha_ocw[i] = rnd() & 0x3CFFF;
            rc.factor0[i] = rnd();
            rc.factor1[i] = rnd();
        }
        rc.final0 = rnd(); rc.final1 = rnd();
        rc.final_c0 = rnd(); rc.final_c1 = rnd();
        rc.control = (rnd() % 9) | (rnd() & 0x11100);
        old_load(rc.color_icw, rc.alpha_icw, rc.color_ocw, rc.alpha_ocw,
                 rc.factor0, rc.factor1, rc.control, rc.final0, rc.final1,
                 rc.final_c0, rc.final_c1, fogc, fv);
        nv2a_rc_unpack(fogc, fog);
        t = fog[0]; fog[0] = fog[2]; fog[2] = t;
        fog[3] = 1.0f;
        nv2a_rc_plan_build(&p, &rc, fog, fv);
        if ((memcmp(p.tmpl, old_tmpl(), sizeof p.tmpl) || p.dirty != old_dirty())
                && bad++ < 3)
            printf("FAIL: plan template or dirty set differs, iteration %ld\n", it);

        memset(r, 0, sizeof r);
        memcpy(r[3], fog, sizeof fog);
        if (fv)
            r[3][3] = rval();
        for (k = 0; k < 4; k++) { r[4][k] = rval(); r[5][k] = rval(); }
        for (i = 0; i < 4; i++)
            for (k = 0; k < 4; k++)
                r[8 + i][k] = rval();
        r[12][3] = r[8][3];
        memcpy(r1, r, sizeof r); memcpy(r2, r, sizeof r);
        memcpy(r3, r, sizeof r); memcpy(r4, r, sizeof r);
        old_eval(r1, o1);
        nv2a_rc_eval_regs(&rc, r2, o2, NULL);
        old_fast(r3, o3);
        nv2a_rc_plan_eval(&p, r4, o4);
        if ((memcmp(o1, o2, sizeof o1) || memcmp(r1, r2, sizeof r1)) && bad++ < 3)
            printf("FAIL: reference differs, iteration %ld\n", it);
        if ((memcmp(o3, o4, sizeof o3) || memcmp(r3, r4, sizeof r3)) && bad++ < 3)
            printf("FAIL: plan differs, iteration %ld\n", it);
        if (memcmp(o1, o4, sizeof o1) && bad++ < 3)
            printf("FAIL: plan vs reference differs, iteration %ld\n", it);
    }
    printf("evaluator: %ld register sets, %ld mismatches\n", n_it, bad);
    return bad;
}

static long fuzz_parse(void)
{
    long it, n_it = iters(200000), bad = 0;

    for (it = 0; it < n_it; it++) {
        uint32_t w[4][8], ctl, f0, f1, sm;
        NV2ACombinerState a, b;
        int i, j;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 8; j++)
                w[i][j] = rnd();
        ctl = rnd() & 0x1FFFF; f0 = rnd(); f1 = rnd(); sm = rnd();
        if (it & 1)
            ctl = (ctl & ~0xFFu) | (rnd() % 9);
        memset(&a, 0xAB, sizeof a);
        memset(&b, 0xAB, sizeof b);
        oracle_combiners_from_regs(w[0], w[1], w[2], w[3], ctl, f0, f1, sm, &a);
        d3d8_combiners_from_regs(w[0], w[1], w[2], w[3], ctl, f0, f1, sm, &b);
        if (memcmp(&a, &b, sizeof a) && bad++ < 3)
            printf("FAIL: parser differs, iteration %ld\n", it);
    }
    printf("parser: %ld register sets, %ld mismatches\n", n_it, bad);
    return bad;
}

int main(void)
{
    long bad = fuzz_eval() + fuzz_parse();
    puts(bad ? "nv2a_combiner_fuzz: FAILED" : "nv2a_combiner_fuzz: all passed");
    return bad != 0;
}
