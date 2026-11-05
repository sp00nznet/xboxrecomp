/* nv2a_basic_msl.h - the Metal backend's basic library as text: fs_basic
 * (batches without register combiners), vs_pass, the clears, and
 * render.snap_2d's fs_basic_snap. In a header so that tests/basic_shaders
 * locks the stock text with a checksum and compiles both with Metal. */
#ifndef NV2A_BASIC_MSL_H
#define NV2A_BASIC_MSL_H

#include "d3d8_msl.h"

/* fs_basic's text: SAMPLE is stage 0's sample into t. */
#define MSL_FS_BASIC(NAME, SAMPLE) \
    "fragment float4 " NAME "(VOut i [[stage_in]],\n" \
    "                         constant PsConsts &pc [[buffer(0)]],\n" \
    "                         texture2d<float> tex0 [[texture(0)]],\n" \
    "                         sampler samp0 [[sampler(0)]]) {\n" \
    "    float4 c = i.d0;\n" \
    "    if (pc.tex_mode.x == 4u) {\n" \
    "        c *= saturate(i.t0);\n" \
    "    } else if (pc.tex_mode.x != 0u) {\n" \
    "        float2 uv = i.t0.xy;\n" \
    "        if (pc.tex_mode.x == 1u && i.t0.w != 0.0 && i.t0.w != 1.0) uv /= i.t0.w;\n" \
    SAMPLE \
    /* alpha_only 2: a target read as a format without alpha (nv2a_rt_no_alpha) */ \
    "        if (pc.alpha_only.x == 2u) t.a = 1.0;\n" \
    "        c *= t;\n" \
    "    }\n" \
    "    if (nv2a_clip_kills(pc.clip_mode.x, i.t0)) discard_fragment();\n" \
    "    if (pc.atest.z != 0u) {\n" \
    "        float ref = as_type<float>(pc.atest.x);\n" \
    "        uint a = (uint)(saturate(c.a) * 255.0 + 0.5), r = (uint)(ref * 255.0 + 0.5);\n" \
    "        bool ok = true;\n" \
    "        uint f = pc.atest.y;\n" \
    "        if      (f == 1u) ok = false;\n" \
    "        else if (f == 2u) ok = a <  r;\n" \
    "        else if (f == 3u) ok = a == r;\n" \
    "        else if (f == 4u) ok = a <= r;\n" \
    "        else if (f == 5u) ok = a >  r;\n" \
    "        else if (f == 6u) ok = a != r;\n" \
    "        else if (f == 7u) ok = a >= r;\n" \
    "        if (!ok) discard_fragment();\n" \
    "    }\n" \
    "    return c;\n" \
    "}\n"

#define NV2A_MSL_BASIC \
    "#include <metal_stdlib>\n" \
    "using namespace metal;\n" \
    "struct VpConsts { float4 c[3]; uint4 u[2]; };\n" \
    D3D8_MSL_PSCONSTS \
    /* The emitters' VOut (d3d8_msl.h), so vs_pass links with a combiner \
     * fs_main and a program's vs_main with fs_basic. */ \
    D3D8_MSL_VOUT \
    /* Attribute 0 is already in surface pixels (x, y, z, rhw), snapped to \
     * the NV2A's 1/16-pixel grid (nv2a_screen_snap): c[0] is (2/w, -2/h, \
     * 1/zmax, 0) and c[1] (-1, 1, tie shift x, y in guest pixels, rhw 1 \
     * only: nv2a_vp_tie_fill), as the D3D11 pass-through's vp_scale / \
     * vp_off. */ \
    "vertex VOut vs_pass(uint vid [[vertex_id]],\n" \
    "                    const device float4 *vb [[buffer(0)]],\n" \
    "                    constant VpConsts &vp [[buffer(1)]]) {\n" \
    "    const device float4 *v = vb + vid * 7u;\n" \
    "    VOut o;\n" \
    "    float2 p = trunc(v[0].xy * 16.0) / 16.0;\n" \
    "    p -= vp.c[1].zw * float2(fract(p) == 0.5) * float(v[0].w == 1.0);\n" \
    "    o.pos = float4(p * vp.c[0].xy + vp.c[1].xy,\n" \
    "                   saturate(v[0].z * vp.c[0].z), 1.0);\n" \
    "    o.d0 = v[1]; o.d1 = v[2];\n" \
    "    o.t0 = v[3]; o.t1 = v[4]; o.t2 = v[5]; o.t3 = v[6];\n" \
    "    o.fog = 1.0; o.psize = 1.0;\n" \
    "    return o;\n" \
    "}\n" \
    /* Batches without register combiners (and, in M1, all of them): the CPU \
     * path's MODULATE, oD0 * T0 when stage 0 is on, a CLIPPLANE stage 0's \
     * kill, then the alpha test (D3DCMP numbering, on the 8-bit values the \
     * way the CPU compares them). Line for line the D3D11 backend's s_ps_src. */ \
    D3D8_MSL_SNAP_UV \
    D3D8_MSL_CLIP_KILLS \
    MSL_FS_BASIC("fs_basic", \
        "        uv = nv2a_snap_uv(uv, i.pos.xy, pc.tex_scale[0]);\n" \
        /* tex_scale.w: the hor+ offset into a padded target, 0 otherwise */ \
        "        float4 t = tex0.sample(samp0, uv * pc.tex_scale[0].xy" \
        " + float2(pc.tex_scale[0].w, 0.0), bias(pc.tex_bias.x));\n") \
    /* A colour clear of part of a target (metal_on_clear): a full-screen \
     * triangle under a scissor, the clear colour as it is. */ \
    "struct ClearV { float4 pos [[position]]; };\n" \
    "vertex ClearV vs_clear(uint vid [[vertex_id]]) {\n" \
    "    float2 t = float2((vid << 1) & 2, vid & 2);\n" \
    "    ClearV o;\n" \
    "    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n" \
    "    return o;\n" \
    "}\n" \
    "fragment float4 fs_clear(constant float4 &c [[buffer(0)]]) { return c; }\n"

/* fs_basic with render.snap_2d's sampling (nv2a_snap_2d_on), compiled the
 * first time the setting draws (fs_basic_snap) so a stock run never builds
 * it: a 2D-snapped stage (.z < 0) samples with the 1x gradients
 * (nv2a_snap_grad), its LOD bias folded into them, as gradient2d takes no
 * bias(). */
#define NV2A_MSL_BASIC_SNAP \
    "#include <metal_stdlib>\n" \
    "using namespace metal;\n" \
    D3D8_MSL_PSCONSTS \
    D3D8_MSL_VOUT \
    D3D8_MSL_SNAP_UV \
    D3D8_MSL_SNAP_2D \
    D3D8_MSL_CLIP_KILLS \
    MSL_FS_BASIC("fs_basic_snap", \
        "        float4 g = nv2a_snap_grad(uv, pc.tex_scale[0]) * pc.tex_scale[0].xyxy" \
        " * exp2(pc.tex_bias.x);\n" \
        "        uv = nv2a_snap_uv2d(uv, i.pos.xy, pc.tex_scale[0]);\n" \
        "        uv = uv * pc.tex_scale[0].xy + float2(pc.tex_scale[0].w, 0.0);\n" \
        "        float4 t;\n" \
        "        if (pc.tex_scale[0].z < 0.0) t = tex0.sample(samp0, uv, gradient2d(g.xy, g.zw));\n" \
        "        else t = tex0.sample(samp0, uv, bias(pc.tex_bias.x));\n")

#endif
