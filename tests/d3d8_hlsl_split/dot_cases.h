/* The texture shader's dot-product cases the combiner generators are checked
 * on: Quantum Redshift's water with and without its cube map, the
 * DOT_RFLCT_DIFF/SPEC pair, DOT_STR_3D and _CUBE, DOT_ST under the
 * DEPENDENT_AR/GB reads, the modes that read no texture (DOT_ZW,
 * SPEC_CONST, BRDF) and a CUBEMAP stage. Shared by d3d8_hlsl_split (the
 * HLSL text, every host), d3d8_msl_split (the MSL, compiled where Metal is)
 * and d3dcompile_smoke (the HLSL through D3DCompile, on Windows), so the
 * three see the same states. */
#ifndef XBR_TESTS_DOT_CASES_H
#define XBR_TESTS_DOT_CASES_H

#include <string.h>
#include "d3d8_combiners_parse.h"

/* modes: 5 bits a stage, as nv2a_ps_stage_key packs them; cube: tex_cube a
 * bit a stage; map: every stage's dot_map; in2: stage 2's tex_input. */
static const struct dot_case {
    const char *name;
    uint32_t    modes;
    int         cube, map, in2;
} k_dot_cases[] = {
    { "rc-dot-water",     1u | 0x11u << 5 | 0x11u << 10 | 0x0Cu << 15, 0x8, 1, 0 },
    { "rc-dot-water-2d",  1u | 0x11u << 5 | 0x11u << 10 | 0x0Cu << 15, 0x0, 1, 0 },
    { "rc-dot-diff-spec", 1u | 0x11u << 5 | 0x0Bu << 10 | 0x0Cu << 15, 0xC, 2, 0 },
    { "rc-dot-str-3d",    1u | 0x11u << 5 | 0x11u << 10 | 0x0Du << 15, 0x0, 3, 0 },
    { "rc-dot-str-cube",  1u | 0x11u << 5 | 0x11u << 10 | 0x0Eu << 15, 0x8, 0, 0 },
    { "rc-dot-dependent", 1u | 0x0Fu << 5 | 0x10u << 10 | 0x09u << 15, 0x0, 3, 1 },
    { "rc-dot-zero",      1u | 0x11u << 5 | 0x0Au << 10 | 0x12u << 15, 0x0, 1, 0 },
    { "rc-dot-brdf",      1u | 0x11u << 5 | 0x08u << 10 | 0x0Au << 15, 0x0, 4, 0 },
    { "rc-cubemap",       3u | 1u << 5,                                0x1, 0, 0 },
};
#define DOT_NCASE ((int)(sizeof k_dot_cases / sizeof k_dot_cases[0]))

/* Case i's combiner state: two general stages that add every texel,
 * t0 + t1 into r0 and t2 + t3 into r1 (each times ONE, zero inverted), and a
 * final combiner that outputs r0 + r1. Whatever texel a dot chain finally
 * samples then reaches the colour. With zeroed registers nothing read
 * t0..t3, and a product such as t0*t1 is still 0 where a dot stage leaves
 * its own texel 0, so D3DCompile stripped the texture shader as dead code
 * and the cases compiled to one bytecode. off is rt_offset (hor+). */
static void dot_case_state(int i, int off, NV2ACombinerState *st)
{
    static const uint32_t zero[8];
    static const uint32_t cicw[8] = { 0x08200920u, 0x0A200B20u }; /* tN * ONE */
    static const uint32_t cocw[8] = { 0x0Cu << 8, 0x0Du << 8 };   /* sum -> r0, r1 */
    const struct dot_case *c = &k_dot_cases[i];
    int k;

    memset(st, 0, sizeof *st);
    d3d8_combiners_from_regs(cicw, zero, cocw, zero, 2u, 0x00000D0Cu /* C=r1 D=r0 */,
                             0u, c->modes, st);
    st->fog_input = 1;
    st->bump_on = 1;
    st->rt_offset = off;
    st->tex_input[2] = c->in2;
    st->tex_cube[0] = c->cube & 1;
    for (k = 1; k < 4; k++) {
        st->dot_map[k] = c->map;
        st->tex_cube[k] = c->cube >> k & 1;
    }
}

/* 1 when case i's later stages sample no texture (DOT_ZW, SPEC_CONST, BRDF
 * read black, design D2), so only t0 reaches the colour and such cases may
 * compile to one bytecode. */
static int dot_case_t0_only(int i)
{
    NV2ACombinerState st;
    int k;

    dot_case_state(i, 0, &st);
    for (k = 1; k < NV2A_MAX_TEXTURES; k++)
        if (nv2a_texmode_samples((int)st.tex_mode[k]))
            return 0;
    return 1;
}

/* FNV-1a of a generated text: the dot cases must each emit their own. */
static uint32_t dot_text_hash(const char *t)
{
    uint32_t h = 2166136261u;
    while (*t)
        h = (h ^ (uint8_t)*t++) * 16777619u;
    return h;
}

/* 1 when hash h already sits in seen[0..n-1]. */
static int dot_text_seen(const uint32_t *seen, int n, uint32_t h)
{
    int j;
    for (j = 0; j < n; j++)
        if (seen[j] == h)
            return 1;
    return 0;
}

#endif
