/* The texture helpers every path shares (nv2a_backend_common.h): cube-map
 * face selection and face stride, SET_TEXTURE_FILTER's linear rule, and
 * texel decodes checked against known bytes. */
#include <stdio.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define NEAR(a, b) ((a) - (b) < 1e-6f && (b) - (a) < 1e-6f)

static uint32_t resolve_id(uint32_t offset) { return offset; }

int main(void)
{
    static const float dir[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    float s, t;
    uint32_t f;

    /* Each axis picks its face, at the face's centre. */
    for (f = 0; f < 6; f++) {
        CHECK(nv2a_cube_face(dir[f], &s, &t) == f);
        CHECK(NEAR(s, 0.5f) && NEAR(t, 0.5f));
    }
    /* +X: s runs along -z, t along -y (D3D's cube layout). */
    { const float c[3] = { 1.0f, -0.5f, -0.5f };
      CHECK(nv2a_cube_face(c, &s, &t) == 0);
      CHECK(NEAR(s, 0.75f) && NEAR(t, 0.75f)); }
    /* +Y: s along x, t along z. */
    { const float c[3] = { 0.5f, 1.0f, -0.5f };
      CHECK(nv2a_cube_face(c, &s, &t) == 2);
      CHECK(NEAR(s, 0.75f) && NEAR(t, 0.25f)); }
    /* -Z: s along -x. */
    { const float c[3] = { 0.5f, 0.0f, -1.0f };
      CHECK(nv2a_cube_face(c, &s, &t) == 5);
      CHECK(NEAR(s, 0.25f) && NEAR(t, 0.5f)); }
    /* The zero vector does not divide by zero. */
    { const float c[3] = { 0, 0, 0 };
      nv2a_cube_face(c, &s, &t);
      CHECK(NEAR(s, 0.5f) && NEAR(t, 0.5f)); }

    /* Face stride: every level of a face, 128-aligned. A8R8G8B8 (0x06,
     * swizzled) 4x4, three levels: (16 + 4 + 1) * 4 = 84 -> 128. 16x16 one
     * level: 1024. DXT1 (0x0C) 8x8, four levels: 32 + 8 + 8 + 8 = 56 -> 128. */
    CHECK(nv2a_tex_face_stride(0x06, 4, 4, 0, 3) == 128);
    CHECK(nv2a_tex_face_stride(0x06, 16, 16, 0, 1) == 1024);
    CHECK(nv2a_tex_face_stride(0x0C, 8, 8, 0, 4) == 128);
    CHECK(nv2a_tex_face_stride(0x06, 32, 32, 0, 6) == 5504);
    /* Linear (LIN_A8R8G8B8 0x12): pitch * height. */
    CHECK(nv2a_tex_face_stride(0x12, 10, 10, 64, 1) == 640);

    /* SET_TEXTURE_FILTER: MAG [27:24], MIN [21:16]. */
    CHECK(!nv2a_tex_filter_linear(0x01010000u, 1) && !nv2a_tex_filter_linear(0x01010000u, 0));
    CHECK(nv2a_tex_filter_linear(0x02000000u, 1));      /* MAG tent */
    CHECK(nv2a_tex_filter_linear(0x04000000u, 1));      /* MAG convolution */
    CHECK(nv2a_tex_filter_linear(0x00020000u, 0));      /* MIN tent LOD 0 */
    CHECK(nv2a_tex_filter_linear(0x00060000u, 0));      /* MIN tent-tent */
    CHECK(!nv2a_tex_filter_linear(0x00050000u, 0));     /* MIN box-tent */
    CHECK(nv2a_tex_filter_mip(0x00060000u) == 2 && nv2a_tex_filter_mip(0x00030000u) == 1);
    CHECK(NEAR(nv2a_tex_lod_bias(0x1F00u), -1.0f) && NEAR(nv2a_tex_lod_bias(0x0080u), 0.5f));

    /* Mip levels and LOD (the CPU sampler's; the GPU paths take the same
     * bias and clamps as sampler state). Level offsets: A8R8G8B8 (0x06)
     * 16x8 is 512 bytes, then 8x4 128, then 4x2; DXT1 (0x0C) 8x8 is 32,
     * then 4x4 8, then 2x2 rounds up to a whole block. */
    CHECK(nv2a_tex_level_offset(0x06, 16, 8, 0) == 0);
    CHECK(nv2a_tex_level_offset(0x06, 16, 8, 1) == 512);
    CHECK(nv2a_tex_level_offset(0x06, 16, 8, 2) == 640);
    CHECK(nv2a_tex_level_offset(0x0C, 8, 8, 3) == 48);
    /* rho 4 texels a pixel: LOD 2; the larger axis wins; the bias adds;
     * the clamps hold; no footprint (0 or NaN) is lod_min. */
    CHECK(NEAR(nv2a_tex_lod(16.0f, 1.0f, 0.0f, 0.0f, 7.0f), 2.0f));
    CHECK(NEAR(nv2a_tex_lod(1.0f, 64.0f, 0.0f, 0.0f, 7.0f), 3.0f));
    CHECK(NEAR(nv2a_tex_lod(16.0f, 0.0f, -0.5f, 0.0f, 7.0f), 1.5f));
    CHECK(NEAR(nv2a_tex_lod(0.25f, 0.0f, 0.0f, 0.0f, 7.0f), 0.0f));   /* magnified */
    CHECK(NEAR(nv2a_tex_lod(1e12f, 0.0f, 0.0f, 0.0f, 5.0f), 5.0f));
    CHECK(NEAR(nv2a_tex_lod(1.0f, 1.0f, 0.0f, 2.0f, 5.0f), 2.0f));
    CHECK(NEAR(nv2a_tex_lod(0.0f, 0.0f, 0.0f, 1.0f, 5.0f), 1.0f));
    { float nan = 0.0f / 0.0f * 0.0f;
      CHECK(NEAR(nv2a_tex_lod(nan, nan, 0.0f, 1.0f, 5.0f), 1.0f)); }
    {   /* Which levels: nearest rounds, linear blends floor and floor + 1,
         * the last level stands alone, mode 0 is level 0. */
        uint32_t l0, l1;
        float w;
        nv2a_tex_mip_levels(1, 1.4f, 8, &l0, &l1, &w);
        CHECK(l0 == 1 && l1 == 1 && w == 0.0f);
        nv2a_tex_mip_levels(1, 1.6f, 8, &l0, &l1, &w);
        CHECK(l0 == 2 && l1 == 2);
        nv2a_tex_mip_levels(1, 7.9f, 8, &l0, &l1, &w);
        CHECK(l0 == 7);
        nv2a_tex_mip_levels(2, 2.25f, 8, &l0, &l1, &w);
        CHECK(l0 == 2 && l1 == 3 && NEAR(w, 0.25f));
        nv2a_tex_mip_levels(2, 7.0f, 8, &l0, &l1, &w);
        CHECK(l0 == 7 && l1 == 7 && w == 0.0f);
        nv2a_tex_mip_levels(2, 0.0f, 8, &l0, &l1, &w);
        CHECK(l0 == 0 && l1 == 1 && w == 0.0f);
        nv2a_tex_mip_levels(0, 3.0f, 8, &l0, &l1, &w);
        CHECK(l0 == 0 && l1 == 0);
    }

    {   /* FORMAT's DIMENSIONALITY [7:4] and CUBEMAP [2]: the CPU sampler
         * mipmaps neither a cube map nor a volume (its level offsets assume
         * a 2D chain). A8R8G8B8 (0x06), 3 levels, log2 sizes 4 x 4. */
        uint32_t r[16] = { 0 };
        uint8_t set[16] = { 0 };
        struct nv2a_stage st;
        r[0] = 0x1000;
        r[0x0C / 4] = (1u << 30) | (7u << 6);           /* enabled, LOD max 7 */
        r[0x04 / 4] = (4u << 24) | (4u << 20) | (3u << 16) | (0x06u << 8) | (2u << 4);
        nv2a_stage_decode(0, r, set, 0, 0, resolve_id, NV2A_STAGE_MIPS, &st);
        CHECK(st.dims == 2 && !st.cube && st.levels == 3 && st.valid);
        r[0x04 / 4] = (r[0x04 / 4] & ~0xF0u) | (3u << 4);
        nv2a_stage_decode(0, r, set, 0, 0, resolve_id, NV2A_STAGE_MIPS, &st);
        CHECK(st.dims == 3 && !st.cube);
        r[0x04 / 4] = (r[0x04 / 4] & ~0xF0u) | (2u << 4) | (1u << 2);
        nv2a_stage_decode(0, r, set, 0, 0, resolve_id, NV2A_STAGE_MIPS, &st);
        CHECK(st.dims == 2 && st.cube);
    }

    /* R8B8 (0x29, Spartan: Total Warrior) and LIN_R8B8 (0x16): B in the low
     * byte, R in the high one, expanded as xemu's GL_RG8 + (G, R, R, G)
     * swizzle and Cxbx's ____R8B8 row both do: R and A take the high byte,
     * G and B the low. G8B8 (0x17) on the same bytes puts the high byte in
     * G and A instead, which is what tells the two apart. */
    {
        static const uint8_t lin[4] = { 0xAB, 0x12, 0x00, 0xFF };
        uint32_t argb = 0;
        CHECK(nv2a_tex_linear_twin(0x29) == 0x16);
        CHECK(nv2a_tex_texel_bytes(0x16) == 2);
        CHECK(nv2a_tex_decode_texel(0x16, lin, 0, NULL, 0, &argb) && argb == 0x1212ABABu);
        CHECK(nv2a_tex_decode_texel(0x16, lin, 1, NULL, 0, &argb) && argb == 0xFFFF0000u);
        CHECK(nv2a_tex_decode_texel(0x17, lin, 0, NULL, 0, &argb) && argb == 0x12AB12ABu);
    }
    {
        /* Swizzled 4x2, one level, texel k = { k, 0x10 + k }: Morton order
         * interleaves x0, y0, x1, so row-major texel i comes from swizzle
         * index { 0, 1, 4, 5, 2, 3, 6, 7 }[i]. A 2x2 block is the same in
         * both orders; 4x2 is the smallest that tells a swizzled walk from
         * a linear one. */
        static const uint8_t sz[16] = {
            0x00, 0x10, 0x01, 0x11, 0x02, 0x12, 0x03, 0x13,
            0x04, 0x14, 0x05, 0x15, 0x06, 0x16, 0x07, 0x17
        };
        static const uint32_t idx[8] = { 0, 1, 4, 5, 2, 3, 6, 7 };
        uint32_t out[8] = { 0 }, i;
        CHECK(nv2a_tex_decode_level(sz, 0x29, 4, 2, NULL, 0, out) == 16);
        CHECK(out[2] == 0x14140404u && out[4] == 0x12120202u);
        for (i = 0; i < 8; i++)
            CHECK(out[i] == (0x10u + idx[i]) * 0x01010000u + idx[i] * 0x0101u);
        /* The level takes 2 bytes a texel, so the extent is no longer 0 and
         * no backend draws it magenta: Spartan's texture is the single
         * 256x256 level (131072 bytes); the 9-level chain checks the extent
         * of a full mip chain. */
        CHECK(nv2a_tex_extent(0x29, 256, 256, 0, 1) == 256u * 256u * 2u);
        CHECK(nv2a_tex_extent(0x29, 256, 256, 0, 9) == 87381u * 2u);
        CHECK(nv2a_tex_extent(0x16, 4, 2, 16, 1) == 16u + 8u);
    }

    puts("nv2a_tex: all passed");
    return 0;
}
