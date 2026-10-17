/* BUMPENVMAP in the code both GPU backends share: the stage decode of
 * SET_TEXTURE_SET_BUMP_ENV_MAT (0x1B28..0x1B34), BUMP_ENV_SCALE (0x1B38) and
 * BUMP_ENV_OFFSET (0x1B3C), as stored (slots 00 01 11 10); their copy into
 * nv2a_ps_consts; SET_SHADER_OTHER_STAGE_INPUT's stage selection; the bump
 * sources render.aniso skips; and the generated HLSL and MSL for a mode-6 and
 * a mode-7 stage. */
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"
#include "d3d8_hlsl.h"
#include "d3d8_msl.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t resolve_id(uint32_t offset) { return offset; }

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

int main(void)
{
    static uint32_t regs[4][16];
    static uint8_t set[4][16];
    static struct nv2a_pb_vsh v;
    static char text[256 * 1024];
    static const uint32_t zero[8];
    struct nv2a_stage st[4];
    struct nv2a_ps_consts pc;
    NV2ACombinerState cs;
    int n;

    /* Four valid swizzled A8R8G8B8 (0x06) 16x16 stages, enabled; stage 1 in
     * BUMPENVMAP (6), stage 2 in BUMPENVMAP_LUMINANCE (7). */
    for (n = 0; n < 4; n++) {
        regs[n][0x00 / 4] = 0x1000u * (uint32_t)(n + 1);
        regs[n][0x04 / 4] = 0x06u << 8 | 4u << 20 | 4u << 24 | 1u << 16 | 2u << 4;
        regs[n][0x0C / 4] = 1u << 30;
        set[n][0x0C / 4] = 1;
    }
    regs[1][0x28 / 4] = fbits(0.5f);     /* slot 0: m00 */
    regs[1][0x2C / 4] = fbits(0.25f);    /* slot 1: m01 */
    regs[1][0x30 / 4] = fbits(-0.75f);   /* slot 2: m11 */
    regs[1][0x34 / 4] = fbits(0.125f);   /* slot 3: m10 */
    regs[1][0x38 / 4] = fbits(2.0f);
    regs[1][0x3C / 4] = fbits(-1.0f);
    regs[2][0x38 / 4] = fbits(0.5f);
    regs[2][0x3C / 4] = fbits(0.25f);
    for (n = 0; n < 4; n++)
        nv2a_stage_decode(n, regs[n], set[n], 1, 1u | 6u << 5 | 7u << 10,
                          resolve_id, 0, &st[n]);
    CHECK(st[1].mode == 6 && st[2].mode == 7 && st[3].mode == 0);
    /* As stored: the decode does not reorder the slots. */
    CHECK(st[1].bump[0] == 0.5f && st[1].bump[1] == 0.25f
          && st[1].bump[2] == -0.75f && st[1].bump[3] == 0.125f);
    CHECK(st[1].bump_scale == 2.0f && st[1].bump_offset == -1.0f);
    CHECK(st[2].bump_scale == 0.5f && st[2].bump_offset == 0.25f);

    nv2a_ps_consts_fill(&v, st, &pc);
    /* The shader takes du' = dot(dudv, bump.xw) = m00 du + m10 dv and
     * dv' = dot(dudv, bump.yz) = m01 du + m11 dv: the D3D formula with the
     * slots 00 01 11 10, so the constants keep the stored order. */
    CHECK(pc.bump[1][0] == 0.5f && pc.bump[1][1] == 0.25f
          && pc.bump[1][2] == -0.75f && pc.bump[1][3] == 0.125f);
    CHECK(pc.bumpls[1][0] == 2.0f && pc.bumpls[1][1] == -1.0f);
    CHECK(pc.bumpls[2][0] == 0.5f && pc.bumpls[2][1] == 0.25f);

    /* OTHER_STAGE_INPUT: stage 1 reads 0 whatever the register says; 2 and
     * 3 take bits 16-19 and 20-23; a late input is clamped to n - 1. */
    CHECK(nv2a_stage_input(0, 0xFFFFFFFFu) == 0);
    CHECK(nv2a_stage_input(1, 0xFFFFFFFFu) == 0);
    CHECK(nv2a_stage_input(2, 1u << 16) == 1);
    CHECK(nv2a_stage_input(2, 0u << 16 | 2u << 20) == 0);
    CHECK(nv2a_stage_input(3, 2u << 20 | 1u << 16) == 2);
    CHECK(nv2a_stage_input(3, 1u << 20) == 1);
    CHECK(nv2a_stage_input(2, 2u << 16) == 1);     /* itself: clamped */
    CHECK(nv2a_stage_input(3, 0xFu << 20) == 2);   /* past it: clamped */

    /* The shader mode both backends key on: 6 and 7 stay from stage 1. */
    CHECK(nv2a_stage_ps_mode(&st[1], 1) == 6);
    CHECK(nv2a_stage_ps_mode(&st[2], 2) == 7);
    {
        struct nv2a_stage s0 = st[1];
        CHECK(nv2a_stage_ps_mode(&s0, 0) == 1);    /* stage 0: no input */
        s0.mode = 9;
        CHECK(nv2a_stage_ps_mode(&s0, 1) == 1);    /* other modes: 2D */
        s0.raw_mode = 5;
        CHECK(nv2a_stage_ps_mode(&s0, 1) == 5);    /* CLIPPLANE */
    }

    /* Bump sources: the stages a 6 or 7 stage reads, combiner draws only. */
    CHECK(nv2a_bump_sources(st, 1, 1u << 16) == 0x3u);   /* 1 <- 0, 2 <- 1 */
    CHECK(nv2a_bump_sources(st, 1, 0u) == 0x1u);         /* 1 and 2 <- 0 */
    CHECK(nv2a_bump_sources(st, 0, 1u << 16) == 0u);     /* basic shader */
    {
        /* stage 3 in mode 6 reads stage 2 (bits 20-23): 1 and 2 <- 0, 3 <- 2 */
        struct nv2a_stage c[4];
        memcpy(c, st, sizeof c);
        c[3].mode = 6;
        CHECK(nv2a_bump_sources(c, 1, 2u << 20) == 0x5u);
        /* stage 3 in mode 0 names stage 1, but a plain stage reads no
         * offsets: stage 1 is not a source (only 0, from stages 1 and 2) */
        c[3].mode = 0;
        CHECK(nv2a_bump_sources(c, 1, 1u << 20) == 0x1u);
    }

    /* render.aniso skips a bump source (its texels are offsets: aniso keeps
     * the ripple a mip would average away and the dependent lookups scatter
     * into noise); a plain mipmapped stage keeps it, and the sampler key
     * carries the result per draw. */
    {
        struct nv2a_host_opts o = *nv2a_host_opts();
        unsigned src = nv2a_bump_sources(st, 1, 0u);
        struct nv2a_stage c = st[3];
        o.aniso = 16;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_stage_aniso(&st[0], 2, 1, src >> 0 & 1) == 0);   /* source */
        CHECK(nv2a_stage_aniso(&st[3], 2, 1, src >> 3 & 1) == 16);  /* plain */
        CHECK(nv2a_stage_aniso(&st[1], 2, 1, src >> 1 & 1) == 16);  /* perturbed */
        CHECK(nv2a_stage_aniso(&st[0], 2, 1,
                               nv2a_bump_sources(st, 0, 0u) & 1) == 16);
        CHECK(nv2a_stage_aniso(&st[3], 0, 1, 0) == 0);              /* no mips */
        CHECK(nv2a_stage_aniso(&st[3], 2, 0, 0) == 0);              /* point */
        c.cube = 1;
        CHECK(nv2a_stage_aniso(&c, 2, 1, 0) == 0);                  /* cube */
        o.aniso = 0;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_stage_aniso(&st[3], 2, 1, 0) == 0);              /* stock */
    }

    /* The generated text: stage 1 mode 6 from stage 0, stage 2 mode 7 from
     * stage 1. */
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                             1u | 6u << 5 | 7u << 10, &cs);
    cs.fog_input = 1;
    cs.tex_input[1] = 0;
    cs.tex_input[2] = 1;
    cs.bump_on = 1;
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "float4 bump[4]; float4 bumpls[4];"));
    CHECK(strstr(text, "float2 ds = round(x8.bg * 255.0);"));
    CHECK(strstr(text, "ds = (ds >= 128.0 ? ds - 256.0 : ds) / 127.0;"));
    CHECK(!strstr(text, "/ 128.0"));
    CHECK(strstr(text, "ds = float2(dot(ds, bump[1].xw), dot(ds, bump[1].yz));"));
    /* the offset in the coordinate's units, before tex_scale */
    CHECK(strstr(text, "x9 = tex1.Sample(samp1, (i.tc1.xy + ds) * tex_scale[1].xy);"));
    CHECK(strstr(text, "float2 ds = round(x9.bg * 255.0);"));
    /* luminance on the whole texel, alpha too (xemu: t = t * lum) */
    CHECK(strstr(text, "x10 *= x9.r * bumpls[2].x + bumpls[2].y;"));
    CHECK(!strstr(text, "x10.rgb *="));
    CHECK(!strstr(text, "x9 *= "));     /* mode 6: no luminance */

    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "float4 bump[4]; float4 bumpls[4];"));
    CHECK(strstr(text, "float2 ds = round(x8.bg * 255.0);"));
    CHECK(strstr(text, "ds = select(ds, ds - 256.0, ds >= 128.0) / 127.0;"));
    CHECK(strstr(text, "ds = float2(dot(ds, pc.bump[1].xw), dot(ds, pc.bump[1].yz));"));
    CHECK(strstr(text, "x9 = tex1.sample(samp1, (i.t1.xy + ds) * pc.tex_scale[1].xy,"
                       " bias(pc.tex_bias[1]));"));
    CHECK(strstr(text, "x10 *= x9.r * pc.bumpls[2].x + pc.bumpls[2].y;"));
    CHECK(strstr(text, "texture2d<float> tex1") && strstr(text, "texture2d<float> tex2"));

    /* Without bump_on (the legacy D3D8 path) the modes are the plain 2D
     * sample on both generators. */
    cs.bump_on = 0;
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(!strstr(text, "ds = ") && strstr(text, "x9 = tex1.Sample(samp1, uv * tex_scale[1].xy);"));
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(!strstr(text, "ds = ") && strstr(text, "x9 = tex1.sample(samp1, uv * pc.tex_scale[1].xy,"));

    printf("nv2a_bump: ok\n");
    return 0;
}
