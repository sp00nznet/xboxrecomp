/*
 * nv2a_scene_ao_shaders.h - render.ao's two passes, MSL and HLSL
 * (nv2a_scene_ao.h). The same maths twice, kept side by side so a change
 * to one is made to the other (the backends agree).
 *
 *  1. ao_fs, into an R8 target of the 3D target's host size: for each
 *     pixel the view-space position from the depth buffer (the frame's fit:
 *     w = B / (D - A), or D * zmax under a W-buffer) and an assumed 60
 *     degree field, the normal from the nearer of each pair of neighbours
 *     (no halo across a silhouette), then 8 directions x 3 steps of a
 *     horizon test, HBAO style, each weighted by a falloff over a world
 *     radius that projects to the screen-constant p1.x pixels. Rotation and
 *     step jitter come from a 4 x 4 Bayer cell: the same every frame (no
 *     temporal noise to flicker) and cancelled by pass 2's 4 x 4 window.
 *     Faded out towards the scene's depth (p1.yzw), where a 24-bit buffer
 *     is too coarse.
 *  2. ao_comp_fs, drawn over the 3D target with a multiply blend (colour
 *     times the result, alpha untouched): the 4 x 4 window, each tap
 *     weighted by its depth's closeness to the centre's (a bilateral blur:
 *     foliage speckle smooths out, an edge does not bleed). Under ao_view
 *     the same output replaces the colour.
 *
 * Nothing samples with a sampler: integer reads, clamped to the target,
 * so the passes are exact at every render scale and need no state.
 */
#ifndef NV2A_SCENE_AO_SHADERS_H
#define NV2A_SCENE_AO_SHADERS_H

#define NV2A_AO_MSL \
"#include <metal_stdlib>\n" \
"using namespace metal;\n" \
"struct AoC { float4 p0, p1, p2, p3; };\n" \
"struct AoV { float4 pos [[position]]; };\n" \
"vertex AoV ao_vs(uint vid [[vertex_id]]) {\n" \
"    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));\n" \
"    AoV o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;\n" \
"}\n" \
"static float ao_d(depth2d<float> t, int2 p, constant AoC &c) {\n" \
"    p = clamp(p, int2(0), int2(c.p2.xy) - 1);\n" \
"    return t.read(uint2(p));\n" \
"}\n" \
"static float ao_w(float d, constant AoC &c) {\n" \
"    if (c.p0.z > 0.5) return d * c.p0.w;\n" \
"    return c.p0.y / min(d - c.p0.x, -1e-7);\n" \
"}\n" \
"static float3 ao_pos(float2 px, float w, constant AoC &c) {\n" \
"    float2 n = float2(px.x / c.p2.x * 2.0 - 1.0, 1.0 - px.y / c.p2.y * 2.0);\n" \
"    return float3(n.x * c.p2.z * w, n.y * c.p2.w * w, w);\n" \
"}\n" \
"static float3 ao_at(depth2d<float> t, int2 p, constant AoC &c) {\n" \
"    return ao_pos(float2(p) + 0.5, ao_w(ao_d(t, p, c), c), c);\n" \
"}\n" \
"constant uint ao_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n" \
"fragment float ao_fs(AoV i [[stage_in]], depth2d<float> dt [[texture(0)]],\n" \
"                     constant AoC &c [[buffer(0)]]) {\n" \
"    int2 p = int2(i.pos.xy);\n" \
"    float d = ao_d(dt, p, c);\n" \
"    if (d >= 0.99999) return 1.0;\n" \
"    float3 P = ao_at(dt, p, c);\n" \
"    float3 r = ao_at(dt, p + int2(1, 0), c), l = ao_at(dt, p - int2(1, 0), c);\n" \
"    float3 dn = ao_at(dt, p + int2(0, 1), c), up = ao_at(dt, p - int2(0, 1), c);\n" \
"    float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;\n" \
"    float3 dy = abs(dn.z - P.z) < abs(P.z - up.z) ? dn - P : P - up;\n" \
"    float3 N = normalize(cross(dx, dy));\n" \
"    if (N.z > 0.0) N = -N;\n" \
"    float rpx = c.p1.x;\n" \
"    float R = rpx * 2.0 * c.p2.w * P.z / c.p2.y;\n" \
"    float R2 = R * R;\n" \
"    uint b = ao_bayer[(uint(p.y) & 3u) * 4u + (uint(p.x) & 3u)];\n" \
"    float rot = (float(b) + 0.5) * (6.2831853 / 128.0);\n" \
"    float jit = float((b * 7u) & 15u) / 16.0;\n" \
"    float occ = 0.0;\n" \
"    for (int k = 0; k < 8; k++) {\n" \
"        float a = rot + float(k) * (6.2831853 / 8.0);\n" \
"        float2 dir = float2(cos(a), sin(a));\n" \
"        for (int s = 0; s < 3; s++) {\n" \
"            float t = max((float(s) + jit) / 3.0 * rpx, 1.0);\n" \
"            int2 q = int2(floor(float2(p) + 0.5 + dir * t));\n" \
"            float3 V = ao_at(dt, q, c) - P;\n" \
"            float dd = dot(V, V);\n" \
"            float fall = saturate(1.0 - dd / R2);\n" \
"            occ += saturate(dot(N, V) * rsqrt(max(dd, 1e-20)) - c.p3.y) * fall;\n" \
"        }\n" \
"    }\n" \
"    float ao = saturate(1.0 - c.p3.x * occ * (2.0 / 24.0));\n" \
"    float fade = 1.0 - smoothstep(c.p1.z, c.p1.w, P.z * c.p1.y);\n" \
"    return mix(1.0, ao, fade);\n" \
"}\n" \
"fragment float4 ao_comp_fs(AoV i [[stage_in]], depth2d<float> dt [[texture(0)]],\n" \
"                           texture2d<float> at [[texture(1)]], constant AoC &c [[buffer(0)]]) {\n" \
"    int2 p = int2(i.pos.xy);\n" \
"    float d = ao_d(dt, p, c);\n" \
"    if (d >= 0.99999) return float4(1.0);\n" \
"    float wc = ao_w(d, c), sum = 0.0, ws = 0.0;\n" \
"    for (int y = -2; y < 2; y++)\n" \
"        for (int x = -2; x < 2; x++) {\n" \
"            int2 q = clamp(p + int2(x, y), int2(0), int2(c.p2.xy) - 1);\n" \
"            float sd = dt.read(uint2(q));\n" \
"            float wt = sd >= 0.99999 ? 0.0\n" \
"                     : max(0.0, 1.0 - abs(ao_w(sd, c) - wc) / (0.04 * wc)) + 1e-4;\n" \
"            sum += at.read(uint2(q)).r * wt;\n" \
"            ws += wt;\n" \
"        }\n" \
"    float a = ws > 0.0 ? sum / ws : 1.0;\n" \
"    return float4(a, a, a, 1.0);\n" \
"}\n"

#define NV2A_AO_HLSL \
"cbuffer AoC : register(b0) { float4 p0, p1, p2, p3; };\n" \
"Texture2D<float> dt : register(t0);\n" \
"Texture2D<float> at : register(t1);\n" \
"float4 ao_vs(uint vid : SV_VertexID) : SV_Position {\n" \
"    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));\n" \
"    return float4(p * 2.0 - 1.0, 0.0, 1.0);\n" \
"}\n" \
"float ao_d(int2 p) {\n" \
"    p = clamp(p, int2(0, 0), int2(p2.xy) - 1);\n" \
"    return dt.Load(int3(p, 0));\n" \
"}\n" \
"float ao_w(float d) {\n" \
"    if (p0.z > 0.5) return d * p0.w;\n" \
"    return p0.y / min(d - p0.x, -1e-7);\n" \
"}\n" \
"float3 ao_pos(float2 px, float w) {\n" \
"    float2 n = float2(px.x / p2.x * 2.0 - 1.0, 1.0 - px.y / p2.y * 2.0);\n" \
"    return float3(n.x * p2.z * w, n.y * p2.w * w, w);\n" \
"}\n" \
"float3 ao_at(int2 p) { return ao_pos(float2(p) + 0.5, ao_w(ao_d(p))); }\n" \
"static const uint ao_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n" \
"float ao_fs(float4 pos : SV_Position) : SV_Target {\n" \
"    int2 p = int2(pos.xy);\n" \
"    float d = ao_d(p);\n" \
"    if (d >= 0.99999) return 1.0;\n" \
"    float3 P = ao_at(p);\n" \
"    float3 r = ao_at(p + int2(1, 0)), l = ao_at(p - int2(1, 0));\n" \
"    float3 dn = ao_at(p + int2(0, 1)), up = ao_at(p - int2(0, 1));\n" \
"    float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;\n" \
"    float3 dy = abs(dn.z - P.z) < abs(P.z - up.z) ? dn - P : P - up;\n" \
"    float3 N = normalize(cross(dx, dy));\n" \
"    if (N.z > 0.0) N = -N;\n" \
"    float rpx = p1.x;\n" \
"    float R = rpx * 2.0 * p2.w * P.z / p2.y;\n" \
"    float R2 = R * R;\n" \
"    uint b = ao_bayer[(uint(p.y) & 3u) * 4u + (uint(p.x) & 3u)];\n" \
"    float rot = (float(b) + 0.5) * (6.2831853 / 128.0);\n" \
"    float jit = float((b * 7u) & 15u) / 16.0;\n" \
"    float occ = 0.0;\n" \
"    [unroll] for (int k = 0; k < 8; k++) {\n" \
"        float a = rot + float(k) * (6.2831853 / 8.0);\n" \
"        float2 dir = float2(cos(a), sin(a));\n" \
"        [unroll] for (int s = 0; s < 3; s++) {\n" \
"            float t = max((float(s) + jit) / 3.0 * rpx, 1.0);\n" \
"            int2 q = int2(floor(float2(p) + 0.5 + dir * t));\n" \
"            float3 V = ao_at(q) - P;\n" \
"            float dd = dot(V, V);\n" \
"            float fall = saturate(1.0 - dd / R2);\n" \
"            occ += saturate(dot(N, V) * rsqrt(max(dd, 1e-20)) - p3.y) * fall;\n" \
"        }\n" \
"    }\n" \
"    float ao = saturate(1.0 - p3.x * occ * (2.0 / 24.0));\n" \
"    float fade = 1.0 - smoothstep(p1.z, p1.w, P.z * p1.y);\n" \
"    return lerp(1.0, ao, fade);\n" \
"}\n" \
"float4 ao_comp_fs(float4 pos : SV_Position) : SV_Target {\n" \
"    int2 p = int2(pos.xy);\n" \
"    float d = ao_d(p);\n" \
"    if (d >= 0.99999) return float4(1.0, 1.0, 1.0, 1.0);\n" \
"    float wc = ao_w(d), sum = 0.0, ws = 0.0;\n" \
"    [unroll] for (int y = -2; y < 2; y++)\n" \
"        [unroll] for (int x = -2; x < 2; x++) {\n" \
"            int2 q = clamp(p + int2(x, y), int2(0, 0), int2(p2.xy) - 1);\n" \
"            float sd = dt.Load(int3(q, 0));\n" \
"            float wt = sd >= 0.99999 ? 0.0\n" \
"                     : max(0.0, 1.0 - abs(ao_w(sd) - wc) / (0.04 * wc)) + 1e-4;\n" \
"            sum += at.Load(int3(q, 0)) * wt;\n" \
"            ws += wt;\n" \
"        }\n" \
"    float a = ws > 0.0 ? sum / ws : 1.0;\n" \
"    return float4(a, a, a, 1.0);\n" \
"}\n"

#endif /* NV2A_SCENE_AO_SHADERS_H */
