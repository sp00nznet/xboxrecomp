/**
 * NV2A Vertex Shader Microcode to HLSL Translator
 *
 * The Xbox NV2A GPU has a programmable vertex shader unit compatible with
 * (and extending) the original GeForce3/4 vertex shader architecture.
 * Games upload sequences of 128-bit microcode instructions via
 * D3DDevice_CreateVertexShader(). At draw time, the NV2A executes
 * these instructions in its vertex shader pipeline.
 *
 * This module translates NV2A vertex shader microcode into HLSL source
 * code, compiles it with D3DCompile, and caches the resulting
 * ID3D11VertexShader for use by the D3D8->D3D11 compatibility layer.
 *
 * NV2A Vertex Shader Architecture:
 *
 *   Registers:
 *     v0  - v15   Input vertex attribute registers (read-only)
 *     R0  - R11   Temporary registers (read/write)
 *     R12         Aliased to oPos (output position)
 *     c0  - c191  Constant registers (set by SetVertexShaderConstant)
 *     a0          Address register (integer, for indexed c[] access)
 *     oPos        Output position (= R12)
 *     oD0, oD1    Output diffuse / specular color
 *     oFog        Output fog factor
 *     oPts        Output point size
 *     oB0, oB1    Output back-face diffuse / specular
 *     oT0 - oT3   Output texture coordinates
 *
 *   Execution Units (per instruction slot, execute in parallel):
 *     MAC (Multiply-Accumulate):
 *       NOP, MOV, MUL, ADD, MAD, DP3, DP4, DPH, DST, MIN, MAX,
 *       SLT, SGE, ARL
 *     ILU (Inverse Logic Unit):
 *       NOP, MOV, RCP, RCC, RSQ, EXP, LOG, LIT
 *
 *   Programs are up to 136 instruction slots.
 *   Each slot is 128 bits (4 DWORDs) encoding both MAC and ILU ops.
 *
 * References:
 *   - envytools NV20 vertex shader documentation
 *   - xemu NV2A vertex shader implementation
 *   - Xbox SDK D3D vertex shader programming guide
 *   - US Patent 7,002,588 (Microsoft/Nvidia vertex shader architecture)
 */

#ifndef XBOXRECOMP_D3D8_VSH_H
#define XBOXRECOMP_D3D8_VSH_H

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>

#include "d3d8_vsh_parse.h"
#include "d3d8_hlsl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Shader Slot (stored microcode)
 * ================================================================ */

/**
 * A stored vertex shader program slot.
 *
 * Created by CreateVertexShader(), indexed by handle.
 * The microcode is stored as raw DWORDs; parsing and compilation
 * are deferred until the shader is first used in a draw call.
 */
typedef struct NV2AVshSlot {
    DWORD   microcode[NV2A_VS_MAX_INSTRUCTIONS * 4]; /* Raw 128-bit instructions */
    int     length;         /* Number of instructions */
    int     in_use;         /* 1 if this slot is allocated */
} NV2AVshSlot;

/* ================================================================
 * VS Constant Buffer Layout (HLSL)
 *
 * Uploaded to register(b1) so it doesn't conflict with the
 * fixed-function transform CB at b0.
 *
 * Must be 16-byte aligned and match the HLSL cbuffer declaration.
 * ================================================================ */

typedef struct NV2AVSConstants {
    float c[NV2A_VS_MAX_CONSTANTS][4];  /* 192 float4 constants */
} NV2AVSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the vertex shader translator.
 * Allocates the constant buffer and shader cache.
 * Must be called after D3D11 device creation.
 */
HRESULT d3d8_vsh_init(void);

/**
 * Shut down the vertex shader translator.
 * Releases all cached shaders, input layouts, and buffers.
 */
void d3d8_vsh_shutdown(void);

/**
 * Store a vertex shader program (CreateVertexShader).
 *
 * Copies the microcode into an internal slot. The shader is not
 * compiled until first use.
 *
 * @param microcode   Pointer to the 128-bit instruction array (4 DWORDs each)
 * @param num_insns   Number of instructions
 * @param out_handle  Receives the shader handle (>= 0x10000 to distinguish from FVF)
 * @return S_OK on success
 */
HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle);

/**
 * Delete a previously created vertex shader.
 *
 * @param handle  The shader handle from d3d8_vsh_create_shader
 * @return S_OK on success
 */
HRESULT d3d8_vsh_delete_shader(DWORD handle);

/**
 * Set a vertex shader constant register.
 *
 * @param start_reg  First register index (0-191)
 * @param data       Pointer to float4 data (4 floats per register)
 * @param count      Number of float4 registers to set
 */
void d3d8_vsh_set_constant(int start_reg, const float *data, int count);

/**
 * Check if a shader handle refers to a programmable vertex shader
 * (as opposed to an FVF code).
 *
 * On Xbox, handles > 0xFFFF are shader handles.
 */
BOOL d3d8_vsh_is_programmable(DWORD handle);

/**
 * Prepare for a draw call using a programmable vertex shader.
 *
 * - Parses microcode if not yet parsed
 * - Generates HLSL and compiles if not cached
 * - Updates the constant buffer
 * - Binds the vertex shader, input layout, and constant buffer
 *
 * @param handle  The active vertex shader handle
 * @return TRUE if a programmable VS was bound, FALSE on fallback
 */
BOOL d3d8_vsh_prepare_draw(DWORD handle);

/* ================================================================
 * Input layouts from NV2A vertex attribute formats
 * ================================================================ */

/**
 * The DXGI format that reads one NV2A attribute (type, size components)
 * as the CPU path does. Sets *expand when there is none: the caller then
 * converts that stream to float4 (R32G32B32A32_FLOAT, 16-byte stride)
 * before upload. S32K, CMP and 3-component bytes/shorts expand.
 */
DXGI_FORMAT d3d8_vsh_nv2a_attr_format(uint32_t type, uint32_t size,
                                      int *expand);

/**
 * Input elements for the attributes a program reads (`inputs_read`, from
 * NV2AVshProgram): ATTR<i> from input slot i at offset 0, so each stream
 * is bound as its own vertex buffer with its own stride. size[i] == 0
 * means the stream is not enabled; it gets R32G32B32A32_FLOAT, for a
 * stride-0 buffer holding the attribute's current SET_VERTEX_DATA value.
 * Expanded streams also get R32G32B32A32_FLOAT. Returns the count.
 */
int d3d8_vsh_nv2a_input_layout(uint16_t inputs_read,
                               const uint32_t type[NV2A_VS_MAX_INPUTS],
                               const uint32_t size[NV2A_VS_MAX_INPUTS],
                               D3D11_INPUT_ELEMENT_DESC out[NV2A_VS_MAX_INPUTS]);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VSH_H */
