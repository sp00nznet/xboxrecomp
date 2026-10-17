/*
 * d3d8_hlsl_split: does the parse -> HLSL pipeline still emit the same text?
 *
 * Feeds NPROG pseudo-random vertex programs (LCG microcode, a random
 * instruction marked final) through d3d8_vsh_parse and
 * d3d8_vsh_generate_hlsl_ex (both flag settings), and NCOMB pseudo-random
 * combiner register sets through d3d8_combiners_from_regs and
 * d3d8_combiners_generate_hlsl. Each case's output (text, or the -1 of an
 * overflow) is hashed (FNV-1a) and compared with its entry in expected.h,
 * recorded from the toolkit before the parsers moved to their own, API-free
 * files (5d3d0f3; the full dumps of that and the split are byte-identical).
 * A mismatch names the case; HLSL_DUMP=<path> writes all the text, so two
 * builds can be diffed directly. After an intended emitter change,
 * HLSL_EXPECT_OUT=<path> writes a new expected.h.
 *
 * Only the API-free headers are used, so this builds and runs on every host.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_hlsl.h"

#define NPROG 400
#define NCOMB 400
#define NCASE (2 * NPROG + NCOMB)
#define EXPECTED_HASH 0x1175d806u      /* all cases, one FNV-1a */
#define EXPECTED_CHARS 7375660u        /* re-recorded: BUMPENVMAP constants in the cbuffer */

#include "expected.h"   /* s_expect[NCASE], or HLSL_NO_EXPECT */

static uint32_t s_seed = 0x2f6b1d3u;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed ^ (s_seed >> 15);
}

static uint32_t s_hash = 0x811c9dc5u;
static unsigned long s_chars;
static FILE *s_dump;
static uint32_t s_case[NCASE];
static int s_ncase;
static int s_bad;

static void absorb(const char *what, int idx, const char *text, int n)
{
    char head[64];
    uint32_t h = 0x811c9dc5u;
    int i, hn = snprintf(head, sizeof head, "// %s %d: %d\n", what, idx, n);
    for (i = 0; i < hn; i++) {
        s_hash ^= (uint8_t)head[i]; s_hash *= 0x01000193u;
        h ^= (uint8_t)head[i]; h *= 0x01000193u;
    }
    if (s_dump) fputs(head, s_dump);
    for (i = 0; i < n; i++) {
        s_hash ^= (uint8_t)text[i]; s_hash *= 0x01000193u;
        h ^= (uint8_t)text[i]; h *= 0x01000193u;
    }
    if (n > 0) {
        s_chars += (unsigned long)n;
        if (s_dump) { fwrite(text, 1, (size_t)n, s_dump); fputc('\n', s_dump); }
    }
#ifndef HLSL_NO_EXPECT
    if (h != s_expect[s_ncase] && ++s_bad <= 20)
        printf("FAIL: %s %d differs (%d chars, hash %08x, expected %08x)\n",
               what, idx, n, h, s_expect[s_ncase]);
#endif
    s_case[s_ncase++] = h;
}

static int write_expect(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f)
        return 0;
    fprintf(f, "/* Per-case FNV-1a of d3d8_hlsl_split's output: %d vsh, %d vsh"
               " screen-space,\n * %d combiner sets, in run order. Written by"
               " HLSL_EXPECT_OUT. */\n", NPROG, NPROG, NCOMB);
    fprintf(f, "static const uint32_t s_expect[%d] = {\n", NCASE);
    for (i = 0; i < NCASE; i++)
        fprintf(f, "%s0x%08xu,%s", i % 6 ? " " : "    ", s_case[i],
                i % 6 == 5 || i == NCASE - 1 ? "\n" : "");
    fprintf(f, "};\n");
    fclose(f);
    return 1;
}

/* Texture-shader modes the hashes above would only catch as "changed": a
 * NONE stage reads (0, 0, 0, 1), as xemu, and CLIPPLANE reads 0 with no
 * texture bound. Stage 0 2D, 1 NONE, 2 CLIPPLANE, 3 NONE. */
static int texmode_check(void)
{
    static const uint32_t zero[8];
    static char text[256 * 1024];
    NV2ACombinerState st;
    int bad = 0;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                             1u | 0u << 5 | 5u << 10 | 0u << 15, &st);
    if (d3d8_combiners_generate_hlsl(&st, text, (int)sizeof text) <= 0)
        return 1;
    if (!strstr(text, "x9 = float4(0.0, 0.0, 0.0, 1.0);")
            || !strstr(text, "x11 = float4(0.0, 0.0, 0.0, 1.0);")) {
        printf("FAIL: a NONE stage does not read (0, 0, 0, 1)\n");
        bad = 1;
    }
    if (strstr(text, "\n    x10 =") || strstr(text, "Texture2D tex2") || strstr(text, "tex2.Sample")) {
        printf("FAIL: the CLIPPLANE stage is sampled or written\n");
        bad = 1;
    }
    return bad;
}

/* rect_snap (render scale > 1): stage 0 (2D) samples through nv2a_snap_uv,
 * the helper is the shared text, and the same state without the flag emits
 * neither, so the hashed cases above stay the stock text. */
static int snap_check(void)
{
    static const uint32_t zero[8];
    static char text[256 * 1024];
    NV2ACombinerState st;
    int bad = 0, n;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u, 1u, &st);
    st.fog_input = 1;
    st.rect_snap = 1;
    if ((n = d3d8_combiners_generate_hlsl(&st, text, (int)sizeof text)) <= 0)
        return 1;
    if (!strstr(text, D3D8_HLSL_SNAP_UV) || !strstr(text, "uv = nv2a_snap_uv(uv, i.pos.xy, tex_scale[0]);")) {
        printf("FAIL: rect_snap does not sample stage 0 through nv2a_snap_uv\n");
        bad = 1;
    }
    (void)n;
    st.rect_snap = 0;
    if (d3d8_combiners_generate_hlsl(&st, text, (int)sizeof text) <= 0)
        return 1;
    if (strstr(text, "nv2a_snap_uv")) {
        printf("FAIL: nv2a_snap_uv without rect_snap\n");
        bad = 1;
    }
    return bad;
}

/* BUMPENVMAP (bump_on): stage 1 in mode 6 reads stage 0, stage 2 in mode 7
 * reads stage 1. The input's blue and green over 127, the matrix as the
 * slots hold it (00 01 11 10: .xw and .yz), the offset before tex_scale,
 * the luminance on the whole texel. Without bump_on the same modes are the
 * plain 2D sample, so the hashed cases above stay the stock text. */
static int bump_check(void)
{
    static const uint32_t zero[8];
    static char text[256 * 1024];
    NV2ACombinerState st;
    int bad = 0, n;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                             1u | 6u << 5 | 7u << 10, &st);
    st.fog_input = 1;
    st.tex_input[1] = 0;
    st.tex_input[2] = 1;
    st.bump_on = 1;
    if ((n = d3d8_combiners_generate_hlsl(&st, text, (int)sizeof text)) <= 0)
        return 1;
    if (!strstr(text, "float2 ds = round(x8.bg * 255.0);")
            || !strstr(text, "float2 ds = round(x9.bg * 255.0);")
            || !strstr(text, "ds = (ds >= 128.0 ? ds - 256.0 : ds) / 127.0;")
            || !strstr(text, "ds = float2(dot(ds, bump[1].xw), dot(ds, bump[1].yz));")
            || !strstr(text, "x9 = tex1.Sample(samp1, (i.tc1.xy + ds) * tex_scale[1].xy);")) {
        printf("FAIL: the BUMPENVMAP stage 1 text\n");
        bad = 1;
    }
    if (!strstr(text, "x10 *= x9.r * bumpls[2].x + bumpls[2].y;")
            || strstr(text, "x9 *= x8.r")) {
        printf("FAIL: the BUMPENVMAP_LUMINANCE stage 2 text\n");
        bad = 1;
    }
    st.bump_on = 0;
    if (d3d8_combiners_generate_hlsl(&st, text, (int)sizeof text) <= 0)
        return 1;
    if (strstr(text, "ds = ") || strstr(text, "bumpls[2].x")) {
        printf("FAIL: BUMPENVMAP text without bump_on\n");
        bad = 1;
    }
    return bad;
}

int main(void)
{
    static uint32_t prog[NV2A_VS_MAX_INSTRUCTIONS * 4];
    static NV2AVshProgram parsed;
    static char buf[256 * 1024];
    const char *dump = getenv("HLSL_DUMP");
    int i, k, n;

    if (dump && !(s_dump = fopen(dump, "wb"))) {
        fprintf(stderr, "cannot write %s\n", dump);
        return 2;
    }

    for (i = 0; i < NPROG; i++) {
        int len = 1 + (int)(rnd() % 40), fin = (int)(rnd() % (uint32_t)len);
        for (k = 0; k < len * 4; k++)
            prog[k] = rnd();
        for (k = 0; k < len; k++)
            prog[k * 4 + 3] &= ~1u;
        prog[fin * 4 + 3] |= 1u;
        d3d8_vsh_parse(prog, len, &parsed);
        n = d3d8_vsh_generate_hlsl_ex(&parsed, 0, buf, (int)sizeof buf);
        absorb("vsh", i, buf, n);
        n = d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      buf, (int)sizeof buf);
        absorb("vsh-ss", i, buf, n);
    }

    for (i = 0; i < NCOMB; i++) {
        uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
        uint32_t control, fcw0, fcw1, modes;
        NV2ACombinerState st;
        for (k = 0; k < 8; k++) {
            cicw[k] = rnd(); aicw[k] = rnd(); cocw[k] = rnd(); aocw[k] = rnd();
        }
        control = (rnd() & ~0xFFu) | (1 + rnd() % 8);
        fcw0 = rnd(); fcw1 = rnd(); modes = rnd();
        d3d8_combiners_from_regs(cicw, aicw, cocw, aocw, control, fcw0, fcw1,
                                 modes, &st);
        st.fog_input = (int)(rnd() & 1);
        n = d3d8_combiners_generate_hlsl(&st, buf, (int)sizeof buf);
        absorb("rc", i, buf, n);
    }

    if (s_dump)
        fclose(s_dump);
    printf("hlsl_split: %d programs, %d combiner sets, %lu chars, hash %08x\n",
           NPROG, NCOMB, s_chars, s_hash);
    if (getenv("HLSL_EXPECT_OUT")) {
        if (!write_expect(getenv("HLSL_EXPECT_OUT"))) {
            fprintf(stderr, "cannot write %s\n", getenv("HLSL_EXPECT_OUT"));
            return 2;
        }
        printf("wrote %s\n", getenv("HLSL_EXPECT_OUT"));
    }
#ifdef HLSL_NO_EXPECT
    printf("FAIL: no expected.h table\n");
    return 1;
#endif
    if (texmode_check())
        return 1;
    if (snap_check())
        return 1;
    if (bump_check())
        return 1;
    if (s_bad) {
        printf("FAIL: %d of %d cases differ (HLSL_DUMP=<file> writes the text)\n",
               s_bad, NCASE);
        return 1;
    }
    if (s_hash != EXPECTED_HASH || s_chars != EXPECTED_CHARS) {
        printf("FAIL: expected %lu chars, hash %08x\n",
               (unsigned long)EXPECTED_CHARS, EXPECTED_HASH);
        return 1;
    }
    printf("done\n");
    return 0;
}
