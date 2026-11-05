/* Frozen oracle for nv2a_vsh_fuzz: src/d3d/d3d8_vsh_parse.c as it was
 * before nv2a_vsh_fields.h, with its own field decoder (before it read nv2a_vsh_fields.h). The fuzz
 * requires the current parser to produce the same NV2AVshProgram. */
#define d3d8_vsh_parse oracle_vsh_parse
/**
 * NV2A vertex shader microcode parser (see d3d8_vsh_parse.h).
 */

#include "d3d8_vsh_parse.h"
#include <string.h>

/* ================================================================
 * NV2A Instruction Fields
 *
 * Each instruction is 128 bits stored as 4 DWORDs (word[0..3]). The field
 * table is xemu's (hw/xbox/nv2a/pgraph/vsh.c), as (word, shift, bits); it is
 * the same table the CPU interpreter uses (src/kernel/nv2a_vsh_cpu.c), which
 * renders the title correctly, so the two decoders must agree. Word 0
 * carries nothing a program needs.
 *
 *   ILU 1,25,3   MAC 1,21,4   CONST 1,13,8   V 1,9,4
 *   A: NEG 1,8,1  SWZ 1,6/4/2/0,2  R 2,28,4  MUX 2,26,2
 *   B: NEG 2,25,1 SWZ 2,23/21/19/17,2  R 2,13,4  MUX 2,11,2
 *   C: NEG 2,10,1 SWZ 2,8/6/4/2,2  R_HIGH 2,0,2  R_LOW 3,30,2  MUX 3,28,2
 *   OUT: MAC_MASK 3,24,4  R 3,20,4  ILU_MASK 3,16,4  O_MASK 3,12,4
 *        ORB 3,11,1  ADDRESS 3,3,8  MUX 3,2,1
 *   A0X 3,1,1   FINAL 3,0,1
 *
 * MUX for a source: 1 = temp R, 2 = input v, 3 = constant c (0 reads zero).
 * The output MUX says which unit writes the output register (0 MAC, 1 ILU);
 * ORB says whether that is an output register (1) or a constant (0).
 * When both units are active the ILU's temp write goes to R1.
 *
 * (An earlier version of this file used a made-up bit table, MSB-first
 * across word boundaries. It compiled and decoded every title program as
 * nothing but v0 reads.)
 * ================================================================ */

static inline uint32_t vsh_fld(const uint32_t *w, int word, int shift, int bits)
{
    return ((uint32_t)w[word] >> shift) & ((1u << bits) - 1u);
}

/* ================================================================
 * Microcode Parser
 * ================================================================ */

static void parse_source(const uint32_t *w, int which, int input_index,
                         int const_index, int rel, NV2AVshSrcOperand *src)
{
    uint32_t mux, r;

    switch (which) {
    case 0:
        src->negate    = (int)vsh_fld(w, 1, 8, 1);
        src->swizzle.x = (uint8_t)vsh_fld(w, 1, 6, 2);
        src->swizzle.y = (uint8_t)vsh_fld(w, 1, 4, 2);
        src->swizzle.z = (uint8_t)vsh_fld(w, 1, 2, 2);
        src->swizzle.w = (uint8_t)vsh_fld(w, 1, 0, 2);
        r   = vsh_fld(w, 2, 28, 4);
        mux = vsh_fld(w, 2, 26, 2);
        break;
    case 1:
        src->negate    = (int)vsh_fld(w, 2, 25, 1);
        src->swizzle.x = (uint8_t)vsh_fld(w, 2, 23, 2);
        src->swizzle.y = (uint8_t)vsh_fld(w, 2, 21, 2);
        src->swizzle.z = (uint8_t)vsh_fld(w, 2, 19, 2);
        src->swizzle.w = (uint8_t)vsh_fld(w, 2, 17, 2);
        r   = vsh_fld(w, 2, 13, 4);
        mux = vsh_fld(w, 2, 11, 2);
        break;
    default:
        src->negate    = (int)vsh_fld(w, 2, 10, 1);
        src->swizzle.x = (uint8_t)vsh_fld(w, 2, 8, 2);
        src->swizzle.y = (uint8_t)vsh_fld(w, 2, 6, 2);
        src->swizzle.z = (uint8_t)vsh_fld(w, 2, 4, 2);
        src->swizzle.w = (uint8_t)vsh_fld(w, 2, 2, 2);
        r   = (vsh_fld(w, 2, 0, 2) << 2) | vsh_fld(w, 3, 30, 2);
        mux = vsh_fld(w, 3, 28, 2);
        break;
    }
    src->rel_addr = 0;
    switch (mux) {
    case 1:
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = (int)r;
        break;
    case 2:
        src->reg_type  = NV2A_VSH_REG_INPUT;
        src->reg_index = input_index;
        break;
    case 3:
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
        uint32_t out_r    = vsh_fld(w, 3, 20, 4);
        uint32_t mac_mask = vsh_fld(w, 3, 24, 4);
        uint32_t ilu_mask = vsh_fld(w, 3, 16, 4);
        uint32_t o_mask   = vsh_fld(w, 3, 12, 4);
        uint32_t orb      = vsh_fld(w, 3, 11, 1);
        uint32_t addr     = vsh_fld(w, 3, 3, 8);
        uint32_t omux     = vsh_fld(w, 3, 2, 1);
        int rel           = (int)vsh_fld(w, 3, 1, 1);
        int s;

        inst->mac_op = (NV2AVshMacOp)vsh_fld(w, 1, 21, 4);
        inst->ilu_op = (NV2AVshIluOp)vsh_fld(w, 1, 25, 3);
        if (inst->mac_op >= NV2A_VSH_MAC_COUNT)
            inst->mac_op = NV2A_VSH_MAC_NOP;
        inst->const_index = (int)vsh_fld(w, 1, 13, 8);
        inst->input_index = (int)vsh_fld(w, 1, 9, 4);
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

        inst->is_final = (int)vsh_fld(w, 3, 0, 1);

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
