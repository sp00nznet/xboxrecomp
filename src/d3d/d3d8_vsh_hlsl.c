/**
 * NV2A vertex program -> HLSL (see d3d8_hlsl.h).
 */

#include "d3d8_hlsl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ================================================================
 * HLSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    if (size > 0) buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    if (sb->pos >= sb->size - 1) return;
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, remaining, fmt, ap);
    va_end(ap);
    if (n > 0 && n < remaining)
        sb->pos += n;
    else if (n >= remaining)
        sb->pos = sb->size - 1;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index <= 12)
            sb_append(sb, "R%d", src->reg_index);   /* R12 is oPos */
        else
            sb_append(sb, "float4(0,0,0,0)");
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
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an HLSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit the writes of one unit's result, `val` ("mac" or "ilu").
 *
 * The NV2A can write a temp register and an output register from the same
 * operation, each with its own mask.
 */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst,
                              const char *val)
{
    if (dst->temp_reg >= 0 && dst->write_mask != 0) {
        sb_append(sb, "        R%d", dst->temp_reg);   /* R12 is oPos */
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

/**
 * Emit the MAC unit's result as `float4 mac = ...;` (or `a0n` for ARL).
 * Returns 1 if there is a value to write back.
 */
static int emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    const NV2AVshSrcOperand *A = &inst->mac_src[0];
    const NV2AVshSrcOperand *B = &inst->mac_src[1];
    const NV2AVshSrcOperand *C = &inst->mac_src[2];

    if (inst->mac_op == NV2A_VSH_MAC_NOP)
        return 0;
    if (inst->mac_op == NV2A_VSH_MAC_ARL) {
        /* Applied after every read of this instruction, as on hardware. */
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
        emit_source(sb, A, 0); sb_append(sb, " * "); emit_source(sb, B, 0);
        sb_append(sb, " + "); emit_source(sb, C, 0);
        break;
    case NV2A_VSH_MAC_DP3:
        sb_append(sb, "dot(("); emit_source(sb, A, 0);
        sb_append(sb, ").xyz, ("); emit_source(sb, B, 0);
        sb_append(sb, ").xyz).xxxx");
        break;
    case NV2A_VSH_MAC_DPH:
        sb_append(sb, "(dot(("); emit_source(sb, A, 0);
        sb_append(sb, ").xyz, ("); emit_source(sb, B, 0);
        sb_append(sb, ").xyz) + ("); emit_source(sb, B, 0);
        sb_append(sb, ").w).xxxx");
        break;
    case NV2A_VSH_MAC_DP4:
        sb_append(sb, "dot("); emit_source(sb, A, 0);
        sb_append(sb, ", "); emit_source(sb, B, 0);
        sb_append(sb, ").xxxx");
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
        sb_append(sb, "(float4)("); emit_source(sb, A, 0);
        sb_append(sb, " < "); emit_source(sb, B, 0); sb_append(sb, ")");
        break;
    case NV2A_VSH_MAC_SGE:
        sb_append(sb, "(float4)("); emit_source(sb, A, 0);
        sb_append(sb, " >= "); emit_source(sb, B, 0); sb_append(sb, ")");
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }
    sb_append(sb, ";\n");
    return 1;
}

/**
 * Emit the ILU unit's result as `float4 ilu = ...;`. Scalar ops read C.x
 * after the swizzle. Semantics follow nv2a_vsh_cpu.c.
 */
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
        sb_append(sb, "        float4 ilu = (1.0 / ic).xxxx;\n");
        break;
    case NV2A_VSH_ILU_RCC:
        /* 1/x with its magnitude clamped to [5.42101e-20, 1.84467e+19]. */
        sb_append(sb,
            "        float  icr = abs(1.0 / ic);\n"
            "        icr = (icr <= 1.84467e+19) ? max(icr, 5.42101e-20) : 1.84467e+19;\n"
            "        float4 ilu = (asuint(ic) >> 31 ? -icr : icr).xxxx;\n");
        break;
    case NV2A_VSH_ILU_RSQ:
        sb_append(sb, "        float4 ilu = rsqrt(abs(ic)).xxxx;\n");
        break;
    case NV2A_VSH_ILU_EXP:
        sb_append(sb,
            "        float  ief = floor(ic);\n"
            "        float4 ilu = float4(exp2(ief), ic - ief, exp2(ic), 1.0);\n");
        break;
    case NV2A_VSH_ILU_LOG:
        sb_append(sb,
            "        float  ilt = abs(ic);\n"
            "        float  ile = floor(log2(ilt));\n"
            "        float4 ilu = (ilt == 0.0)\n"
            "            ? float4(-1.#INF, 1.0, -1.#INF, 1.0)\n"
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
        sb_append(sb, "        float4 ilu = float4(0,0,0,0);\n");
        break;
    }
    return 1;
}

int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize)
{
    return d3d8_vsh_generate_hlsl_ex(program, 0, buf, bufsize);
}

int d3d8_vsh_generate_hlsl_ex(const NV2AVshProgram *program, unsigned flags,
                               char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    uint16_t inputs = program->inputs_read;

    sb_init(&sb, buf, bufsize);

    /* Constant buffer: 192 float4 constants */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "cbuffer VSH_Constants : register(b1) {\n"
        "    float4 c[%d];\n"
        "};\n"
        "\n", NV2A_VS_MAX_CONSTANTS);
    if (flags & NV2A_VSH_HLSL_SCREEN_SPACE)
        sb_append(&sb,
            "/* Surface pixels -> NDC: x * vp_scale.x + vp_off.x, likewise y;\n"
            " * z * vp_scale.z. See NV2A_VSH_HLSL_SCREEN_SPACE. */\n"
            "cbuffer VSH_Viewport : register(b2) {\n"
            "    float4 vp_scale;\n"
            "    float4 vp_off;\n"
            "    float4 fog_param;   /* SET_FOG_PARAMS[0], [1] */\n"
            "    uint4  fog_ctl;     /* enable, 0 linear/1 exp/2 exp2, abs, inf/NaN result */\n"
            "    uint4  spec_ctl;    /* x: specular on, y: alpha from material */\n"
            "};\n"
            "\n");

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d : ATTR%d;\n", i, i);
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : FOG;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD4;\n"
        "    float4 oB1  : TEXCOORD5;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i <= 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Alias input registers for readability */
    sb_append(&sb, "    /* Input register aliases */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d = input.v%d;\n", i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables */
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
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];
        int m, u;

        /* MAC and ILU issue together: both read the registers as they were
         * before the instruction, then both write. */
        sb_append(&sb, "\n    { /* Instruction %d */\n", i);
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

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Populate output structure */
    sb_append(&sb, "    /* Write outputs */\n    VS_OUT o;\n");
    if (flags & NV2A_VSH_HLSL_SCREEN_SPACE) {
        /* The program leaves oPos in surface pixels, already divided by w,
         * with the clip w in oPos.w (the XDK epilogue: mul by c[58], rcc
         * of w, mad with c[59]). NV2A has no viewport stage after the
         * program; D3D11 does. Undo it: pixels -> NDC, z -> [0,1], then
         * multiply back by w so the rasteriser divides it out again and
         * interpolates perspective-correctly. First the NV2A rasteriser's
         * 1/16-pixel truncation (nv2a_screen_snap): without it the XDK's
         * 0.53125 offset moves full-screen passes one pixel. */
        sb_append(&sb,
            "    oPos.xy = trunc(oPos.xy * 16.0) / 16.0;\n"
            "    float  pw = (oPos.w == 0.0) ? 1.0 : oPos.w;\n"
            "    o.oPos = float4((oPos.xy * vp_scale.xy + vp_off.xy) * pw,\n"
            "                    oPos.z * vp_scale.z * pw, pw);\n");
    } else {
        sb_append(&sb, "    o.oPos = oPos;\n");
    }
    if (flags & NV2A_VSH_HLSL_SCREEN_SPACE) {
        /* The NV2A pushbuffer path: what xemu's vsh.c appends to every
         * program. oFog.x is the fog distance and leaves as the fog factor;
         * specular off forces oD1 to (0,0,0,1), and its alpha is 1 unless
         * SET_LIGHT_CONTROL takes it from the material. */
        sb_append(&sb,
            "    {\n"
            "        float fd = oFog.x, ff = 1.0;\n"
            "        if (fog_ctl.x != 0u) {\n"
            "            if (fog_ctl.y == 0u)      ff = fog_param.x + fd * fog_param.y - 1.0;\n"
            "            else if (fog_ctl.y == 1u) ff = fog_param.x + exp2(fd * fog_param.y * 16.0) - 1.5;\n"
            "            else ff = fog_param.x + exp2(-fd * fd * fog_param.y * fog_param.y * 32.0) - 1.5;\n"
            "            if (fog_ctl.z != 0u) ff = abs(ff);\n"
            /* isinf/isnan by bits: vkd3d's compiler has neither, and
             * fxc may fold an x != x test away. */
            "            if ((asuint(fd) & 0x7FFFFFFFu) == 0x7F800000u\n"
            "                    || (asuint(ff) & 0x7FFFFFFFu) > 0x7F800000u)\n"
            "                ff = fog_ctl.w != 0u ? 1.0 : 0.0;\n"
            "        }\n"
            "        oFog = float4(ff, ff, ff, ff);\n"
            "        if (spec_ctl.x == 0u) oD1 = float4(0, 0, 0, 1);\n"
            "        else if (spec_ctl.y == 0u) oD1.w = 1.0;\n"
            "    }\n");
    }
    sb_append(&sb,

        "    o.oD0  = saturate(oD0);\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(oD1);\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n"
        "    o.oFog = oFog.x;\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(oB0);\n"
        "    o.oB1  = saturate(oB1);\n"
        "    return o;\n"
        "}\n");

    return sb.pos;
}
