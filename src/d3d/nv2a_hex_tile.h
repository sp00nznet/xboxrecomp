/*
 * nv2a_hex_tile.h - textures.detile's hex-tile sampling (Mikkelsen 2022,
 * "Practical Real-Time Hex-Tiling", the offsets-only form of Heitz and
 * Neyret 2018): a stage whose pack entry is marked XRTP_FLAG_DETILE_HEX
 * blends three copies of its texture, each moved by an offset fixed per
 * vertex of a triangle grid, so an upscaled ground tile stops showing its
 * repeat. No rotation: the game's tiles carry baked light, which would
 * turn with the copy.
 *
 * The HLSL and MSL texts the combiner shaders take (d3d8_combiners_hlsl.c,
 * d3d8_combiners_msl.c, beside mag_span) and the C reference below are the
 * same math with the same constants; tests/hex_tile checks the reference,
 * the split ctests the texts. The hash is an integer one so every backend
 * gets the same offsets bit for bit.
 *
 * t is the normalised coordinate (uv * tex_scale.xy); dx, dy its screen
 * derivatives, taken by the caller in uniform flow. Every fetch uses them:
 * an offset changes no mip level, and the cell seams have no derivative
 * spike. The blend: W = w^7 * lerp(1, luma, 0.6), normalised, so a copy's
 * bright features win over another's flat parts instead of washing out.
 */
#ifndef NV2A_HEX_TILE_H
#define NV2A_HEX_TILE_H

#include <math.h>
#include <stdint.h>

/* The grid: 2 sqrt(3) cells per texture, Mikkelsen's default rate (a hex
 * about 0.58 of the texture across), then the triangle skew. */
#define NV2A_HEX_RATE  3.46410162f
#define NV2A_HEX_SKEW  0.57735027f
#define NV2A_HEX_SKEW2 1.15470054f
#define NV2A_HEX_H1    1597334673u
#define NV2A_HEX_H2    3812015801u

/* The triangle of the grid that holds t: its three vertices and their
 * barycentric weights (summing to 1). */
static inline void nv2a_hex_grid(float tx, float ty, float w[3], int32_t v[3][2])
{
    float sx = tx * NV2A_HEX_RATE, sy = ty * NV2A_HEX_RATE;
    float kx = sx - sy * NV2A_HEX_SKEW, ky = sy * NV2A_HEX_SKEW2;
    float bx = floorf(kx), by = floorf(ky);
    float fx = kx - bx, fy = ky - by, z = 1.0f - fx - fy;
    float s = z < 0.0f ? 1.0f : 0.0f, sg = 2.0f * s - 1.0f;
    int32_t ix = (int32_t)bx, iy = (int32_t)by, si = (int32_t)s;
    w[0] = -z * sg;
    w[1] = s - fy * sg;
    w[2] = s - fx * sg;
    v[0][0] = ix + si;     v[0][1] = iy + si;
    v[1][0] = ix + si;     v[1][1] = iy + 1 - si;
    v[2][0] = ix + 1 - si; v[2][1] = iy + si;
}

/* A vertex's offset, in [0, 1) on each axis: the high bits of each
 * product, all arithmetic mod 2^32. */
static inline void nv2a_hex_offset(const int32_t v[2], float off[2])
{
    uint32_t qx = (uint32_t)v[0] * NV2A_HEX_H1, qy = (uint32_t)v[1] * NV2A_HEX_H2;
    uint32_t n = (qx ^ qy) * NV2A_HEX_H1;
    off[0] = (float)(n >> 16) * (1.0f / 65536.0f);
    off[1] = (float)((n * NV2A_HEX_H2) >> 16) * (1.0f / 65536.0f);
}

/* Whether a stage may sample hex-tiled: wrap on both axes (SET_TEXTURE_ADDRESS
 * mode 1). A copy reads up to a whole texture away from t, so clamp would
 * smear the edge texels over it and mirror would flip it; the pack's mark
 * comes from a trace that may never have seen the texture bound that way. */
static inline int nv2a_hex_wrap_ok(uint32_t addr_u, uint32_t addr_v)
{
    return (addr_u & 0xFu) == 1u && (addr_v & 0xFu) == 1u;
}

/* The blend weights from the grid's and each copy's luminance. */
static inline void nv2a_hex_weights(const float w[3], const float luma[3], float out[3])
{
    float sum = 0.0f;
    int k;
    for (k = 0; k < 3; k++) {
        float w2 = w[k] * w[k];
        out[k] = w2 * w2 * w2 * w[k] * (1.0f + 0.6f * (luma[k] - 1.0f));
        sum += out[k];
    }
    for (k = 0; k < 3; k++)
        out[k] /= sum;
}

/* The shader texts. The caller emits them only for the hex variant
 * (NV2ACombinerState.hex_tile); the stock text never holds them. */
#define D3D8_HLSL_HEX \
    "cbuffer TexDetile : register(b4) { uint4 tex_detile; };\n" \
    "float2 nv2a_hex_off(int2 v) {\n" \
    "    uint2 q = uint2(v) * uint2(1597334673u, 3812015801u);\n" \
    "    uint n = (q.x ^ q.y) * 1597334673u;\n" \
    "    return float2(n >> 16, (n * 3812015801u) >> 16) * (1.0 / 65536.0);\n" \
    "}\n" \
    "float4 nv2a_hex(Texture2D tx, SamplerState sm, float2 t, float2 dx, float2 dy) {\n" \
    "    float2 k = t * 3.46410162;\n" \
    "    k = float2(k.x - k.y * 0.57735027, k.y * 1.15470054);\n" \
    "    float2 b = floor(k), f = k - b;\n" \
    "    float z = 1.0 - f.x - f.y;\n" \
    "    float s = z < 0.0 ? 1.0 : 0.0, sg = 2.0 * s - 1.0;\n" \
    "    float3 w = float3(-z * sg, s - f.y * sg, s - f.x * sg);\n" \
    "    int2 ib = int2(b); int si = int(s);\n" \
    "    float4 c0 = tx.SampleGrad(sm, t + nv2a_hex_off(ib + int2(si, si)), dx, dy);\n" \
    "    float4 c1 = tx.SampleGrad(sm, t + nv2a_hex_off(ib + int2(si, 1 - si)), dx, dy);\n" \
    "    float4 c2 = tx.SampleGrad(sm, t + nv2a_hex_off(ib + int2(1 - si, si)), dx, dy);\n" \
    "    float3 l = mul(float3x3(c0.rgb, c1.rgb, c2.rgb), float3(0.299, 0.587, 0.114));\n" \
    "    float3 w2 = w * w;\n" \
    "    float3 W = w2 * w2 * w2 * w * lerp(1.0, l, 0.6);\n" \
    "    W /= W.x + W.y + W.z;\n" \
    "    return c0 * W.x + c1 * W.y + c2 * W.z;\n" \
    "}\n"

/* As D3D8_HLSL_HEX; the flags come in buffer(3) (fs_main's hex). */
#define D3D8_MSL_HEX \
    "static inline float2 nv2a_hex_off(int2 v) {\n" \
    "    uint2 q = uint2(v) * uint2(1597334673u, 3812015801u);\n" \
    "    uint n = (q.x ^ q.y) * 1597334673u;\n" \
    "    return float2(float(n >> 16), float((n * 3812015801u) >> 16)) * (1.0 / 65536.0);\n" \
    "}\n" \
    "static inline float4 nv2a_hex(texture2d<float> tx, sampler sm, float2 t, float2 dx,\n" \
    "                              float2 dy) {\n" \
    "    float2 k = t * 3.46410162;\n" \
    "    k = float2(k.x - k.y * 0.57735027, k.y * 1.15470054);\n" \
    "    float2 b = floor(k), f = k - b;\n" \
    "    float z = 1.0 - f.x - f.y;\n" \
    "    float s = z < 0.0 ? 1.0 : 0.0, sg = 2.0 * s - 1.0;\n" \
    "    float3 w = float3(-z * sg, s - f.y * sg, s - f.x * sg);\n" \
    "    int2 ib = int2(b); int si = int(s);\n" \
    "    float4 c0 = tx.sample(sm, t + nv2a_hex_off(ib + int2(si, si)), gradient2d(dx, dy));\n" \
    "    float4 c1 = tx.sample(sm, t + nv2a_hex_off(ib + int2(si, 1 - si)), gradient2d(dx, dy));\n" \
    "    float4 c2 = tx.sample(sm, t + nv2a_hex_off(ib + int2(1 - si, si)), gradient2d(dx, dy));\n" \
    "    float3 l = float3(dot(c0.rgb, float3(0.299, 0.587, 0.114)),\n" \
    "                      dot(c1.rgb, float3(0.299, 0.587, 0.114)),\n" \
    "                      dot(c2.rgb, float3(0.299, 0.587, 0.114)));\n" \
    "    float3 w2 = w * w;\n" \
    "    float3 W = w2 * w2 * w2 * w * mix(float3(1.0), l, 0.6);\n" \
    "    W /= W.x + W.y + W.z;\n" \
    "    return c0 * W.x + c1 * W.y + c2 * W.z;\n" \
    "}\n"

#endif /* NV2A_HEX_TILE_H */
