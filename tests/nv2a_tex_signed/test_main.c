/* SET_TEXTURE_FILTER's signed channels (NV_PGRAPH_TEXFILTER0_{A,R,G,B}SIGNED,
 * bits 28-31) in the code both GPU backends share: the stage decode and the
 * pixel-shader key, the remap of a fetched byte to 2 s / 255 against the
 * nxdk_pgraph_tests hardware goldens' values, the upload bias that lets the
 * sampler filter the signed value, and the HLSL and MSL the generators emit
 * for a signed stage, biased and not, with a bump stage reading it. A stock
 * draw's text has none of it. On macOS the MSL is compiled with Metal. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"
#include "d3d8_hlsl.h"
#include "d3d8_msl.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

#ifdef MSL_HAVE_METAL
int msl_compile(const char *src, const char *fn, char *err, int errsize);
#endif

static uint32_t resolve_id(uint32_t offset) { return offset; }

static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }

/* The shaders' biased remap, u the fetched (filtered) offset-binary value. */
static float biased_remap(float u) { return u * 2.0f - 256.0f / 255.0f; }

/* The shaders' xr of a biased channel: the stock byte back, two's complement. */
static float biased_stock(float u)
{
    return u + 128.0f / 255.0f - (u >= 127.5f / 255.0f ? 256.0f / 255.0f : 0.0f);
}

int main(void)
{
    static uint32_t regs[4][16];
    static uint8_t set[4][16];
    static char text[256 * 1024];
    static const uint32_t zero[8];
    struct nv2a_stage st[4];
    NV2ACombinerState cs;
    int n;

    /* Four swizzled A8R8G8B8 16x16 stages; stage 0 2D (1) with R, G and B
     * signed and trilinear, as BLiNX 2's water du/dv map (E2063ECE); stage 1
     * BUMPENVMAP (6) reading it; stage 2 2D with A signed; stage 3 off. */
    for (n = 0; n < 4; n++) {
        regs[n][0x00 / 4] = 0x1000u * (uint32_t)(n + 1);
        regs[n][0x04 / 4] = 0x06u << 8 | 4u << 20 | 4u << 24 | 1u << 16 | 2u << 4;
        regs[n][0x0C / 4] = 1u << 30;
        set[n][0x0C / 4] = 1;
    }
    regs[0][0x14 / 4] = 0xE2063ECEu;
    regs[1][0x14 / 4] = 0x01012000u;     /* unsigned */
    regs[2][0x14 / 4] = 0x12012000u;     /* A signed */
    regs[3][0x14 / 4] = 0xF2012000u;     /* all signed, but mode 0 */
    for (n = 0; n < 4; n++)
        nv2a_stage_decode(n, regs[n], set[n], 1, 1u | 6u << 5 | 1u << 10,
                          resolve_id, 0, &st[n]);
    CHECK(nv2a_tex_filter_signed(st[0].filter) == 0xE);
    CHECK(nv2a_tex_filter_signed(st[1].filter) == 0);
    CHECK(nv2a_tex_filter_signed(st[2].filter) == 0x1);
    /* The key: a nibble a stage, only for stages whose mode reads a texture. */
    CHECK(nv2a_ps_signed_key(st) == (0xEu | 0x1u << 8));
    {
        struct nv2a_stage c[4];
        memcpy(c, st, sizeof c);
        for (n = 0; n < 4; n++)
            c[n].filter &= 0x0FFFFFFFu;
        CHECK(nv2a_ps_signed_key(c) == 0);   /* stock */
        /* A mode that fetches no texel (DOTPRODUCT, stages 1-2 only) keys
         * no variant: stage 2's signed alpha drops out of the key. */
        memcpy(c, st, sizeof c);
        nv2a_stage_decode(2, regs[2], set[2], 1, 1u | 6u << 5 | 17u << 10,
                          resolve_id, 0, &c[2]);
        CHECK(nv2a_stage_ps_mode(&c[2], 2) == 17u);
        CHECK(nv2a_tex_filter_signed(c[2].filter) == 0x1);
        CHECK(nv2a_ps_signed_key(c) == 0xEu);
    }

    /* The remap of a fetched byte b (u = b / 255). The positive half is
     * measured: nxdk_pgraph_tests "Texture signed component tests", golden
     * txt_A8R8G8B8_ADD.png (signed rows read 0x01 2, 0x40 128, 0x7F 254).
     * The negative half is clamped to 0 by that test's unsigned map, so its
     * two's-complement values here are an open question (design.md). */
    CHECK(near(nv2a_signed_remap(0.0f), 0.0f));
    CHECK(near(nv2a_signed_remap(1.0f / 255.0f), 2.0f / 255.0f));
    CHECK(near(nv2a_signed_remap(64.0f / 255.0f), 128.0f / 255.0f));
    CHECK(near(nv2a_signed_remap(127.0f / 255.0f), 254.0f / 255.0f));
    CHECK(near(nv2a_signed_remap(128.0f / 255.0f), -256.0f / 255.0f));
    CHECK(near(nv2a_signed_remap(255.0f / 255.0f), -2.0f / 255.0f));

    /* The bias: each marked byte's top bit, in A8R8G8B8 order. */
    CHECK(nv2a_tex_signed_xor(0) == 0);
    CHECK(nv2a_tex_signed_xor(1) == 0x80000000u);
    CHECK(nv2a_tex_signed_xor(2) == 0x00800000u);
    CHECK(nv2a_tex_signed_xor(4) == 0x00008000u);
    CHECK(nv2a_tex_signed_xor(8) == 0x00000080u);
    CHECK(nv2a_tex_signed_xor(0xE) == 0x00808080u);
    {
        uint32_t tx[2] = { 0x11FF01FFu, 0x11017F80u }, keep[2];
        memcpy(keep, tx, sizeof tx);
        nv2a_tex_signed_bias(tx, 2, 0);
        CHECK(!memcmp(tx, keep, sizeof tx));   /* no mask: the stock texels */
        nv2a_tex_signed_bias(tx, 2, 0xE);
        CHECK(tx[0] == 0x117F817Fu && tx[1] == 0x1181FF00u);
        /* Every byte biased reads back the stock remap, and xr its byte. */
        for (n = 0; n < 256; n++) {
            float u = (float)(n ^ 0x80) / 255.0f;
            CHECK(near(biased_remap(u), nv2a_signed_remap((float)n / 255.0f)));
            CHECK(near(biased_stock(u), (float)n / 255.0f));
        }
        /* The water's case: du/dv crossing zero, 0xFF (-1) next to 0x01
         * (+1), filtered half way. Stored as is the sampler gives 0x80,
         * -128; biased it gives the signed mean, 0, as the hardware. */
        CHECK(near(nv2a_signed_remap((255.0f + 1.0f) / 2.0f / 255.0f), -256.0f / 255.0f));
        CHECK(near(biased_remap((float)((0xFF ^ 0x80) + (0x01 ^ 0x80)) / 2.0f / 255.0f), 0.0f));
        CHECK(near(biased_stock((float)((0xFF ^ 0x80) + (0x01 ^ 0x80)) / 2.0f / 255.0f), 0.0f));
    }

    /* The generated text. Stock first: no signed stage, none of it, and
     * byte for byte the text of a state whose only signed stages fetch no
     * texel (mode NONE, DOTPRODUCT) or carry the bias flag alone. */
    d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                             1u | 6u << 5 | 1u << 10, &cs);
    cs.fog_input = 1;
    cs.tex_input[1] = 0;
    cs.bump_on = 1;
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(!strstr(text, "xr") && !strstr(text, "127.5"));
    CHECK(strstr(text, "float2 ds = round(x8.bg * 255.0);"));
    {
        static char stock[2][256 * 1024], other[256 * 1024];
        NV2ACombinerState o = cs;
        int msl;
        strcpy(stock[0], text);
        CHECK(d3d8_combiners_generate_msl(&cs, stock[1], (int)sizeof stock[1]) > 0);
        CHECK(!strstr(stock[1], "xr") && !strstr(stock[1], "127.5"));
        o.tex_signed[3] = 0xF;
        o.tex_signed[2] = NV2A_TEX_SIGNED_BIASED;
        for (msl = 0; msl < 2; msl++) {
            CHECK((msl ? d3d8_combiners_generate_msl(&o, other, (int)sizeof other)
                       : d3d8_combiners_generate_hlsl(&o, other, (int)sizeof other)) > 0);
            CHECK(!strcmp(other, stock[msl]));
        }
        {
            NV2ACombinerState a, b;
            static char ta[256 * 1024];
            d3d8_combiners_from_regs(zero, zero, zero, zero, 1u, 0u, 0u,
                                     1u | 6u << 5 | 17u << 10, &a);
            a.fog_input = 1;
            a.bump_on = 1;
            b = a;
            b.tex_signed[2] = 0xF;
            for (msl = 0; msl < 2; msl++) {
                CHECK((msl ? d3d8_combiners_generate_msl(&a, ta, (int)sizeof ta)
                           : d3d8_combiners_generate_hlsl(&a, ta, (int)sizeof ta)) > 0);
                CHECK((msl ? d3d8_combiners_generate_msl(&b, other, (int)sizeof other)
                           : d3d8_combiners_generate_hlsl(&b, other, (int)sizeof other)) > 0);
                CHECK(!strcmp(ta, other));
            }
        }
    }

    /* Stage 0 signed rgb, as the stock bytes (a render target or a pack
     * entry): the point-sampled remap; the bump stage reads xr8. Stage 2
     * signed alpha. */
    cs.tex_signed[0] = 0xE;
    cs.tex_signed[2] = 0x1;
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "float4 xr8 = x8;"));
    CHECK(strstr(text, "x8.rgb = x8.rgb * 2.0 - step(127.5 / 255.0, x8.rgb) * (512.0 / 255.0);"));
    CHECK(strstr(text, "float2 ds = round(xr8.bg * 255.0);"));
    CHECK(strstr(text, "x10.a = x10.a * 2.0 - step(127.5 / 255.0, x10.a) * (512.0 / 255.0);"));
    CHECK(!strstr(text, "xr9"));
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "float4 xr8 = x8;"));
    CHECK(strstr(text, "x8.rgb = x8.rgb * 2.0 - step(float3(127.5 / 255.0), x8.rgb)"
                       " * (512.0 / 255.0);"));
    CHECK(strstr(text, "float2 ds = round(xr8.bg * 255.0);"));
    CHECK(strstr(text, "x10.a = x10.a * 2.0 - step(float(127.5 / 255.0), x10.a)"
                       " * (512.0 / 255.0);"));
#ifdef MSL_HAVE_METAL
    {
        char err[4096];
        if (msl_compile(text, "fs_main", err, (int)sizeof err) != 0) {
            printf("FAIL: MSL (stock bytes) does not compile:\n%s\n", err);
            return 1;
        }
    }
#endif

    /* Biased (the guest texels, nv2a_tex_signed_bias): linear in u, and xr
     * the stock byte back for the bump decode. */
    cs.tex_signed[0] = 0xE | NV2A_TEX_SIGNED_BIASED;
    cs.tex_signed[2] = 0x1 | NV2A_TEX_SIGNED_BIASED;
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "xr8.rgb = x8.rgb + (128.0 / 255.0) - step(127.5 / 255.0, x8.rgb)"
                       " * (256.0 / 255.0);"));
    CHECK(strstr(text, "x8.rgb = x8.rgb * 2.0 - (256.0 / 255.0);"));
    CHECK(strstr(text, "float2 ds = round(xr8.bg * 255.0);"));
    CHECK(strstr(text, "x10.a = x10.a * 2.0 - (256.0 / 255.0);"));
    CHECK(d3d8_combiners_generate_msl(&cs, text, (int)sizeof text) > 0);
    CHECK(strstr(text, "xr8.rgb = x8.rgb + (128.0 / 255.0) - step(float3(127.5 / 255.0), x8.rgb)"
                       " * (256.0 / 255.0);"));
    CHECK(strstr(text, "x8.rgb = x8.rgb * 2.0 - (256.0 / 255.0);"));
    CHECK(strstr(text, "float2 ds = round(xr8.bg * 255.0);"));
#ifdef MSL_HAVE_METAL
    {
        char err[4096];
        if (msl_compile(text, "fs_main", err, (int)sizeof err) != 0) {
            printf("FAIL: MSL (biased) does not compile:\n%s\n", err);
            return 1;
        }
    }
#endif

    /* A signed stage whose mode reads no texture is left alone. */
    memset(cs.tex_signed, 0, sizeof cs.tex_signed);
    cs.tex_signed[3] = 0xF;
    CHECK(d3d8_combiners_generate_hlsl(&cs, text, (int)sizeof text) > 0);
    CHECK(!strstr(text, "xr"));

    printf("nv2a_tex_signed: ok\n");
    return 0;
}
