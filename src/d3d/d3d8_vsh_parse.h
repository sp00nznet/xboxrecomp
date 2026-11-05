/**
 * NV2A vertex shader microcode parser: 128-bit instruction words ->
 * NV2AVshProgram. No graphics API here; the D3D11 backend (d3d8_vsh.c)
 * turns the result into HLSL, and any other backend can do the same.
 */

#ifndef XBOXRECOMP_D3D8_VSH_PARSE_H
#define XBOXRECOMP_D3D8_VSH_PARSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NV2A Vertex Shader Constants
 * ================================================================ */

/** Maximum program length in 128-bit instruction slots. */
#define NV2A_VS_MAX_INSTRUCTIONS    136

/** Number of constant registers (c0 - c191). */
#define NV2A_VS_MAX_CONSTANTS       192

/** Number of input attribute registers (v0 - v15). */
#define NV2A_VS_MAX_INPUTS          16

/** Number of temporary registers (R0 - R11, plus R12 = oPos). */
#define NV2A_VS_MAX_TEMPS           13

/** Maximum number of shader programs that can be stored. */
#define NV2A_VS_MAX_SLOTS           128

/** Shader cache size (hashed microcode -> compiled shader). */
#define NV2A_VS_CACHE_SIZE          64

/* ================================================================
 * NV2A VS Instruction Encoding (128 bits = 4 DWORDs)
 *
 * Each instruction is composed of 4 x 32-bit words:
 *
 * Word 0 (ILU/misc):
 *   [31:29] - Reserved / type
 *   [28:25] - ILU opcode (4 bits)
 *   [24:21] - MAC opcode (4 bits)
 *   [20:13] - Const index (8 bits) - constant register selector
 *   [12:9]  - Input reg (v#) (4 bits) - which v0-v15 register
 *   [8]     - Source A negate
 *   [7:4]   - Source A register type + index (mux field A)
 *   [3:0]   - Source A swizzle X component (2 bits in combined field)
 *
 * Word 1 (Source A/B):
 *   [31:24] - Source A swizzle (Y, Z, W components) + remaining bits
 *   [23:16] - Source B register select + negate + type
 *   [15:0]  - Source B swizzle + remaining fields
 *
 * Word 2 (Source C / MAC dest):
 *   [31:16] - Source C register select + negate + swizzle
 *   [15:12] - MAC dest temp register index
 *   [11:8]  - MAC dest write mask (xyzw)
 *   [7:3]   - MAC dest output register mux
 *   [2:0]   - Remaining source C bits
 *
 * Word 3 (ILU dest / final):
 *   [31:28] - ILU dest temp register index
 *   [27:24] - ILU dest write mask (xyzw)
 *   [23:19] - ILU dest output register mux
 *   [18:1]  - Reserved / other fields
 *   [0]     - Final instruction flag (1 = last instruction in program)
 *
 * NOTE: The exact bit layout follows the xemu/envytools conventions.
 * The fields below are extracted using shift-and-mask operations
 * matching the NV2A hardware encoding.
 * ================================================================ */

/* ================================================================
 * Opcode Enumerations
 * ================================================================ */

/**
 * MAC unit opcodes.
 *
 * The MAC unit handles multiply-accumulate class operations.
 * It reads up to 3 source operands (A, B, C) and writes one destination.
 */
typedef enum NV2AVshMacOp {
    NV2A_VSH_MAC_NOP = 0,   /* No operation */
    NV2A_VSH_MAC_MOV = 1,   /* dst = A */
    NV2A_VSH_MAC_MUL = 2,   /* dst = A * B */
    NV2A_VSH_MAC_ADD = 3,   /* dst = A + C */
    NV2A_VSH_MAC_MAD = 4,   /* dst = A * B + C */
    NV2A_VSH_MAC_DP3 = 5,   /* dst = dot3(A.xyz, B.xyz) */
    NV2A_VSH_MAC_DPH = 6,   /* dst = dot3(A.xyz, B.xyz) + B.w */
    NV2A_VSH_MAC_DP4 = 7,   /* dst = dot4(A, B) */
    NV2A_VSH_MAC_DST = 8,   /* dst = distance vector */
    NV2A_VSH_MAC_MIN = 9,   /* dst = min(A, B) */
    NV2A_VSH_MAC_MAX = 10,  /* dst = max(A, B) */
    NV2A_VSH_MAC_SLT = 11,  /* dst = (A < B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_SGE = 12,  /* dst = (A >= B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_ARL = 13,  /* a0.x = floor(A.x) */
    NV2A_VSH_MAC_COUNT = 14,
} NV2AVshMacOp;

/**
 * ILU unit opcodes.
 *
 * The ILU unit handles transcendental/reciprocal operations.
 * It reads source operand C and writes one destination.
 * The ILU operates in parallel with the MAC unit.
 */
typedef enum NV2AVshIluOp {
    NV2A_VSH_ILU_NOP = 0,   /* No operation */
    NV2A_VSH_ILU_MOV = 1,   /* dst = C */
    NV2A_VSH_ILU_RCP = 2,   /* dst = 1.0 / C.x (scalar, replicated) */
    NV2A_VSH_ILU_RCC = 3,   /* dst = clamp(1.0/C.x, 5.42e-36, 1.884e+19) */
    NV2A_VSH_ILU_RSQ = 4,   /* dst = 1.0 / sqrt(abs(C.x)) */
    NV2A_VSH_ILU_EXP = 5,   /* dst = exp2(C.x) */
    NV2A_VSH_ILU_LOG = 6,   /* dst = log2(abs(C.x)) */
    NV2A_VSH_ILU_LIT = 7,   /* dst = lighting helper */
    NV2A_VSH_ILU_COUNT = 8,
} NV2AVshIluOp;

/**
 * Source operand register types.
 *
 * Each source operand selects from one of three register banks.
 */
typedef enum NV2AVshRegType {
    NV2A_VSH_REG_TEMP   = 0,  /* R0-R11 (R12 = oPos alias) */
    NV2A_VSH_REG_INPUT  = 1,  /* v0-v15 */
    NV2A_VSH_REG_CONST  = 2,  /* c0-c191 (may be indexed via a0) */
    NV2A_VSH_REG_COUNT  = 3,
} NV2AVshRegType;

/**
 * Output register selectors.
 *
 * When a MAC/ILU destination targets an output register, these
 * identify which output. If the mux value is 0xFF, the write
 * goes only to a temp register.
 */
typedef enum NV2AVshOutputReg {
    NV2A_VSH_OUT_POS  = 0,   /* oPos (clip-space position) */
    NV2A_VSH_OUT_D0   = 3,   /* oD0 (diffuse color) */
    NV2A_VSH_OUT_D1   = 4,   /* oD1 (specular color) */
    NV2A_VSH_OUT_FOG  = 5,   /* oFog (fog factor) */
    NV2A_VSH_OUT_PTS  = 6,   /* oPts (point size) */
    NV2A_VSH_OUT_B0   = 7,   /* oB0 (back diffuse) */
    NV2A_VSH_OUT_B1   = 8,   /* oB1 (back specular) */
    NV2A_VSH_OUT_T0   = 9,   /* oT0 (texcoord 0) */
    NV2A_VSH_OUT_T1   = 10,  /* oT1 (texcoord 1) */
    NV2A_VSH_OUT_T2   = 11,  /* oT2 (texcoord 2) */
    NV2A_VSH_OUT_T3   = 12,  /* oT3 (texcoord 3) */
    NV2A_VSH_OUT_NONE = 0xFF, /* No output register write */
} NV2AVshOutputReg;

/* ================================================================
 * Parsed Instruction Representation
 * ================================================================ */

/**
 * Swizzle encoding for one component.
 * Each component selector picks from {x=0, y=1, z=2, w=3}.
 */
typedef struct NV2AVshSwizzle {
    uint8_t x;  /* 0=x, 1=y, 2=z, 3=w */
    uint8_t y;
    uint8_t z;
    uint8_t w;
} NV2AVshSwizzle;

/**
 * A fully decoded source operand.
 */
typedef struct NV2AVshSrcOperand {
    NV2AVshRegType  reg_type;    /* TEMP, INPUT, or CONST */
    int             reg_index;   /* Register number within the bank */
    int             negate;      /* 1 = negate the value */
    NV2AVshSwizzle  swizzle;     /* Per-component swizzle */
    int             rel_addr;    /* 1 = use a0.x relative addressing (CONST only) */
} NV2AVshSrcOperand;

/**
 * A fully decoded destination operand.
 */
typedef struct NV2AVshDstOperand {
    int               temp_reg;    /* Temp register index (0-12), or -1 if none */
    NV2AVshOutputReg  output_reg;  /* Output register, or NV2A_VSH_OUT_NONE */
    uint8_t           write_mask;  /* Temp write mask: bit3=x, bit2=y, bit1=z, bit0=w */
    uint8_t           out_mask;    /* Output register write mask, same bit order */
} NV2AVshDstOperand;

/**
 * A fully decoded NV2A vertex shader instruction.
 *
 * Each 128-bit instruction slot can encode both a MAC operation
 * and an ILU operation that execute in parallel. Either (or both)
 * may be NOP.
 */
typedef struct NV2AVshInstruction {
    /* MAC unit */
    NV2AVshMacOp    mac_op;
    NV2AVshSrcOperand mac_src[3];  /* A, B, C */
    NV2AVshDstOperand mac_dst;

    /* ILU unit */
    NV2AVshIluOp    ilu_op;
    NV2AVshSrcOperand ilu_src;     /* C (ILU only reads source C) */
    NV2AVshDstOperand ilu_dst;

    /* Constant register index (shared) */
    int             const_index;

    /* Input register index v# (shared) */
    int             input_index;

    /* Final instruction flag */
    int             is_final;
} NV2AVshInstruction;

/**
 * A complete parsed vertex shader program.
 */
typedef struct NV2AVshProgram {
    NV2AVshInstruction  insns[NV2A_VS_MAX_INSTRUCTIONS];
    int                 length;     /* Number of instructions */

    /* Bitmask of input registers read (v0-v15). Bit N = vN is used.
     * Used to determine the required input layout. */
    uint16_t            inputs_read;
} NV2AVshProgram;

/**
 * Parse NV2A vertex shader microcode into intermediate representation.
 *
 * @param microcode  Raw instruction data (4 DWORDs per instruction)
 * @param num_insns  Number of instructions
 * @param program    Output parsed program
 */
void d3d8_vsh_parse(const uint32_t *microcode, int num_insns,
                    NV2AVshProgram *program);

/** NV097_SET_VERTEX_DATA_ARRAY_FORMAT type field. */
#define NV2A_VTX_D3DCOLOR 0   /* 4 bytes, 0xAARRGGBB */
#define NV2A_VTX_S1       1   /* signed short, normalised */
#define NV2A_VTX_FLOAT    2
#define NV2A_VTX_UBYTE    4   /* unsigned byte, normalised */
#define NV2A_VTX_S32K     5   /* signed short, not normalised */
#define NV2A_VTX_CMP      6   /* 11:11:10 signed normalised, one dword */

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VSH_PARSE_H */
