/**
 * NV2A register combiner state: the decoded form of the combiner
 * registers (SET_COMBINER_*), and the decoder. No graphics API here; the
 * D3D11 backend (d3d8_combiners.c) turns the state into HLSL, and any
 * other backend can do the same.
 */

#ifndef XBOXRECOMP_D3D8_COMBINERS_PARSE_H
#define XBOXRECOMP_D3D8_COMBINERS_PARSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NV2A Register Combiner Enumerations
 * ================================================================ */

/**
 * Input register selectors.
 *
 * These identify which register in the NV2A register file is read
 * as input to a combiner stage. The register file is shared across
 * all stages; writes from stage N are visible to stage N+1.
 */
typedef enum NV2ACombinerRegister {
    NV2A_REG_ZERO       = 0,   /* Constant zero (reads 0.0) */
    NV2A_REG_C0         = 1,   /* Per-stage constant color 0 */
    NV2A_REG_C1         = 2,   /* Per-stage constant color 1 */
    NV2A_REG_FOG        = 3,   /* Fog factor (interpolated) */
    NV2A_REG_V0         = 4,   /* Primary color (diffuse) */
    NV2A_REG_V1         = 5,   /* Secondary color (specular) */
    /* 6, 7 reserved */
    NV2A_REG_T0         = 8,   /* Texture 0 sample result */
    NV2A_REG_T1         = 9,   /* Texture 1 sample result */
    NV2A_REG_T2         = 10,  /* Texture 2 sample result */
    NV2A_REG_T3         = 11,  /* Texture 3 sample result */
    NV2A_REG_R0         = 12,  /* Temporary register 0 (also SPARE0) */
    NV2A_REG_R1         = 13,  /* Temporary register 1 (also SPARE1) */
    /* Final combiner only (xemu psh.c, and the CPU path in nv2a_combiner.c): */
    NV2A_REG_V1R0_SUM   = 14,  /* V1+R0 sum (final combiner) */
    NV2A_REG_EF_PROD    = 15,  /* E*F product (final combiner) */
    NV2A_REG_COUNT       = 16,
} NV2ACombinerRegister;

/* Aliases used in documentation and comments */
#define NV2A_REG_SPARE0  NV2A_REG_R0
#define NV2A_REG_SPARE1  NV2A_REG_R1

/**
 * Input mapping modes.
 *
 * Applied to the selected register value before it enters the
 * multiply. These implement common math operations as a pre-step.
 *
 * All inputs are clamped to [0,1] after texture sampling / interpolation,
 * then these mappings produce the final value fed to the multiplier:
 *
 *   UNSIGNED_IDENTITY : x        (passthrough, range [0,1])
 *   UNSIGNED_INVERT   : 1 - x    (complement, range [0,1])
 *   EXPAND_NORMAL     : 2x - 1   (expand to [-1,1])
 *   EXPAND_NEGATE     : 1 - 2x   (expand + negate)
 *   HALFBIAS_NORMAL   : x - 0.5  (shift to [-0.5,0.5])
 *   HALFBIAS_NEGATE   : 0.5 - x  (shift + negate)
 *   SIGNED_IDENTITY   : x        (signed passthrough, allows negative)
 *   SIGNED_NEGATE     : -x       (negate)
 */
typedef enum NV2AInputMapping {
    NV2A_MAP_UNSIGNED_IDENTITY = 0,
    NV2A_MAP_UNSIGNED_INVERT   = 1,
    NV2A_MAP_EXPAND_NORMAL     = 2,
    NV2A_MAP_EXPAND_NEGATE     = 3,
    NV2A_MAP_HALFBIAS_NORMAL   = 4,
    NV2A_MAP_HALFBIAS_NEGATE   = 5,
    NV2A_MAP_SIGNED_IDENTITY   = 6,
    NV2A_MAP_SIGNED_NEGATE     = 7,
    NV2A_MAP_COUNT             = 8,
} NV2AInputMapping;

/**
 * Output scale/bias modes.
 *
 * Applied to the stage output (AB+CD or AB.CD) before writing
 * to the destination register.
 */
typedef enum NV2AOutputMapping {
    NV2A_OUT_IDENTITY           = 0,  /* x */
    NV2A_OUT_BIAS               = 1,  /* x - 0.5 */
    NV2A_OUT_SHIFTLEFT_1        = 2,  /* x * 2 */
    NV2A_OUT_SHIFTLEFT_1_BIAS   = 3,  /* (x - 0.5) * 2 */
    NV2A_OUT_SHIFTLEFT_2        = 4,  /* x * 4 */
    /* 5 is not a mapping; it is treated as identity */
    NV2A_OUT_SHIFTRIGHT_1       = 6,  /* x / 2 */
    NV2A_OUT_COUNT              = 7,
} NV2AOutputMapping;

/**
 * Texture-shader mode per stage: the hardware's SET_SHADER_STAGE_PROGRAM
 * field, which is also the XDK's PS_TEXTUREMODES value. BUMPENVMAP and
 * BUMPENVMAP_LUMINANCE are generated when the caller sets bump_on; the other
 * modes above 5 (dot products, ...) are sampled as 2D at (s, t), as the CPU
 * path does.
 *
 * A stage with no texture is not zero: xemu's psh.c gives a NONE stage
 * (0, 0, 0, 1), as GL does for an unbound sampler, and titles read its
 * alpha as a constant 1: a water combiner that weights its reflections by
 * the alpha of a stage left off draws them black at 0. CLIPPLANE reads 0.
 */
typedef enum NV2ATextureMode {
    NV2A_TEXMODE_NONE     = 0,  /* No texture: the stage reads (0, 0, 0, 1) */
    NV2A_TEXMODE_2D       = 1,  /* PROJECT2D */
    NV2A_TEXMODE_3D       = 2,  /* PROJECT3D: volume texture */
    NV2A_TEXMODE_CUBEMAP  = 3,  /* Cube map */
    NV2A_TEXMODE_PASSTHRU = 4,  /* The coordinate itself, clamped */
    NV2A_TEXMODE_CLIPPLANE = 5, /* No texture, reads zero; the coordinate is
                                 * clip distances (the CPU path kills there) */
    NV2A_TEXMODE_BUMPENVMAP = 6,     /* 2D at (s, t) + the bump matrix times
                                      * an earlier stage's (du, dv) */
    NV2A_TEXMODE_BUMPENVMAP_LUM = 7, /* the same, times its luminance */
} NV2ATextureMode;

/* ================================================================
 * Combiner Stage Input / Output Descriptors
 * ================================================================ */

/**
 * A single combiner input selection.
 *
 * Packed in hardware as 8 bits:
 *   [3:0] register   - which register to read (NV2ACombinerRegister)
 *   [4]   alpha_rep  - 1: the alpha channel. 0: RGB in an RGB portion, and
 *                      BLUE in an alpha portion.
 *   [7:5] mapping    - input mapping mode (NV2AInputMapping). In the final
 *                      combiner only bit 5 counts: invert.
 *
 * An input word holds four of them, A in the top byte: A [31:24], B [23:16],
 * C [15:8], D [7:0] (and E [31:24], F [23:16], G [15:8] in the final
 * combiner's second word, with its flags in [7:0]).
 */
typedef struct NV2ACombinerInput {
    NV2ACombinerRegister reg;       /* Source register */
    int                  alpha_rep; /* 1 = replicate alpha to RGB */
    NV2AInputMapping     mapping;   /* Input mapping function */
} NV2ACombinerInput;

/**
 * Output configuration for one channel (RGB or alpha) of a stage.
 *
 * RGB output word (NV097_SET_COMBINER_COLOR_OCW):
 *   [3:0]   cd_dst      - destination register for CD product
 *   [7:4]   ab_dst      - destination register for AB product
 *   [11:8]  sum_dst     - destination register for AB+CD sum
 *   [12]    cd_dot      - 1: CD uses dot product instead of multiply
 *   [13]    ab_dot      - 1: AB uses dot product instead of multiply
 *   [14]    mux_sum     - 1: mux instead of sum (R0.a selects AB or CD)
 *   [17:15] output_map  - output scale/bias (NV2AOutputMapping)
 *   [18]    cd_blue_to_alpha, [19] ab_blue_to_alpha
 *
 * Alpha output word (NV097_SET_COMBINER_ALPHA_OCW): the same destinations,
 * then [14] mux_sum and [17:15] output_map, as in the RGB word; bits [13:12]
 * (the dot-product flags there) are unused.
 *
 * The "dot product" flag means AB = dot3(A, B) instead of A * B
 * component-wise. This is the key to bump mapping on NV2A.
 */
typedef struct NV2ACombinerOutput {
    NV2ACombinerRegister ab_dst;     /* Where to write A*B (or 0=discard) */
    NV2ACombinerRegister cd_dst;     /* Where to write C*D (or 0=discard) */
    NV2ACombinerRegister sum_dst;    /* Where to write AB+CD (or 0=discard) */
    int                  ab_dot;     /* 1 = dot product for AB */
    int                  cd_dot;     /* 1 = dot product for CD */
    int                  mux_sum;    /* 1 = mux(R0.a, AB, CD) instead of AB+CD */
    NV2AOutputMapping    output_map; /* Scale/bias applied to results */
    int                  ab_blue_to_alpha; /* RGB only: AB's blue -> its dst alpha */
    int                  cd_blue_to_alpha;
} NV2ACombinerOutput;

/* ================================================================
 * Full Combiner State
 * ================================================================ */

/** Maximum number of general combiner stages (NV2A hardware limit). */
#define NV2A_MAX_COMBINER_STAGES 8

/** Maximum texture stages. */
#define NV2A_MAX_TEXTURES 4

/** One general combiner stage. */
typedef struct NV2ACombinerStage {
    /* Four inputs per channel: A, B, C, D */
    NV2ACombinerInput  rgb_input[4];
    NV2ACombinerInput  alpha_input[4];
    NV2ACombinerOutput rgb_output;
    NV2ACombinerOutput alpha_output;
} NV2ACombinerStage;

/**
 * Complete NV2A register combiner configuration.
 *
 * This holds everything needed to generate an equivalent HLSL pixel
 * shader. Games set this up via SetPixelShader (DWORD token) or by
 * individually setting the D3DRS_PS* render states; the pushbuffer
 * executor's path builds it from the registers (d3d8_combiners_from_regs).
 */
typedef struct NV2ACombinerState {
    /* --- General combiner stages --- */
    int num_stages;  /* Active stage count (0-8) */

    NV2ACombinerStage stages[NV2A_MAX_COMBINER_STAGES];

    /* --- Final combiner --- */
    NV2ACombinerInput final_input[7]; /* A, B, C, D, E, F, G */

    /* --- Per-stage constant colors --- */
    uint32_t c0[NV2A_MAX_COMBINER_STAGES]; /* D3DCOLOR (ARGB) per stage */
    uint32_t c1[NV2A_MAX_COMBINER_STAGES]; /* D3DCOLOR (ARGB) per stage */

    /* --- Final combiner constants --- */
    uint32_t final_c0; /* Final combiner C0 (same as stage[final].c0) */
    uint32_t final_c1; /* Final combiner C1 (same as stage[final].c1) */

    /* --- Texture modes --- */
    NV2ATextureMode tex_mode[NV2A_MAX_TEXTURES];
    /* BUMPENVMAP stages: the earlier stage whose texel is (du, dv); from
     * SET_SHADER_OTHER_STAGE_INPUT, filled by the caller (0 otherwise). */
    int tex_input[NV2A_MAX_TEXTURES];
    int bump_on;     /* the caller fills bump/bumpls and tex_input: modes 6
                      * and 7 are generated as such, else sampled as 2D */

    /* --- Flags --- */
    uint32_t flags;    /* Dot mapping and other flags from token bits 24-31 */

    /* --- Control (NV097_SET_COMBINER_CONTROL / D3DRS_PSCOMBINERCOUNT) --- */
    int uniq_c0;     /* [12]: stage s reads C0[s], else C0[0] */
    int uniq_c1;     /* [16]: likewise C1 */
    int mux_msb;     /* [8]: mux selects on R0.a's top bit, else its LSB */

    /* --- Final combiner flags (second word, [7:0]) --- */
    uint32_t final_flags; /* 0x20 invert V1, 0x40 invert R0, 0x80 clamp V1+R0 */

    /* 1: the pixel shader's input also carries the vertex fog factor (FOG,
     * after TEXCOORD3, as d3d8_vsh.c's VS_OUT has it) and the FOG register's
     * alpha is that factor (xemu psh.c: vec4(fogColor.rgb, clamp(vtxFog))).
     * 0: FOG is the constant fog colour, as before. */
    int fog_input;

    /* 1: 2D stages sample through nv2a_snap_uv (D3D8_HLSL_SNAP_UV), which
     * acts where tex_scale[n].z > 1. A backend flag, set at render scale
     * > 1 so the stock shader text is unchanged; part of the cache key. */
    int rect_snap;
} NV2ACombinerState;

/**
 * Build combiner state from the NV2A's own registers, as the pushbuffer
 * executor records them (struct nv2a_pb_vsh in nv2a_pb_state.h):
 * SET_COMBINER_COLOR_ICW/ALPHA_ICW/COLOR_OCW/ALPHA_OCW per stage,
 * SET_COMBINER_CONTROL, SET_COMBINER_SPECULAR_FOG_CW0/CW1, and the stage
 * modes packed 5 bits per stage as in SET_SHADER_STAGE_PROGRAM.
 * Constants are not part of the state (they go in the constant buffer), so
 * the state can key a shader cache.
 */
void d3d8_combiners_from_regs(const uint32_t cicw[8], const uint32_t aicw[8],
                              const uint32_t cocw[8], const uint32_t aocw[8],
                              uint32_t control, uint32_t fcw0, uint32_t fcw1,
                              uint32_t stage_modes, NV2ACombinerState *state);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_COMBINERS_PARSE_H */
