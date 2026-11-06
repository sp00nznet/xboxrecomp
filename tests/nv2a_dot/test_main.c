/* The texture shader's dot-product modes and cube maps in the code both GPU
 * backends share, on Quantum Redshift's water: stage program
 * 0x64621 is stage 0 PROJECT2D on a normal map, stages 1 and 2 DOTPRODUCT
 * (stage 1 disabled, no texture) and stage 3 DOT_RFLCT_SPEC on a DXT5 cube
 * map, SET_DOT_RGBMAPPING 0x111. Before, every one of those modes was
 * sampled as 2D at (s, t) with the cube's face 0 bound, and the water drew
 * as one flat brown plane. Checked: the shader modes the stages keep, the
 * cube-bind rule and the face stride, the packed key, and the generated
 * HLSL and MSL. */
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"
#include "d3d8_hlsl.h"
#include "d3d8_msl.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t resolve_id(uint32_t offset) { return offset; }

int main(void)
{
    static uint32_t regs[4][16];
    static uint8_t set[4][16];
    static char text[256 * 1024];
    static const uint32_t zero[8];
    const uint32_t prog = 0x64621u, dotmap = 0x111u;
    struct nv2a_stage st[4];
    NV2ACombinerState cs;
    uint32_t modes, extra, face;
    int n;

    /* The water draw's registers as the title writes them (mlog, flip 2850). */
    regs[0][0x00 / 4] = 0x02866880u;    /* normal map, A8R8G8B8 32x32 */
    regs[0][0x04 / 4] = 0x05540629u;
    regs[0][0x0C / 4] = 0x4003FFC0u;
    set[0][0x0C / 4] = 1;
    set[1][0x0C / 4] = 1;               /* stage 1: CONTROL0 0, disabled */
    for (n = 2; n < 4; n++) {
        regs[n][0x00 / 4] = 0x02B65700u;    /* cube map, DXT5 128x128, 6 levels */
        regs[n][0x04 / 4] = 0x07760F2Du;
        regs[n][0x0C / 4] = 0x4003FFC0u;
        set[n][0x0C / 4] = 1;
    }
    for (n = 0; n < 4; n++)
        nv2a_stage_decode(n, regs[n], set[n], 1, prog, resolve_id,
                          NV2A_STAGE_P8_PALETTE | NV2A_STAGE_PITCH_LINEAR | NV2A_STAGE_MIPS,
                          &st[n]);
    CHECK(st[0].mode == 1 && !st[0].cube);
    CHECK(st[1].mode == 0 && st[1].raw_mode == 0x11);   /* gated: no texture */
    CHECK(st[2].mode == 0x11 && st[3].mode == 0x0C);
    CHECK(st[3].cube && st[3].width == 128 && st[3].height == 128 && st[3].levels == 6);

    /* The shader modes: DOTPRODUCT stays on the disabled stage 1, since it
     * reads no texture and its dot feeds stage 3's normal. */
    CHECK(nv2a_stage_ps_mode(&st[0], 0) == 1);
    CHECK(nv2a_stage_ps_mode(&st[1], 1) == 0x11);
    CHECK(nv2a_stage_ps_mode(&st[2], 2) == 0x11);
    CHECK(nv2a_stage_ps_mode(&st[3], 3) == 0x0C);
    /* Only stage 3 binds a cube: stage 2 has the same texture but reads none. */
    CHECK(!nv2a_stage_cube_bind(&st[2], 0x11));
    CHECK(nv2a_stage_cube_bind(&st[3], 0x0C));
    {
        struct nv2a_stage c = st[3];
        /* a dot mode outside its stages is sampled as 2D */
        CHECK(nv2a_stage_ps_mode(&c, 1) == 1);
        CHECK(nv2a_stage_ps_mode(&c, 2) == 1);
        /* CUBEMAP: a cube over a square cube texture, 2D otherwise */
        c.mode = c.raw_mode = 3;
        CHECK(nv2a_stage_ps_mode(&c, 0) == 3);
        c.cube = 0;
        CHECK(nv2a_stage_ps_mode(&c, 0) == 1);
        c.cube = 1;
        c.height = 64;
        CHECK(nv2a_stage_ps_mode(&c, 0) == 1);
        c.height = 128;
        c.mode = 0;     /* not bound: nothing to sample */
        CHECK(!nv2a_stage_cube_bind(&c, 3));
        CHECK(nv2a_stage_ps_mode(&c, 0) == 0);
        /* the other dot stages */
        c.mode = c.raw_mode = 0x0B;
        CHECK(nv2a_stage_ps_mode(&c, 2) == 0x0B && nv2a_stage_ps_mode(&c, 3) == 1);
        c.raw_mode = 0x09;
        CHECK(nv2a_stage_ps_mode(&c, 1) != 0x09 && nv2a_stage_ps_mode(&c, 2) == 0x09);
        c.raw_mode = 0x0F;
        CHECK(nv2a_stage_ps_mode(&c, 0) != 0x0F && nv2a_stage_ps_mode(&c, 1) == 0x0F);
        c.raw_mode = 0x11;
        CHECK(nv2a_stage_ps_mode(&c, 3) != 0x11);
    }

    /* SET_DOT_RGBMAPPING: 3 bits a stage from stage 1, 4 apart. */
    CHECK(nv2a_stage_dot_map(0, 0x777u) == 0);
    CHECK(nv2a_stage_dot_map(1, 0x321u) == 1);
    CHECK(nv2a_stage_dot_map(2, 0x321u) == 2);
    CHECK(nv2a_stage_dot_map(3, 0x321u) == 3);
    CHECK(nv2a_stage_dot_map(1, 0xFu) == 7);

    /* Six DXT5 faces of 6 levels: 21840 bytes each, 128-byte aligned. */
    face = 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4;
    CHECK(nv2a_stage_face_stride(&st[3]) == ((face + 127u) & ~127u));
    CHECK(nv2a_stage_cube_extent(&st[3]) == 5u * ((face + 127u) & ~127u) + face);

    /* The key: modes 5 bits a stage; inputs (all stage 0 here), the
     * mappings from bit 16 and the cube bits from 28. */
    nv2a_ps_stage_key(st, 0u, dotmap, &modes, &extra);
    CHECK(modes == (1u | 0x11u << 5 | 0x11u << 10 | 0x0Cu << 15));
    CHECK(extra == (1u << 16 | 1u << 20 | 1u << 24 | 1u << 31));
    nv2a_ps_stage_key(st, 1u << 16 | 2u << 20, 0x123u, &modes, &extra);
    CHECK(extra == (1u << 8 | 2u << 12 | 3u << 16 | 2u << 20 | 1u << 24 | 1u << 31));

    /* A cube whose faces lie under a live render target (cube_rt) is not
     * bound as one: the key, the shader mode and the bind take the 2D
     * read, as the backends then bind the target's own texture. */
    {
        struct nv2a_stage c[4];
        uint32_t span = nv2a_stage_cube_extent(&st[3]);
        memcpy(c, st, sizeof c);
        CHECK(nv2a_stage_cube_over_rt(&c[3], 0x02B65700u, 128u * 4u * 128u));    /* face 0 */
        CHECK(nv2a_stage_cube_over_rt(&c[3], 0x82B65700u + span - 4u, 64u));     /* face 5 */
        CHECK(!nv2a_stage_cube_over_rt(&c[3], 0x02B65700u + span, 0x10000u));    /* after */
        CHECK(!nv2a_stage_cube_over_rt(&c[0], 0x02866880u, 0x1000u));           /* no cube */
        c[3].cube_rt = 1;
        CHECK(!nv2a_stage_cube_bind(&c[3], 0x0C));
        CHECK(nv2a_stage_ps_mode(&c[3], 3) == 0x0C);    /* reads 2D (nv2a_cube_2d) */
        nv2a_ps_stage_key(c, 0u, dotmap, &modes, &extra);
        CHECK(extra == (1u << 16 | 1u << 20 | 1u << 24));
        c[3].mode = c[3].raw_mode = 3;
        CHECK(nv2a_stage_ps_mode(&c[3], 0) == 1);       /* CUBEMAP: 2D at (s, t) */
    }

    /* DOT_RFLCT_DIFF at stage 2: its third dot product is stage 3's input
     * and mapping, packed whatever stage 3's mode. */
    {
        struct nv2a_stage c[4];
        memcpy(c, st, sizeof c);
        c[2].mode = c[2].raw_mode = 0x0B;
        c[3].mode = c[3].raw_mode = 1;
        c[3].cube = 0;
        nv2a_ps_stage_key(c, 2u << 20, 0x521u, &modes, &extra);
        CHECK((modes >> 10 & 31u) == 0x0Bu);
        CHECK((extra >> 12 & 0xFu) == 2u && (extra >> 24 & 0xFu) == 5u);
        CHECK(!(extra >> 31 & 1u));
    }

    /* The input stages a draw's later stages read (nv2a_bump_sources): the
     * dot-product family's normals too, DOT_RFLCT_DIFF's stage 3 included. */
    CHECK(nv2a_bump_sources(st, 1, 0u) == 0x1u);            /* water: all stage 0 */
    CHECK(nv2a_bump_sources(st, 1, 1u << 16 | 2u << 20) == 0x7u);
    CHECK(nv2a_bump_sources(st, 0, 1u << 16) == 0u);         /* basic shader */
    {
        struct nv2a_stage c[4];
        memcpy(c, st, sizeof c);
        c[1].raw_mode = c[1].mode = 1;   /* stage 1 a plain 2D stage */
        c[2].mode = c[2].raw_mode = 0x0F;   /* DEPENDENT_AR on stage 1 */
        c[3].mode = c[3].raw_mode = 1;
        CHECK(nv2a_bump_sources(c, 1, 1u << 16 | 2u << 20) == 0x2u);
        c[2].mode = c[2].raw_mode = 0x0B;   /* DIFF: stage 3's input too */
        CHECK(nv2a_bump_sources(c, 1, 1u << 16 | 2u << 20) == 0x6u);
    }

    /* The generated text, as the backends fill the state. */
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                             1u | 0x11u << 5 | 0x11u << 10 | 0x0Cu << 15, &cs);
    cs.fog_input = 1;
    cs.bump_on = 1;
    for (n = 1; n < 4; n++)
        cs.dot_map[n] = 1;
    cs.tex_cube[3] = 1;

    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "texture2d<float> tex0 [[texture(0)]]"));
    CHECK(strstr(text, "texturecube<float> tex3 [[texture(3)]]"));
    CHECK(!strstr(text, "tex1 [[") && !strstr(text, "tex2 [["));
    /* (x * 255 - 128) / 127, D3D's signed mapping */
    CHECK(strstr(text, "if (m == 1) return (x - 128.0) / 127.0;"));
    CHECK(strstr(text, "dp1 = dot(i.t1.xyz, nv2a_dotmap(x8, 1));"));
    CHECK(strstr(text, "dp2 = dot(i.t2.xyz, nv2a_dotmap(x8, 1));"));
    CHECK(strstr(text, "dp3 = dot(i.t3.xyz, nv2a_dotmap(x8, 1));"));
    CHECK(strstr(text, "x9 = float4(0.0);") && strstr(text, "x10 = float4(0.0);"));
    CHECK(strstr(text, "float3 eye = float3(i.t1.w, i.t2.w, i.t3.w);"));
    CHECK(strstr(text, "float3 rv = 2.0 * nrm * dot(nrm, eye) / dot(nrm, nrm) - eye;"));
    CHECK(strstr(text, "x11 = tex3.sample(samp3, rv, bias(pc.tex_bias[3]));"));

    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "Texture2D tex0 : register(t0);"));
    CHECK(strstr(text, "TextureCube tex3 : register(t3);"));
    CHECK(!strstr(text, "tex1 : register") && !strstr(text, "tex2 : register"));
    CHECK(strstr(text, "dp1 = dot(i.tc1.xyz, nv2a_dotmap(x8, 1));"));
    CHECK(strstr(text, "dp3 = dot(i.tc3.xyz, nv2a_dotmap(x8, 1));"));
    CHECK(strstr(text, "float3 eye = float3(i.tc1.w, i.tc2.w, i.tc3.w);"));
    CHECK(strstr(text, "x11 = tex3.Sample(samp3, rv);"));

    /* A reflection over a 2D texture reads it at xemu's remapCubeTo2D. */
    cs.tex_cube[3] = 0;
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "texture2d<float> tex3 [[texture(3)]]"));
    CHECK(strstr(text, "x11 = tex3.sample(samp3, (nv2a_cube_2d(rv)) * pc.tex_scale[3].xy,"));
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "Texture2D tex3 : register(t3);"));
    CHECK(strstr(text, "x11 = tex3.Sample(samp3, (nv2a_cube_2d(rv)) * tex_scale[3].xy);"));

    /* DOT_ST and DPNDNT: 2D at the dots, at the input texel's channels. */
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                             1u | 0x10u << 5 | 0x11u << 10 | 0x09u << 15, &cs);
    cs.bump_on = 1;
    cs.tex_input[2] = 1;
    cs.dot_map[2] = cs.dot_map[3] = 3;
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "x9 = tex1.sample(samp1, (x8.gb) * pc.tex_scale[1].xy,"));
    CHECK(strstr(text, "dp2 = dot(i.t2.xyz, nv2a_dotmap(x9, 3));"));
    CHECK(strstr(text, "x11 = tex3.sample(samp3, (float2(dp2, dp3)) * pc.tex_scale[3].xy,"));
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "x9 = tex1.Sample(samp1, (x8.gb) * tex_scale[1].xy);"));
    CHECK(strstr(text, "x11 = tex3.Sample(samp3, (float2(dp2, dp3)) * tex_scale[3].xy);"));

    /* CUBEMAP over a bound cube: a cube in the key and the text. */
    {
        struct nv2a_stage c[4];
        memset(c, 0, sizeof c);
        c[0] = st[3];
        c[0].mode = c[0].raw_mode = 3;
        nv2a_ps_stage_key(c, 0u, 0u, &modes, &extra);
        CHECK(modes == 3u && extra == 1u << 28);
    }
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u, 3u, &cs);
    cs.bump_on = 1;
    cs.tex_cube[0] = 1;
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "texturecube<float> tex0 [[texture(0)]]"));
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "TextureCube tex0 : register(t0);"));

    /* Without bump_on (the D3D8 front end) the modes stay plain 2D samples. */
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u, prog, &cs);
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(!strstr(text, "dp1") && !strstr(text, "texturecube"));
    CHECK(strstr(text, "texture2d<float> tex1 [[texture(1)]]"));
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(!strstr(text, "dp1") && !strstr(text, "TextureCube"));
    CHECK(strstr(text, "x9 = tex1.Sample(samp1, uv * tex_scale[1].xy);"));

    printf("nv2a_dot: ok\n");
    return 0;
}
