/* nv2a_basic_hlsl.h - the D3D11 backend's shader declarations and its
 * basic pixel shader (batches without register combiners), as text. In a
 * header so that tests/basic_shaders locks the stock text with a checksum
 * and d3dcompile_smoke compiles both variants. */
#ifndef NV2A_BASIC_HLSL_H
#define NV2A_BASIC_HLSL_H

#include "d3d8_hlsl.h"

/* struct nv2a_ps_consts (PsConsts, nv2a_pb_d3d11.c) in HLSL. */
#define PS_CB_DECL \
    "cbuffer CombinerCB : register(b0) {\n" \
    "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n" \
    "    float alpha_ref; uint alpha_func; uint alpha_test_enable; uint fog_enable;\n" \
    "    uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n" \
    "    float4 bump[4]; float4 bumpls[4]; uint4 clip_mode;\n" \
    "};\n"

/* The VS output struct d3d8_vsh.c emits. A pixel shader that declares the
 * same struct links with any of its vertex shaders. */
#define VS_OUT_DECL \
    "struct VS_OUT {\n" \
    "    float4 oPos : SV_POSITION;\n" \
    "    float4 oD0  : COLOR0;\n" \
    "    float4 oD1  : COLOR1;\n" \
    "    float4 oT0  : TEXCOORD0;\n" \
    "    float4 oT1  : TEXCOORD1;\n" \
    "    float4 oT2  : TEXCOORD2;\n" \
    "    float4 oT3  : TEXCOORD3;\n" \
    "    float  oFog : FOG;\n" \
    "    float  oPts : PSIZE;\n" \
    "    float4 oB0  : TEXCOORD4;\n" \
    "    float4 oB1  : TEXCOORD5;\n" \
    "};\n"

/* Batches without a vertex program: attribute 0 is already in surface pixels
 * (x, y, z, rhw), snapped to the NV2A's 1/16-pixel grid (nv2a_screen_snap),
 * flat (rhw 1) ties moved by vp_off.zw (nv2a_vp_tie_fill);
 * 3 and 4 are the diffuse and specular colours, 9..12 the texture
 * coordinates. Here so tests/basic_shaders pins it, as vs_pass in
 * nv2a_basic_msl.h. */
#define NV2A_HLSL_VS_PASS \
    "cbuffer VSH_Viewport : register(b2) { float4 vp_scale; float4 vp_off; };\n" \
    "struct VS_IN { float4 v0 : ATTR0; float4 v3 : ATTR3; float4 v4 : ATTR4;\n" \
    "               float4 v9 : ATTR9; float4 v10 : ATTR10; float4 v11 : ATTR11;\n" \
    "               float4 v12 : ATTR12; };\n" \
    VS_OUT_DECL \
    "VS_OUT main(VS_IN i) {\n" \
    "    VS_OUT o = (VS_OUT)0;\n" \
    "    float2 p = trunc(i.v0.xy * 16.0) / 16.0;\n" \
    "    p -= vp_off.zw * (float2)(frac(p) == 0.5) * (float)(i.v0.w == 1.0);\n" \
    "    o.oPos = float4(p * vp_scale.xy + vp_off.xy,\n" \
    "                    saturate(i.v0.z * vp_scale.z), 1.0);\n" \
    "    o.oD0 = i.v3; o.oD1 = i.v4; o.oFog = 1.0;\n" \
    "    o.oT0 = i.v9; o.oT1 = i.v10; o.oT2 = i.v11; o.oT3 = i.v12;\n" \
    "    return o;\n" \
    "}\n"

/* Batches without register combiners: the CPU path's MODULATE, oD0 * T0
 * when stage 0 is on, a CLIPPLANE stage 0's kill, then the alpha test
 * (D3DCMP numbering, on the 8-bit values the way the CPU compares them).
 * SNAP is the snap helpers' text, SAMPLE stage 0's sample into t. */
#define PS_BASIC_SRC(SNAP, SAMPLE) \
    VS_OUT_DECL \
    PS_CB_DECL \
    SNAP \
    D3D8_HLSL_CLIP_KILLS \
    "Texture2D tex0 : register(t0);\n" \
    "SamplerState samp0 : register(s0);\n" \
    "float4 main(VS_OUT i) : SV_Target {\n" \
    "    float4 c = i.oD0;\n" \
    "    if (tex_mode.x == 4u) {\n" \
    "        c *= saturate(i.oT0);\n" \
    "    } else if (tex_mode.x != 0u) {\n" \
    "        float2 uv = i.oT0.xy;\n" \
    "        if (tex_mode.x == 1u && i.oT0.w != 0.0 && i.oT0.w != 1.0) uv /= i.oT0.w;\n" \
    SAMPLE \
    "        if (alpha_only.x == 2u) t.a = 1.0;\n" \
    "        c *= t;\n" \
    "    }\n" \
    "    if (nv2a_clip_kills(clip_mode.x, i.oT0)) discard;\n" \
    "    if (alpha_test_enable != 0u) {\n" \
    "        uint a = (uint)(saturate(c.a) * 255.0 + 0.5), r = (uint)(alpha_ref * 255.0 + 0.5);\n" \
    "        bool ok = true;\n" \
    "        if      (alpha_func == 1u) ok = false;\n" \
    "        else if (alpha_func == 2u) ok = a <  r;\n" \
    "        else if (alpha_func == 3u) ok = a == r;\n" \
    "        else if (alpha_func == 4u) ok = a <= r;\n" \
    "        else if (alpha_func == 5u) ok = a >  r;\n" \
    "        else if (alpha_func == 6u) ok = a != r;\n" \
    "        else if (alpha_func == 7u) ok = a >= r;\n" \
    "        if (!ok) discard;\n" \
    "    }\n" \
    "    return c;\n" \
    "}\n"

/* tex_scale.w: the hor+ offset into a padded target, 0 otherwise */
#define NV2A_HLSL_PS_BASIC PS_BASIC_SRC( \
    D3D8_HLSL_SNAP_UV, \
    "        uv = nv2a_snap_uv(uv, i.oPos.xy, tex_scale[0]);\n" \
    "        float4 t = tex0.Sample(samp0, uv * tex_scale[0].xy + float2(tex_scale[0].w, 0.0));\n")

/* render.snap_2d's (nv2a_snap_2d_on), built when it first draws: a
 * 2D-snapped stage (.z < 0) samples with the 1x gradients (nv2a_snap_grad).
 * The stock s_ps_src keeps its text, gradients and branch left out. */
#define NV2A_HLSL_PS_BASIC_SNAP PS_BASIC_SRC( \
    D3D8_HLSL_SNAP_UV D3D8_HLSL_SNAP_2D, \
    "        float4 g = nv2a_snap_grad(uv, tex_scale[0]) * tex_scale[0].xyxy;\n" \
    "        uv = nv2a_snap_uv2d(uv, i.oPos.xy, tex_scale[0]);\n" \
    "        uv = uv * tex_scale[0].xy + float2(tex_scale[0].w, 0.0);\n" \
    "        float4 t;\n" \
    "        if (tex_scale[0].z < 0.0) t = tex0.SampleGrad(samp0, uv, g.xy, g.zw);\n" \
    "        else t = tex0.Sample(samp0, uv);\n")

#endif
