/**
 * MSL (Metal Shading Language) emitters for NV2A vertex programs
 * (d3d8_vsh_msl.c) and register combiners (d3d8_combiners_msl.c). They are the
 * Metal counterparts of d3d8_hlsl.h, statement for statement: the same parsed
 * state goes in (d3d8_vsh_parse.h, d3d8_combiners_parse.h) and shader source
 * text comes out. They build strings only and use no graphics API, so they
 * build and can be tested on every host. The Metal backend
 * (nv2a_pb_metal.m) compiles the text at runtime with fast math off.
 *
 * The interface between the stages is D3D8_MSL_VOUT: the vertex function
 * returns it and every fragment function takes it as [[stage_in]]. The
 * members carry [[user(...)]] names, so a vertex function and a fragment
 * function from different libraries still link.
 */

#ifndef XBOXRECOMP_D3D8_MSL_H
#define XBOXRECOMP_D3D8_MSL_H

#include "d3d8_vsh_parse.h"
#include "d3d8_combiners_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The vertex-to-fragment struct, as MSL source. */
#define D3D8_MSL_VOUT \
    "struct VOut {\n" \
    "    float4 pos [[position]];\n" \
    "    float4 d0  [[user(d0)]];\n" \
    "    float4 d1  [[user(d1)]];\n" \
    "    float4 t0  [[user(t0)]];\n" \
    "    float4 t1  [[user(t1)]];\n" \
    "    float4 t2  [[user(t2)]];\n" \
    "    float4 t3  [[user(t3)]];\n" \
    "    float  fog [[user(fog)]];\n" \
    "    float  psize [[point_size]];\n" \
    "};\n"

/** The fragment constants: struct nv2a_ps_consts (nv2a_backend_common.h)
 * declared in float4/uint4 only, then Metal's own tex_bias (stage k's mip
 * LOD bias, passed to sample() as bias()). atest is (alpha_ref as float
 * bits, alpha_func, alpha_test_enable, fog_enable). */
#define D3D8_MSL_PSCONSTS \
    "struct PsConsts {\n" \
    "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n" \
    "    uint4 atest; uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n" \
    "    float4 bump[4]; float4 bumpls[4];\n" \
    "    float4 tex_bias;\n" \
    "};\n"

/** D3D8_HLSL_SNAP_UV (d3d8_hlsl.h) in MSL; pos is [[position]].xy. */
#define D3D8_MSL_SNAP_UV \
    "float2 nv2a_snap_uv(float2 uv, float2 pos, float4 ts) {\n" \
    "    float2 dx = dfdx(uv), dy = dfdy(uv);\n" \
    "    float n = ts.z, eps = 1.0 / 64.0;\n" \
    "    if (n > 1.0 && abs(dx.y) <= eps && abs(dy.x) <= eps\n" \
    "            && abs(abs(dx.x) * n - 1.0) <= eps && abs(abs(dy.y) * n - 1.0) <= eps) {\n" \
    "        float2 d = (floor(pos / n) + 0.5) * n - pos;\n" \
    "        uv += dx * d.x + dy * d.y;\n" \
    "    }\n" \
    "    return uv;\n" \
    "}\n"

/**
 * The vertex function `vs_main`:
 *
 *   vertex VOut vs_main(uint vid [[vertex_id]],
 *                       const device float4 *vb [[buffer(0)]],
 *                       constant float4 *c [[buffer(1)]],      c[192]
 *                       constant VpConsts &vp [[buffer(2)]])   nv2a_vp_consts
 *
 * vb holds, for each vertex, one float4 per input the program reads
 * (program->inputs_read), in ascending register order. Vertex vid's vN is
 * at vb[vid * popcount(inputs_read) + (the number of inputs below N)].
 *
 * With D3D8_MSL_SCREEN_SPACE, the function maps oPos from surface pixels to
 * clip space, and applies fog and specular, as NV2A_VSH_HLSL_SCREEN_SPACE
 * does (see d3d8_hlsl.h). VpConsts is vp_scale, vp_off, fog_param, then
 * uint4 fog_ctl and uint4 spec_ctl.
 */
#define D3D8_MSL_SCREEN_SPACE 0x1

int d3d8_vsh_generate_msl(const NV2AVshProgram *program, unsigned flags,
                          char *buf, int bufsize);

/**
 * The fragment function `fs_main`. It evaluates the combiners as
 * d3d8_combiners_generate_hlsl does, then runs the alpha test:
 *
 *   fragment float4 fs_main(VOut i [[stage_in]],
 *                           constant PsConsts &pc [[buffer(0)]],
 *                           texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]],
 *                           ...one pair for each stage the state samples)
 *
 * A 3D stage gets texture3d and a cube-map stage texturecube. With
 * state->rect_snap, 2D stages go through D3D8_MSL_SNAP_UV.
 */
int d3d8_combiners_generate_msl(const NV2ACombinerState *state,
                                char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_MSL_H */
