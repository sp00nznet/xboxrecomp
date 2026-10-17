/**
 * NV2A vertex shader microcode parser (see d3d8_vsh_parse.h).
 */

#include "d3d8_vsh_parse.h"
#include "../kernel/nv2a_vsh_fields.h"
#include <string.h>

/* ================================================================
 * NV2A Instruction Fields
 *
 * Each instruction is 128 bits stored as 4 DWORDs (word[0..3]). The field
 * table lives in src/kernel/nv2a_vsh_fields.h, the one the CPU interpreter
 * (src/kernel/nv2a_vsh_cpu.c) decodes through too, so the two cannot
 * disagree. When both units are active the ILU's temp write goes to R1.
 * ================================================================ */

/* ================================================================
 * Microcode Parser
 * ================================================================ */

static void parse_source(const uint32_t *w, int which, int input_index,
                         int const_index, int rel, NV2AVshSrcOperand *src)
{
    struct nv2a_vsh_src s;

    nv2a_vsh_src_decode(w, which, &s);
    src->negate    = (int)s.neg;
    src->swizzle.x = (uint8_t)NV2A_VSH_SWZ(s.swz, 0);
    src->swizzle.y = (uint8_t)NV2A_VSH_SWZ(s.swz, 1);
    src->swizzle.z = (uint8_t)NV2A_VSH_SWZ(s.swz, 2);
    src->swizzle.w = (uint8_t)NV2A_VSH_SWZ(s.swz, 3);
    src->rel_addr = 0;
    switch (s.mux) {
    case NV2A_VSH_MUX_R:
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = (int)s.reg;
        break;
    case NV2A_VSH_MUX_V:
        src->reg_type  = NV2A_VSH_REG_INPUT;
        src->reg_index = input_index;
        break;
    case NV2A_VSH_MUX_C:
        src->reg_type  = NV2A_VSH_REG_CONST;
        src->reg_index = const_index;
        src->rel_addr  = rel;
        break;
    default:
        src->reg_type  = NV2A_VSH_REG_COUNT;   /* reads zero */
        src->reg_index = 0;
        break;
    }
}

/* An output register by its ADDRESS, or NONE for one that does not exist. */
static NV2AVshOutputReg decode_output_reg(uint32_t addr)
{
    switch (addr) {
    case 0:  return NV2A_VSH_OUT_POS;
    case 3:  return NV2A_VSH_OUT_D0;
    case 4:  return NV2A_VSH_OUT_D1;
    case 5:  return NV2A_VSH_OUT_FOG;
    case 6:  return NV2A_VSH_OUT_PTS;
    case 7:  return NV2A_VSH_OUT_B0;
    case 8:  return NV2A_VSH_OUT_B1;
    case 9:  return NV2A_VSH_OUT_T0;
    case 10: return NV2A_VSH_OUT_T1;
    case 11: return NV2A_VSH_OUT_T2;
    case 12: return NV2A_VSH_OUT_T3;
    default: return NV2A_VSH_OUT_NONE;
    }
}

static int mac_reads(NV2AVshMacOp op, int s)
{
    switch (op) {
    case NV2A_VSH_MAC_NOP: return 0;
    case NV2A_VSH_MAC_MOV:
    case NV2A_VSH_MAC_ARL: return s == 0;
    case NV2A_VSH_MAC_ADD: return s == 0 || s == 2;
    case NV2A_VSH_MAC_MAD: return 1;
    default:               return s < 2;
    }
}

void d3d8_vsh_parse(const uint32_t *microcode, int num_insns,
                     NV2AVshProgram *program)
{
    int i;
    memset(program, 0, sizeof(*program));
    program->inputs_read = 0;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    for (i = 0; i < num_insns; i++) {
        const uint32_t *w = &microcode[i * 4];
        NV2AVshInstruction *inst = &program->insns[i];
        uint32_t out_r    = NV2A_VSH_OUT_R(w);
        uint32_t mac_mask = NV2A_VSH_MAC_MASK(w);
        uint32_t ilu_mask = NV2A_VSH_ILU_MASK(w);
        uint32_t o_mask   = NV2A_VSH_O_MASK(w);
        uint32_t orb      = NV2A_VSH_ORB(w);
        uint32_t addr     = NV2A_VSH_O_ADDR(w);
        uint32_t omux     = NV2A_VSH_O_MUX(w);
        int rel           = (int)NV2A_VSH_A0X(w);
        int s;

        inst->mac_op = (NV2AVshMacOp)NV2A_VSH_MAC(w);
        inst->ilu_op = (NV2AVshIluOp)NV2A_VSH_ILU(w);
        if (inst->mac_op >= NV2A_VSH_MAC_COUNT)
            inst->mac_op = NV2A_VSH_MAC_NOP;
        inst->const_index = (int)NV2A_VSH_CONST(w);
        inst->input_index = (int)NV2A_VSH_INPUT(w);
        if (inst->const_index >= NV2A_VS_MAX_CONSTANTS)
            inst->const_index = 0;

        for (s = 0; s < 3; s++)
            parse_source(w, s, inst->input_index, inst->const_index, rel,
                         &inst->mac_src[s]);
        inst->ilu_src = inst->mac_src[2];      /* the ILU reads C */

        /* MAC: temp R (R12 = oPos) and, with MUX 0, the output register. */
        inst->mac_dst.temp_reg   = -1;
        inst->mac_dst.output_reg = NV2A_VSH_OUT_NONE;
        inst->mac_dst.write_mask = 0;
        inst->mac_dst.out_mask   = 0;
        if (inst->mac_op != NV2A_VSH_MAC_NOP && inst->mac_op != NV2A_VSH_MAC_ARL) {
            if (mac_mask && out_r <= 12) {
                inst->mac_dst.temp_reg   = (int)out_r;
                inst->mac_dst.write_mask = (uint8_t)mac_mask;
            }
            if (omux == 0 && o_mask && orb) {
                inst->mac_dst.output_reg = decode_output_reg(addr);
                inst->mac_dst.out_mask   = (uint8_t)o_mask;
            }
            /* orb == 0 writes a constant (CXT_WRITE_EN); not supported. */
        }

        /* ILU: temp R1 when the MAC is busy, else R; MUX 1 for the output. */
        inst->ilu_dst.temp_reg   = -1;
        inst->ilu_dst.output_reg = NV2A_VSH_OUT_NONE;
        inst->ilu_dst.write_mask = 0;
        inst->ilu_dst.out_mask   = 0;
        if (inst->ilu_op != NV2A_VSH_ILU_NOP) {
            uint32_t r = inst->mac_op != NV2A_VSH_MAC_NOP ? 1u : out_r;
            if (ilu_mask && r <= 12) {
                inst->ilu_dst.temp_reg   = (int)r;
                inst->ilu_dst.write_mask = (uint8_t)ilu_mask;
            }
            if (omux == 1 && o_mask && orb) {
                inst->ilu_dst.output_reg = decode_output_reg(addr);
                inst->ilu_dst.out_mask   = (uint8_t)o_mask;
            }
        }

        inst->is_final = (int)NV2A_VSH_FINAL(w);

        for (s = 0; s < 3; s++)
            if (mac_reads(inst->mac_op, s)
                    && inst->mac_src[s].reg_type == NV2A_VSH_REG_INPUT)
                program->inputs_read |= (uint16_t)(1u << inst->mac_src[s].reg_index);
        if (inst->ilu_op != NV2A_VSH_ILU_NOP
                && inst->ilu_src.reg_type == NV2A_VSH_REG_INPUT)
            program->inputs_read |= (uint16_t)(1u << inst->ilu_src.reg_index);

        program->length = i + 1;
        if (inst->is_final)
            break;
    }
}
