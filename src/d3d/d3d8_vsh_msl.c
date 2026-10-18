/**
 * NV2A vertex program -> MSL (see d3d8_msl.h). Statement for statement the
 * HLSL emitter in d3d8_vsh_hlsl.c; the differences are MSL's alone: scalars
 * take no swizzle (float4(x), not x.xxxx), as_type<> for asuint, INFINITY
 * for 1.#INF, and the inputs come from a float4 buffer rather than an input
 * layout.
 */

#include "d3d8_msl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    int   pos;
    int   size;
    int   overflow;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf = buf;
    sb->pos = 0;
    sb->size = size;
    sb->overflow = 0;
    if (size > 0)
        buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining, n;
    if (sb->overflow || sb->pos >= sb->size - 1) {
        sb->overflow = 1;
        return;
    }
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->pos, (size_t)remaining, fmt, ap);
    va_end(ap);
    if (n >= 0 && n < remaining)
        sb->pos += n;
    else
        sb->overflow = 1;
}

static const char g_comp[] = "xyzw";

static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *s)
{
    if (s->x == 0 && s->y == 1 && s->z == 2 && s->w == 3)
        return;
    sb_append(sb, ".%c%c%c%c", g_comp[s->x & 3], g_comp[s->y & 3],
              g_comp[s->z & 3], g_comp[s->w & 3]);
}

static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");
    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index <= 12)
            sb_append(sb, "R%d", src->reg_index);   /* R12 is oPos */
        else
            sb_append(sb, "float4(0.0)");
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr)
            sb_append(sb, "c[clamp(a0 + %d, 0, %d)]", src->reg_index,
                      NV2A_VS_MAX_CONSTANTS - 1);
        else
            sb_append(sb, "c[%d]", src->reg_index);
        break;
    default:
        sb_append(sb, "float4(0.0)");
        break;
    }
    if (scalar)
        sb_append(sb, ".%c", g_comp[src->swizzle.x & 3]);
    else
        emit_swizzle(sb, &src->swizzle);
    if (src->negate)
        sb_append(sb, ")");
}

static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF)
        return;
    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS: return "oPos";
    case NV2A_VSH_OUT_D0:  return "oD0";
    case NV2A_VSH_OUT_D1:  return "oD1";
    case NV2A_VSH_OUT_FOG: return "oFog";
    case NV2A_VSH_OUT_PTS: return "oPts";
    case NV2A_VSH_OUT_B0:  return "oB0";
    case NV2A_VSH_OUT_B1:  return "oB1";
    case NV2A_VSH_OUT_T0:  return "oT0";
    case NV2A_VSH_OUT_T1:  return "oT1";
    case NV2A_VSH_OUT_T2:  return "oT2";
    case NV2A_VSH_OUT_T3:  return "oT3";
    default:               return NULL;
    }
}

static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst, const char *val)
{
    if (dst->temp_reg >= 0 && dst->write_mask != 0) {
        sb_append(sb, "        R%d", dst->temp_reg);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = %s", val);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
    }
    if (dst->output_reg != NV2A_VSH_OUT_NONE && dst->out_mask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            sb_append(sb, "        %s", name);
            emit_write_mask(sb, dst->out_mask);
            sb_append(sb, " = %s", val);
            emit_write_mask(sb, dst->out_mask);
            sb_append(sb, ";\n");
        }
    }
}

static int emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    const NV2AVshSrcOperand *A = &inst->mac_src[0];
    const NV2AVshSrcOperand *B = &inst->mac_src[1];
    const NV2AVshSrcOperand *C = &inst->mac_src[2];

    if (inst->mac_op == NV2A_VSH_MAC_NOP)
        return 0;
    if (inst->mac_op == NV2A_VSH_MAC_ARL) {
        sb_append(sb, "        int a0n = (int)floor(");
        emit_source(sb, A, 1);
        sb_append(sb, " + 0.001);\n");
        return 0;
    }
    sb_append(sb, "        float4 mac = ");
    switch (inst->mac_op) {
    case NV2A_VSH_MAC_MOV:
        emit_source(sb, A, 0);
        break;
    case NV2A_VSH_MAC_MUL:
        emit_source(sb, A, 0); sb_append(sb, " * "); emit_source(sb, B, 0);
        break;
    case NV2A_VSH_MAC_ADD:
        emit_source(sb, A, 0); sb_append(sb, " + "); emit_source(sb, C, 0);
        break;
    case NV2A_VSH_MAC_MAD:
        /* One expression, as the HLSL: Metal contracts a * b + c within a
         * statement to fma even with fast math off, as FXC turns it into a
         * mad, so this matches the D3D11 backend (and the hardware's MAD);
         * the CPU's a * b + c may round once more. */
        emit_source(sb, A, 0); sb_append(sb, " * "); emit_source(sb, B, 0);
        sb_append(sb, " + "); emit_source(sb, C, 0);
        break;
    case NV2A_VSH_MAC_DP3:
        sb_append(sb, "float4(dot(("); emit_source(sb, A, 0);
        sb_append(sb, ").xyz, ("); emit_source(sb, B, 0);
        sb_append(sb, ").xyz))");
        break;
    case NV2A_VSH_MAC_DPH:
        sb_append(sb, "float4(dot(("); emit_source(sb, A, 0);
        sb_append(sb, ").xyz, ("); emit_source(sb, B, 0);
        sb_append(sb, ").xyz) + ("); emit_source(sb, B, 0);
        sb_append(sb, ").w)");
        break;
    case NV2A_VSH_MAC_DP4:
        sb_append(sb, "float4(dot("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0);
        sb_append(sb, "))");
        break;
    case NV2A_VSH_MAC_DST:
        sb_append(sb, "float4(1.0, ("); emit_source(sb, A, 0);
        sb_append(sb, ").y * ("); emit_source(sb, B, 0);
        sb_append(sb, ").y, ("); emit_source(sb, A, 0);
        sb_append(sb, ").z, ("); emit_source(sb, B, 0);
        sb_append(sb, ").w)");
        break;
    case NV2A_VSH_MAC_MIN:
        sb_append(sb, "min("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0); sb_append(sb, ")");
        break;
    case NV2A_VSH_MAC_MAX:
        sb_append(sb, "max("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0); sb_append(sb, ")");
        break;
    case NV2A_VSH_MAC_SLT:
        sb_append(sb, "float4(("); emit_source(sb, A, 0);
        sb_append(sb, ") < ("); emit_source(sb, B, 0); sb_append(sb, "))");
        break;
    case NV2A_VSH_MAC_SGE:
        sb_append(sb, "float4(("); emit_source(sb, A, 0);
        sb_append(sb, ") >= ("); emit_source(sb, B, 0); sb_append(sb, "))");
        break;
    default:
        sb_append(sb, "float4(0.0)");
        break;
    }
    sb_append(sb, ";\n");
    return 1;
}

static int emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    const NV2AVshSrcOperand *C = &inst->ilu_src;

    if (inst->ilu_op == NV2A_VSH_ILU_NOP)
        return 0;
    sb_append(sb, "        float  ic = ");
    emit_source(sb, C, 1);
    sb_append(sb, ";\n");
    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_MOV:
        sb_append(sb, "        float4 ilu = ");
        emit_source(sb, C, 0);
        sb_append(sb, ";\n");
        break;
    case NV2A_VSH_ILU_RCP:
        sb_append(sb, "        float4 ilu = float4(1.0 / ic);\n");
        break;
    case NV2A_VSH_ILU_RCC:
        sb_append(sb,
            "        float  icr = fabs(1.0 / ic);\n"
            "        icr = (icr <= 1.84467e+19) ? max(icr, 5.42101e-20) : 1.84467e+19;\n"
            "        float4 ilu = float4((as_type<uint>(ic) >> 31) != 0u ? -icr : icr);\n");
        break;
    case NV2A_VSH_ILU_RSQ:
        sb_append(sb, "        float4 ilu = float4(rsqrt(fabs(ic)));\n");
        break;
    case NV2A_VSH_ILU_EXP:
        sb_append(sb,
            "        float  ief = floor(ic);\n"
            "        float4 ilu = float4(exp2(ief), ic - ief, exp2(ic), 1.0);\n");
        break;
    case NV2A_VSH_ILU_LOG:
        sb_append(sb,
            "        float  ilt = fabs(ic);\n"
            "        float  ile = floor(log2(ilt));\n"
            "        float4 ilu = (ilt == 0.0)\n"
            "            ? float4(-INFINITY, 1.0, -INFINITY, 1.0)\n"
            "            : float4(ile, ilt / exp2(ile), log2(ilt), 1.0);\n");
        break;
    case NV2A_VSH_ILU_LIT:
        sb_append(sb, "        float4 ilc = ");
        emit_source(sb, C, 0);
        sb_append(sb, ";\n"
            "        float  ilp = clamp(ilc.w, -127.9961, 127.9961);\n"
            "        float4 ilu = float4(1.0, max(ilc.x, 0.0),\n"
            "            ilc.x > 0.0 ? pow(max(ilc.y, 0.0), ilp) : 0.0, 1.0);\n");
        break;
    default:
        sb_append(sb, "        float4 ilu = float4(0.0);\n");
        break;
    }
    return 1;
}

int d3d8_vsh_generate_msl(const NV2AVshProgram *program, unsigned flags,
                          char *buf, int bufsize)
{
    StrBuf sb;
    int i, k, stride = 0;
    uint16_t inputs = program->inputs_read;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
        if (inputs & (1u << i))
            stride++;

    sb_init(&sb, buf, bufsize);
    sb_append(&sb,
        "/* Auto-generated NV2A vertex program (MSL) */\n"
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct VpConsts {\n"
        "    float4 vp_scale; float4 vp_off;\n"
        "    float4 fog_param;   /* SET_FOG_PARAMS[0], [1] */\n"
        "    uint4  fog_ctl;     /* enable, 0 linear/1 exp/2 exp2, abs, inf/NaN result */\n"
        "    uint4  spec_ctl;    /* x: specular on, y: alpha from material */\n"
        "};\n"
        D3D8_MSL_VOUT
        "vertex VOut vs_main(uint vid [[vertex_id]],\n"
        "                    const device float4 *vb [[buffer(0)]],\n"
        "                    constant float4 *c [[buffer(1)]],\n"
        "                    constant VpConsts &vp [[buffer(2)]]) {\n");
    for (i = 0; i <= 12; i++)
        sb_append(&sb, "    float4 R%d = float4(0.0);\n", i);
    sb_append(&sb, "    int a0 = 0;\n");
    for (i = 0, k = 0; i < NV2A_VS_MAX_INPUTS; i++)
        if (inputs & (1u << i))
            sb_append(&sb, "    float4 v%d = vb[vid * %du + %du];\n", i, stride, k++);
    sb_append(&sb,
        "    /* Output registers, (0,0,0,1) until written, as on hardware */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,1);\n"
        "    float4 oPts = float4(0,0,0,1);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "    (void)a0; (void)R0; (void)oB0; (void)oB1; (void)oPts;\n"
        "#define R12 oPos\n");
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n", program->length);
    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];
        int m, u;
        sb_append(&sb, "    { /* Instruction %d */\n", i);
        m = emit_mac_op(&sb, inst);
        u = emit_ilu_op(&sb, inst);
        if (m)
            emit_dest_assign(&sb, &inst->mac_dst, "mac");
        if (u)
            emit_dest_assign(&sb, &inst->ilu_dst, "ilu");
        if (inst->mac_op == NV2A_VSH_MAC_ARL)
            sb_append(&sb, "        a0 = a0n;\n");
        sb_append(&sb, "    }\n");
    }
    sb_append(&sb, "#undef R12\n    VOut o;\n");
    if (flags & D3D8_MSL_SCREEN_SPACE) {
        /* As the HLSL: onto the NV2A's 1/16-pixel grid (nv2a_screen_snap),
         * pixels -> NDC, z -> [0,1], times w again so the rasteriser's
         * divide gives perspective-correct interpolation. */
        sb_append(&sb,
            "    oPos.xy = trunc(oPos.xy * 16.0) / 16.0;\n"
            "    float  pw = (oPos.w == 0.0) ? 1.0 : oPos.w;\n"
            "    o.pos = float4((oPos.xy * vp.vp_scale.xy + vp.vp_off.xy) * pw,\n"
            "                   oPos.z * vp.vp_scale.z * pw, pw);\n"
            "    {\n"
            "        float fd = oFog.x, ff = 1.0;\n"
            "        if (vp.fog_ctl.x != 0u) {\n"
            "            if (vp.fog_ctl.y == 0u)      ff = vp.fog_param.x + fd * vp.fog_param.y - 1.0;\n"
            "            else if (vp.fog_ctl.y == 1u) ff = vp.fog_param.x + exp2(fd * vp.fog_param.y * 16.0) - 1.5;\n"
            "            else ff = vp.fog_param.x + exp2(-fd * fd * vp.fog_param.y * vp.fog_param.y * 32.0) - 1.5;\n"
            "            if (vp.fog_ctl.z != 0u) ff = fabs(ff);\n"
            "            if ((as_type<uint>(fd) & 0x7FFFFFFFu) == 0x7F800000u\n"
            "                    || (as_type<uint>(ff) & 0x7FFFFFFFu) > 0x7F800000u)\n"
            "                ff = vp.fog_ctl.w != 0u ? 1.0 : 0.0;\n"
            "        }\n"
            "        oFog = float4(ff);\n"
            "        if (vp.spec_ctl.x == 0u) oD1 = float4(0, 0, 0, 1);\n"
            "        else if (vp.spec_ctl.y == 0u) oD1.w = 1.0;\n"
            "    }\n");
    } else {
        sb_append(&sb, "    (void)vp;\n    o.pos = oPos;\n");
    }
    sb_append(&sb,
        "    o.d0 = saturate(oD0);\n"
        "    o.d1 = saturate(oD1);\n"
        "    o.t0 = oT0; o.t1 = oT1; o.t2 = oT2; o.t3 = oT3;\n"
        "    o.fog = oFog.x;\n"
        "    o.psize = oPts.x;\n"
        "    return o;\n"
        "}\n");
    return sb.overflow ? -1 : sb.pos;
}
