/**
 * NV2A register combiners -> HLSL pixel shader (see d3d8_hlsl.h).
 */

#include "d3d8_hlsl.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * HLSL Code Generation
 *
 * The shader mirrors nv2a_rc_eval_regs (src/kernel/nv2a_combiner.c), so
 * the GPU and CPU paths agree:
 *   - registers x0..x15 by register code: 0 zero, 1 C0, 2 C1, 3 fog,
 *     4 V0, 5 V1, 8-11 T0-T3, 12 R0 (alpha starts as T0's), 13 R1,
 *     14 V1+R0 and 15 E*F (final combiner)
 *   - in each stage all inputs are read before any output is written;
 *     the RGB writes go AB, CD, sum, then alpha AB, CD, sum, then the
 *     blue-to-alpha copies
 *   - outputs are mapped, then clamped to [-1, 1]
 *   - the final combiner computes A*B + (1-A)*C + D with clamped,
 *     optionally inverted inputs, and alpha G
 *   - the alpha test compares 8-bit values
 * ================================================================ */

#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/* An input as float3 (RGB portion) or float (alpha portion), before
 * mapping. */
static void input_src(const NV2ACombinerInput *in, int alpha_portion,
                      char *out, size_t n)
{
    int reg = (int)in->reg & 0xF;
    if (alpha_portion)
        snprintf(out, n, "x%d.%s", reg, in->alpha_rep ? "a" : "b");
    else
        snprintf(out, n, "x%d.%s", reg, in->alpha_rep ? "aaa" : "rgb");
}

static const char s_rc_helpers[] =
    "float rc_map(float x, int m) {\n"
    "    if (m == 0) return max(x, 0.0);\n"
    "    if (m == 1) return 1.0 - saturate(x);\n"
    "    if (m == 2) return 2.0 * max(x, 0.0) - 1.0;\n"
    "    if (m == 3) return 1.0 - 2.0 * max(x, 0.0);\n"
    "    if (m == 4) return max(x, 0.0) - 0.5;\n"
    "    if (m == 5) return 0.5 - max(x, 0.0);\n"
    "    if (m == 6) return x;\n"
    "    return -x;\n"
    "}\n"
    "float3 rc_map3(float3 x, int m) {\n"
    "    return float3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
    "}\n"
    "float rc_op(float x, int op) {\n"
    "    if (op == 1) x -= 0.5;\n"
    "    else if (op == 2) x *= 2.0;\n"
    "    else if (op == 3) x = (x - 0.5) * 2.0;\n"
    "    else if (op == 4) x *= 4.0;\n"
    "    else if (op == 6) x *= 0.5;\n"
    "    return clamp(x, -1.0, 1.0);\n"
    "}\n"
    "float3 rc_op3(float3 x, int op) {\n"
    "    return float3(rc_op(x.r, op), rc_op(x.g, op), rc_op(x.b, op));\n"
    "}\n";

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    static const char *const in_name[4] = { "A", "B", "C", "D" };
    char src[32];
    int off = 0;
    int i, s, k;

    /* ---- Textures ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE || m == NV2A_TEXMODE_PASSTHRU
                || m == NV2A_TEXMODE_CLIPPLANE)
            continue;
        EMIT("%s tex%d : register(t%d);\n",
             m == NV2A_TEXMODE_3D ? "Texture3D" :
             m == NV2A_TEXMODE_CUBEMAP ? "TextureCube" : "Texture2D", i, i);
        EMIT("SamplerState samp%d : register(s%d);\n", i, i);
    }

    /* ---- Constants: NV2APSConstants ---- */
    EMIT("cbuffer CombinerCB : register(b0) {\n"
         "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n"
         "    float alpha_ref; uint alpha_func; uint alpha_test_enable; uint fog_enable;\n"
         "    uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n"
         "    float4 bump[4]; float4 bumpls[4]; uint4 clip_mode;\n"
         "};\n");

    /* ---- Input: a prefix of d3d8_vsh.c's VS_OUT ---- */
    EMIT("struct PS_IN {\n"
         "    float4 pos : SV_POSITION;\n"
         "    float4 d0  : COLOR0;\n"
         "    float4 d1  : COLOR1;\n"
         "    float4 tc0 : TEXCOORD0;\n"
         "    float4 tc1 : TEXCOORD1;\n"
         "    float4 tc2 : TEXCOORD2;\n"
         "    float4 tc3 : TEXCOORD3;\n");
    if (state->fog_input)
        EMIT("    float  fog : FOG;\n");
    EMIT("};\n");
    EMIT("%s", s_rc_helpers);
    if (state->rect_snap)
        EMIT("%s", D3D8_HLSL_SNAP_UV);
    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        if ((int)state->tex_mode[i] == NV2A_TEXMODE_CLIPPLANE) {
            EMIT("%s", D3D8_HLSL_CLIP_KILLS);
            break;
        }

    EMIT("float4 main(PS_IN i) : SV_Target {\n");
    for (k = 0; k < 16; k++)
        EMIT("    float4 x%d = 0;\n", k);
    if (state->fog_input)
        EMIT("    x3 = float4(fog_color.rgb, saturate(i.fog));\n");
    else
        EMIT("    x3 = fog_color;\n");
    EMIT("    x4 = i.d0;\n");
    EMIT("    x5 = i.d1;\n");

    /* ---- Texture stages ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE) {
            /* (0, 0, 0, 1), as xemu: see NV2ATextureMode */
            EMIT("    x%d = float4(0.0, 0.0, 0.0, 1.0);\n", 8 + i);
            continue;
        }
        if (m == NV2A_TEXMODE_CLIPPLANE)
            continue;
        if (m == NV2A_TEXMODE_PASSTHRU) {
            EMIT("    x%d = saturate(i.tc%d);\n", 8 + i, i);
            continue;
        }
        if (m == NV2A_TEXMODE_3D || m == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    x%d = tex%d.Sample(samp%d, i.tc%d.xyz);\n", 8 + i, i, i, i);
        } else if (state->bump_on && (m == NV2A_TEXMODE_BUMPENVMAP
                                      || m == NV2A_TEXMODE_BUMPENVMAP_LUM)) {
            /* The input stage's blue and green are du and dv as signed
             * bytes: textures arrive as unsigned BGRA8, so the byte is taken
             * back to two's complement and over 127, as xemu's sign3. The
             * matrix is bump[] as the slots hold it, 00 01 11 10
             * (nv2a_ps_consts.bump). The offset is added in the
             * coordinate's own units, before tex_scale (texels for a
             * linear-format texture). */
            int in = state->tex_input[i];
            EMIT("    {\n");
            EMIT("        float2 ds = round(x%d.bg * 255.0);\n", 8 + in);
            EMIT("        ds = (ds >= 128.0 ? ds - 256.0 : ds) / 127.0;\n");
            EMIT("        ds = float2(dot(ds, bump[%d].xw), dot(ds, bump[%d].yz));\n", i, i);
            EMIT("        x%d = tex%d.Sample(samp%d, (i.tc%d.xy + ds) * tex_scale[%d].xy);\n",
                 8 + i, i, i, i, i);
            if (m == NV2A_TEXMODE_BUMPENVMAP_LUM)
                /* luminance: the input's unsigned red; it scales the whole
                 * texel, alpha too, as xemu's t = t * lum */
                EMIT("        x%d *= x%d.r * bumpls[%d].x + bumpls[%d].y;\n",
                     8 + i, 8 + in, i, i);
            EMIT("    }\n");
        } else {
            EMIT("    {\n");
            EMIT("        float2 uv = i.tc%d.xy;\n", i);
            if (m == NV2A_TEXMODE_2D)
                /* tex_mode (constant buffer) is the stage's real mode: a
                 * caller that samples other modes as 2D passes them as
                 * NV2A_TEXMODE_2D here, and only true 2D stages divide by q,
                 * as the CPU path's stage_sample does. */
                EMIT("        if (tex_mode[%d] == 1u && i.tc%d.w != 0.0 && i.tc%d.w != 1.0)"
                     " uv /= i.tc%d.w;\n", i, i, i, i);
            if (state->rect_snap)
                EMIT("        uv = nv2a_snap_uv(uv, i.pos.xy, tex_scale[%d]);\n", i);
            EMIT("        x%d = tex%d.Sample(samp%d, uv * tex_scale[%d].xy);\n",
                 8 + i, i, i, i);
            EMIT("    }\n");
        }
        EMIT("    if (alpha_only[%d] == 2u) x%d.a = 1.0;\n"
             "    else if (alpha_only[%d] != 0u) x%d.rgb = 1.0;\n", i, 8 + i, i, 8 + i);
    }
    EMIT("    x12.a = x8.a;\n");

    /* ---- General combiner stages ---- */
    for (s = 0; s < state->num_stages; s++) {
        const NV2ACombinerStage *st = &state->stages[s];
        const NV2ACombinerOutput *ro = &st->rgb_output, *ao = &st->alpha_output;

        EMIT("    {   /* stage %d */\n", s);
        EMIT("        x1 = c0[%d]; x2 = c1[%d];\n",
             state->uniq_c0 ? s : 0, state->uniq_c1 ? s : 0);
        if (ro->mux_sum || ao->mux_sum) {
            if (state->mux_msb)
                EMIT("        bool sel = x12.a >= 0.5;\n");
            else
                EMIT("        bool sel = (((uint)(saturate(x12.a) * 255.0 + 0.5)) & 1u) != 0u;\n");
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->rgb_input[k], 0, src, sizeof src);
            EMIT("        float3 %s = rc_map3(%s, %d);\n",
                 in_name[k], src, (int)st->rgb_input[k].mapping);
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->alpha_input[k], 1, src, sizeof src);
            EMIT("        float a%s = rc_map(%s, %d);\n",
                 in_name[k], src, (int)st->alpha_input[k].mapping);
        }
        EMIT("        float3 ab = %s;\n", ro->ab_dot ? "(float3)dot(A, B)" : "A * B");
        EMIT("        float3 cd = %s;\n", ro->cd_dot ? "(float3)dot(C, D)" : "C * D");
        EMIT("        float3 sum = %s;\n", ro->mux_sum ? "sel ? cd : ab" : "ab + cd");
        EMIT("        ab = rc_op3(ab, %d); cd = rc_op3(cd, %d); sum = rc_op3(sum, %d);\n",
             (int)ro->output_map, (int)ro->output_map, (int)ro->output_map);
        EMIT("        float aab = aA * aB, acd = aC * aD;\n");
        EMIT("        float asum = %s;\n", ao->mux_sum ? "sel ? acd : aab" : "aab + acd");
        if (ro->ab_dst)  EMIT("        x%d.rgb = ab;\n",  (int)ro->ab_dst);
        if (ro->cd_dst)  EMIT("        x%d.rgb = cd;\n",  (int)ro->cd_dst);
        if (ro->sum_dst) EMIT("        x%d.rgb = sum;\n", (int)ro->sum_dst);
        if (ao->ab_dst)  EMIT("        x%d.a = rc_op(aab, %d);\n",  (int)ao->ab_dst,  (int)ao->output_map);
        if (ao->cd_dst)  EMIT("        x%d.a = rc_op(acd, %d);\n",  (int)ao->cd_dst,  (int)ao->output_map);
        if (ao->sum_dst) EMIT("        x%d.a = rc_op(asum, %d);\n", (int)ao->sum_dst, (int)ao->output_map);
        if (ro->ab_blue_to_alpha && ro->ab_dst) EMIT("        x%d.a = ab.b;\n", (int)ro->ab_dst);
        if (ro->cd_blue_to_alpha && ro->cd_dst) EMIT("        x%d.a = cd.b;\n", (int)ro->cd_dst);
        EMIT("    }\n");
    }

    /* ---- Final combiner ---- */
    EMIT("    float4 res;\n");
    EMIT("    {   /* final combiner */\n");
    EMIT("        x1 = fc0; x2 = fc1;\n");
    {
        /* E and F first, then the special registers, then A..D and G,
         * which can read them. */
        static const char *const fname[7] = { "A", "B", "C", "D", "E", "F", "G" };
        static const int order[7] = { 4, 5, -1, 0, 1, 2, 3 };
        for (i = 0; i < 7; i++) {
            const NV2ACombinerInput *in;
            k = order[i];
            if (k < 0) {
                /* COMPLEMENT_V1 0x40, COMPLEMENT_R0 0x20 (xemu psh_regs.h). */
                EMIT("        float3 fv1 = %ssaturate(x5.rgb);\n",
                     (state->final_flags & 0x40) ? "1.0 - " : "");
                EMIT("        float3 fr0 = %ssaturate(x12.rgb);\n",
                     (state->final_flags & 0x20) ? "1.0 - " : "");
                EMIT("        x14 = float4(%s(fv1 + fr0), 0.0);\n",
                     (state->final_flags & 0x80) ? "saturate" : "");
                EMIT("        x15 = float4(E * F, 0.0);\n");
                continue;
            }
            in = &state->final_input[k];
            input_src(in, 0, src, sizeof src);
            EMIT("        float3 %s = %ssaturate(%s);\n", fname[k],
                 ((int)in->mapping & 1) ? "1.0 - " : "", src);
        }
        input_src(&state->final_input[6], 1, src, sizeof src);
        EMIT("        float G = %ssaturate(%s);\n",
             ((int)state->final_input[6].mapping & 1) ? "1.0 - " : "", src);
    }
    EMIT("        res = float4(saturate(A * B + (1.0 - A) * C + D), G);\n");
    EMIT("    }\n");

    /* ---- CLIPPLANE stages: the coordinate's four plane distances ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        if ((int)state->tex_mode[i] == NV2A_TEXMODE_CLIPPLANE)
            EMIT("    if (nv2a_clip_kills(clip_mode[%d], i.tc%d)) discard;\n", i, i);

    /* ---- Alpha test: D3DCMP numbering on 8-bit values ---- */
    EMIT("    if (alpha_test_enable != 0u) {\n"
         "        uint a = (uint)(saturate(res.a) * 255.0 + 0.5), r = (uint)(alpha_ref * 255.0 + 0.5);\n"
         "        bool ok = true;\n"
         "        if      (alpha_func == 1u) ok = false;\n"
         "        else if (alpha_func == 2u) ok = a <  r;\n"
         "        else if (alpha_func == 3u) ok = a == r;\n"
         "        else if (alpha_func == 4u) ok = a <= r;\n"
         "        else if (alpha_func == 5u) ok = a >  r;\n"
         "        else if (alpha_func == 6u) ok = a != r;\n"
         "        else if (alpha_func == 7u) ok = a >= r;\n"
         "        if (!ok) discard;\n"
         "    }\n");
    EMIT("    return res;\n");
    EMIT("}\n");
    return off;
}

#undef EMIT
