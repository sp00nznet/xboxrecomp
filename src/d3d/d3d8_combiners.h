/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator
 *
 * The Xbox NV2A GPU uses "register combiners" rather than traditional
 * pixel shaders. Games configure up to 8 general combiner stages plus
 * one final combiner stage. Each general combiner stage performs
 * independent RGB and alpha math on a register file that includes
 * texture samples, interpolated vertex colors, constants, and results
 * from previous stages.
 *
 * Register combiner pipeline overview:
 *
 *   Texture fetch -> [Stage 0] -> [Stage 1] -> ... -> [Stage N-1] -> [Final Combiner] -> output
 *
 * Each general combiner stage computes:
 *   AB = map(A) * map(B)      (per-component multiply)
 *   CD = map(C) * map(D)      (per-component multiply)
 *   output = AB + CD           (or AB dot CD if dot product flag set)
 *   output = scale_bias(output) (optional output mapping)
 *
 * RGB and alpha paths are fully independent per stage - different
 * inputs, different output registers, different flags.
 *
 * The final combiner computes:
 *   result.rgb = A*B + (1-A)*C + D
 *   result.a   = G.a
 * where A,B,C,D,E,F,G each select from the register file.
 * The product E*F is available as a special register (EF_PROD).
 * The sum V1+R0 is also available (V1R0_SUM / SPARE0).
 *
 * This module translates the combiner configuration into HLSL source,
 * compiles it to a D3D11 pixel shader, and caches the result.
 *
 * References:
 *   - NV_register_combiners / NV_register_combiners2 GL extensions
 *   - Xbox SDK D3D pixel shader documentation
 *   - xemu NV2A pgraph register combiner implementation
 */

#ifndef XBOXRECOMP_D3D8_COMBINERS_H
#define XBOXRECOMP_D3D8_COMBINERS_H

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>

#include "d3d8_combiners_parse.h"
#include "d3d8_hlsl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PS Constant Buffer (HLSL layout - must be 16-byte aligned)
 *
 * This is uploaded to the GPU every draw call with current values
 * of the combiner constants, fog parameters, etc.
 * ================================================================ */

typedef struct NV2APSConstants {
    float c0[NV2A_MAX_COMBINER_STAGES][4]; /* Per-stage C0 (RGBA float) */
    float c1[NV2A_MAX_COMBINER_STAGES][4]; /* Per-stage C1 (RGBA float) */
    float final_c0[4];                      /* Final combiner C0 */
    float final_c1[4];                      /* Final combiner C1 */
    float fog_color[4];                     /* Fog color (from D3DRS_FOGCOLOR) */
    float alpha_ref;                        /* Normalized [0,1] alpha ref */
    UINT  alpha_func;                       /* D3DCMPFUNC enum value */
    UINT  alpha_test_enable;                /* 0 or 1 */
    UINT  fog_enable;                       /* 0 or 1 */
    UINT  alpha_only[NV2A_MAX_TEXTURES];     /* A8 sampling uses white RGB */
    float tex_scale[NV2A_MAX_TEXTURES][4];  /* xy: texcoord multiplier */
    UINT  tex_mode[NV2A_MAX_TEXTURES];         /* NV2ATextureMode per stage */
    float bump[NV2A_MAX_TEXTURES][4];       /* BUMPENVMAP matrix (nv2a_ps_consts) */
    float bumpls[NV2A_MAX_TEXTURES][4];     /* x: luminance scale, y: offset */
} NV2APSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the combiner translator system.
 * Allocates the PS constant buffer and sets up the shader cache.
 * Must be called after D3D11 device creation.
 */
HRESULT d3d8_combiners_init(void);

/**
 * Shut down the combiner system.
 * Releases all cached pixel shaders and the constant buffer.
 */
void d3d8_combiners_shutdown(void);

/**
 * Parse an Xbox pixel shader DWORD token into combiner state.
 *
 * The token encodes the combiner count and texture modes.
 * The actual combiner stage inputs/outputs come from the
 * D3DRS_PS* render states which must already be set.
 *
 * @param token  The DWORD passed to SetPixelShader
 * @param rs     Pointer to the render state array (from d3d8_GetRenderStates)
 * @param state  Output: filled combiner state structure
 */
void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state);

/**
 * Build combiner state from individual render states.
 *
 * Called when games set PS render states directly rather than
 * using a pixel shader token.
 *
 * @param rs     Pointer to the render state array
 * @param state  Output: filled combiner state structure
 */
void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state);

/**
 * Get or create a compiled pixel shader for the given combiner state.
 *
 * Looks up the state in the shader cache. On cache miss, generates
 * HLSL, compiles it with D3DCompile, and caches the result.
 *
 * @param state  The combiner configuration
 * @return       Compiled pixel shader, or NULL on failure.
 *               The shader is owned by the cache - do NOT release it.
 */
ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state);

/**
 * Prepare for a draw call using register combiners.
 *
 * This is the main integration point. Call this instead of (or after)
 * d3d8_shaders_prepare_draw() when a combiner pixel shader is active.
 *
 * - Rebuilds combiner state from current render states if dirty
 * - Gets or compiles the matching pixel shader
 * - Updates the PS constant buffer with current C0/C1/fog values
 * - Binds the pixel shader and constant buffer to the pipeline
 *
 * @return TRUE if a combiner shader was bound, FALSE if falling back
 *         to the fixed-function pixel shader.
 */
BOOL d3d8_combiners_prepare_draw(void);

/**
 * Notify the combiner system that a PS-related render state changed.
 * This marks the combiner state as dirty so it will be rebuilt
 * on the next prepare_draw call.
 */
void d3d8_combiners_mark_dirty(void);

/**
 * Set the current pixel shader token.
 * Pass 0 to disable combiner shaders and revert to fixed-function.
 */
void d3d8_combiners_set_pixel_shader(DWORD token);

/**
 * Get whether a combiner pixel shader is currently active.
 */
BOOL d3d8_combiners_active(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_COMBINERS_H */
