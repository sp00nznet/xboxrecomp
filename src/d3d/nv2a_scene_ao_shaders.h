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
 *     foliage speckle smooths out, an edge does not bleed). Its second
 *     target (RG32F) records each pixel's total occlusion so far this
 *     frame and its depth then. A top-up (p3.w, a later chunk of the same
 *     frame: nv2a_scene_ao.h) reads the last pass's record (texture 2):
 *     where the depth is unchanged the pixel already carries that total,
 *     so it is multiplied by new / old only (never above 1: a multiply
 *     cannot brighten, and the record keeps what was applied, so a chain
 *     of top-ups stays exact), unless its colour moved since that pass
 *     left it: texture 3 is a copy of the colour then, texture 4 of the
 *     colour now. A draw without depth writes covered it (a translucent
 *     chunk: the BLiNX 2 Stage 1 pool's water), whose content
 *     has had no occlusion, so it takes the new map in full, as a single
 *     pass after that draw would give it. With the ratio there, the frames
 *     whose pass fell before the water showed the water unoccluded and
 *     those after it occluded, and the pool's edge flickered as the event
 *     count moved by one while the camera panned. A move of up to 0.02 a
 *     channel keeps the ratio, from 0.06 the full map, a ramp between
 *     (nv2a_ao_top_full; the literals here are NV2A_AO_TOP_LO and
 *     TOP_HI - TOP_LO, keep them in step). LO is a rounding step on 8-bit
 *     channels only: on a 16-bit target (R5G6B5) one red or blue step is
 *     about 0.032, so a dither or one-step change lands mid-ramp there. A
 *     top-up whose window held a flat draw (p3.w 3, nv2a_ao_flat_draw:
 *     depth test off or ALWAYS, or a modulating blend) keeps the ratio
 *     everywhere and reads no colour: the move there may be the HUD, text
 *     or 2D over the scene, a full-screen glow or fog quad, or a lightmap
 *     pass over an occluded base, and the full map would occlude those a
 *     second time. A pixel whose depth changed was drawn again and gets
 *     the new map in full.
 *     Why this colour mask and not a delta (C filt + L (r - filt) with a
 *     replace blend, L the colour the last pass left, r the ratio): write
 *     what was drawn since as C = (1 - b) L + X. The truth is
 *     (1 - b) L r + X filt. The mask assumes b = 1 (exact for an opaque
 *     or near-opaque layer), the delta b = 0 (exact for an additive one),
 *     the ratio a modulating one. On the pool's water (alpha 0.8 over a
 *     floor at occlusion 0.6) the mask is 0.014 off, the delta 0.058, the
 *     old ratio 0.224; the delta also mixes a HUD with the scene under it,
 *     so it needs the flat gate as much, and the layers it gets right
 *     (glows, fog) are mostly drawn without a depth test, which the gate
 *     already hands to the ratio. The costs:
 *     - a translucent draw over a creased floor already occluded takes the
 *       full map over both, so the floor seen through it is occluded
 *       twice, by its uncovered share (1 - alpha); a faint glow (under
 *       0.02) is spared that;
 *     - an additive draw that tests depth (a sparkle, a 3D light shaft)
 *       does not make its window flat, so the pixels it covers are
 *       occluded twice where it moved them 0.02 or more: local, against a
 *       whole window back on the ratio for every particle;
 *     - an opaque draw with depth writes off, or an opaque replace pass at
 *       the same depth, takes the full map: what a single pass gives it;
 *     - a blended draw that writes depth, or a polygon-offset decal, moves
 *       the depth and takes the full occlusion, so the background seen
 *       through it is occluded twice;
 *     - a depth buffer swapped on the same target between passes makes
 *       every depth compare miss, and the top-up runs as a full pass.
 *     nv2a_ao_comp_px is this step in C, for the tests. Under ao_view the
 *     new map replaces the colour. A fallback (p3.w 2, a frame one event
 *     short of the memo) runs at the flip, after the HUD: it
 *     reads the colour copied at the event before (texture 3) and the
 *     colour now (texture 4). A pixel whose colour moved little since (a
 *     full-screen glow, fog: up to 0.06 a channel) takes the occlusion in
 *     full, one that moved a lot (the HUD, text, a reflection drawn over
 *     the floor: 0.20 or more) none, with a ramp between
 *     (nv2a_ao_fb_keep; the literals are NV2A_AO_FB_LO and
 *     FB_HI - FB_LO). An exact compare kept nothing in the story hub,
 *     where the glow drawn after the last 3D chunk changes nearly every
 *     pixel by a few levels.
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
"static float ao_occ(depth2d<float> dt, int2 p, constant AoC &c) {\n" \
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
"fragment float ao_fs(AoV i [[stage_in]], depth2d<float> dt [[texture(0)]],\n" \
"                     constant AoC &c [[buffer(0)]]) {\n" \
"    return ao_occ(dt, int2(i.pos.xy), c);\n" \
"}\n" \
"struct AoO { float4 c [[color(0)]]; float2 s [[color(1)]]; };\n" \
"fragment AoO ao_comp_fs(AoV i [[stage_in]], depth2d<float> dt [[texture(0)]],\n" \
"                        texture2d<float> at [[texture(1)]],\n" \
"                        texture2d<float> st [[texture(2)]],\n" \
"                        texture2d<float> sn [[texture(3)]],\n" \
"                        texture2d<float> cu [[texture(4)]], constant AoC &c [[buffer(0)]]) {\n" \
"    int2 p = int2(i.pos.xy);\n" \
"    float d = ao_d(dt, p, c), a = 1.0;\n" \
"    if (d < 0.99999) {\n" \
"        float wc = ao_w(d, c), sum = 0.0, ws = 0.0;\n" \
"        for (int y = -2; y < 2; y++)\n" \
"            for (int x = -2; x < 2; x++) {\n" \
"                int2 q = clamp(p + int2(x, y), int2(0), int2(c.p2.xy) - 1);\n" \
"                float sd = dt.read(uint2(q));\n" \
"                float wt = sd >= 0.99999 ? 0.0\n" \
"                         : max(0.0, 1.0 - abs(ao_w(sd, c) - wc) / (0.04 * wc)) + 1e-4;\n" \
"                sum += at.read(uint2(q)).r * wt;\n" \
"                ws += wt;\n" \
"            }\n" \
"        a = ws > 0.0 ? sum / ws : 1.0;\n" \
"    }\n" \
"    float t = a;\n" \
"    if (((c.p3.w > 0.5 && c.p3.w < 1.5) || c.p3.w > 2.5) && c.p3.z < 0.5) {\n" \
"        float2 pv = st.read(uint2(p)).rg;\n" \
"        if (pv.y == d) {\n" \
"            float k = 0.0;\n" \
"            if (c.p3.w < 1.5) {\n" \
"                float3 dc = abs(sn.read(uint2(p)).rgb - cu.read(uint2(p)).rgb);\n" \
"                k = saturate((max(dc.r, max(dc.g, dc.b)) - 0.02) / 0.04);\n" \
"            }\n" \
"            t = mix(min(a, pv.x), a, k);\n" \
"            a = mix(a < pv.x ? a / pv.x : 1.0, a, k);\n" \
"        }\n" \
"    }\n" \
"    if (c.p3.w > 1.5 && c.p3.w < 2.5) {\n" \
"        float3 dc = abs(sn.read(uint2(p)).rgb - cu.read(uint2(p)).rgb);\n" \
"        float k = saturate(1.0 - (max(dc.r, max(dc.g, dc.b)) - 0.06) / 0.14);\n" \
"        a = mix(1.0, a, k);\n" \
"        t = mix(1.0, t, k);\n" \
"    }\n" \
"    AoO o;\n" \
"    o.c = float4(a, a, a, 1.0);\n" \
"    o.s = float2(t, d);\n" \
"    return o;\n" \
"}\n"

#define NV2A_AO_HLSL \
"cbuffer AoC : register(b0) { float4 p0, p1, p2, p3; };\n" \
"Texture2D<float> dt : register(t0);\n" \
"Texture2D<float> at : register(t1);\n" \
"Texture2D<float2> st : register(t2);\n" \
"Texture2D<float4> sn : register(t3);\n" \
"Texture2D<float4> cu : register(t4);\n" \
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
"float ao_occ(int2 p) {\n" \
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
"float ao_fs(float4 pos : SV_Position) : SV_Target {\n" \
"    return ao_occ(int2(pos.xy));\n" \
"}\n" \
"struct AoO { float4 c : SV_Target0; float2 s : SV_Target1; };\n" \
"AoO ao_comp_fs(float4 pos : SV_Position) {\n" \
"    int2 p = int2(pos.xy);\n" \
"    float d = ao_d(p), a = 1.0;\n" \
"    if (d < 0.99999) {\n" \
"        float wc = ao_w(d), sum = 0.0, ws = 0.0;\n" \
"        [unroll] for (int y = -2; y < 2; y++)\n" \
"            [unroll] for (int x = -2; x < 2; x++) {\n" \
"                int2 q = clamp(p + int2(x, y), int2(0, 0), int2(p2.xy) - 1);\n" \
"                float sd = dt.Load(int3(q, 0));\n" \
"                float wt = sd >= 0.99999 ? 0.0\n" \
"                         : max(0.0, 1.0 - abs(ao_w(sd) - wc) / (0.04 * wc)) + 1e-4;\n" \
"                sum += at.Load(int3(q, 0)) * wt;\n" \
"                ws += wt;\n" \
"            }\n" \
"        a = ws > 0.0 ? sum / ws : 1.0;\n" \
"    }\n" \
"    float t = a;\n" \
"    if (((p3.w > 0.5 && p3.w < 1.5) || p3.w > 2.5) && p3.z < 0.5) {\n" \
"        float2 pv = st.Load(int3(p, 0));\n" \
"        if (pv.y == d) {\n" \
"            float k = 0.0;\n" \
"            if (p3.w < 1.5) {\n" \
"                float3 dc = abs(sn.Load(int3(p, 0)).rgb - cu.Load(int3(p, 0)).rgb);\n" \
"                k = saturate((max(dc.r, max(dc.g, dc.b)) - 0.02) / 0.04);\n" \
"            }\n" \
"            t = lerp(min(a, pv.x), a, k);\n" \
"            a = lerp(a < pv.x ? a / pv.x : 1.0, a, k);\n" \
"        }\n" \
"    }\n" \
"    if (p3.w > 1.5 && p3.w < 2.5) {\n" \
"        float3 dc = abs(sn.Load(int3(p, 0)).rgb - cu.Load(int3(p, 0)).rgb);\n" \
"        float k = saturate(1.0 - (max(dc.r, max(dc.g, dc.b)) - 0.06) / 0.14);\n" \
"        a = lerp(1.0, a, k);\n" \
"        t = lerp(1.0, t, k);\n" \
"    }\n" \
"    AoO o;\n" \
"    o.c = float4(a, a, a, 1.0);\n" \
"    o.s = float2(t, d);\n" \
"    return o;\n" \
"}\n"

#endif /* NV2A_SCENE_AO_SHADERS_H */
