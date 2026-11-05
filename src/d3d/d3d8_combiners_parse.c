/**
 * NV2A register combiner decoder (see d3d8_combiners_parse.h).
 */

#include "d3d8_combiners_parse.h"
#include "../kernel/nv2a_combiner.h"
#include <string.h>

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is 8 bits:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel (else RGB, or blue in an alpha portion)
 *   [7:5] input mapping mode (NV2AInputMapping value)
 *
 * The XDK's D3DRS_PS* render states hold the words the hardware
 * registers take, so both paths parse the same layout: the field table
 * in src/kernel/nv2a_combiner.h, which the CPU evaluator reads too.
 * ================================================================ */

static void parse_combiner_input(uint32_t packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)NV2A_RC_IN_REG(packed);
    input->alpha_rep = (int)NV2A_RC_IN_ALPHA(packed);
    input->mapping   = (NV2AInputMapping)NV2A_RC_IN_MAP(packed);
}

/** An input word: [31:24]=A [23:16]=B [15:8]=C [7:0]=D. */
static void parse_four_inputs(uint32_t dword, NV2ACombinerInput inputs[4])
{
    int k;
    for (k = 0; k < 4; k++)
        parse_combiner_input(NV2A_RC_IN(dword, k), &inputs[k]);
}

/** RGB output word: see NV2ACombinerOutput. */
static void parse_rgb_output(uint32_t dword, NV2ACombinerOutput *output)
{
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (int)NV2A_RC_OUT_CD_DOT(dword);
    output->ab_dot     = (int)NV2A_RC_OUT_AB_DOT(dword);
    output->mux_sum    = (int)NV2A_RC_OUT_MUX(dword);
    output->output_map = (NV2AOutputMapping)NV2A_RC_OUT_MAP(dword);
    output->cd_blue_to_alpha = (int)NV2A_RC_OUT_CD_B2A(dword);
    output->ab_blue_to_alpha = (int)NV2A_RC_OUT_AB_B2A(dword);
}

/** Alpha output word: the RGB word's layout -- destinations, mux [14],
 * mapping [17:15] -- with no dot products or blue-to-alpha. The XDK builds
 * both with the one PS_COMBINEROUTPUTS macro, flags << 12, as xemu's psh.c
 * reads them. A shadow resolve seen in one title (aocw 00020800: tex0.a + tex0.a,
 * times 4) shows bit 17 is the mapping's: read as [15:13] the x4 was lost
 * and a one-layer shadow came out 1/10 as dark as on xemu. */
static void parse_alpha_output(uint32_t dword, NV2ACombinerOutput *output)
{
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->mux_sum    = (int)NV2A_RC_OUT_MUX(dword);
    output->output_map = (NV2AOutputMapping)NV2A_RC_OUT_MAP(dword);
}

/* ================================================================
 * Register Decoding
 * ================================================================ */

void d3d8_combiners_from_regs(const uint32_t cicw[8], const uint32_t aicw[8],
                              const uint32_t cocw[8], const uint32_t aocw[8],
                              uint32_t control, uint32_t fcw0, uint32_t fcw1,
                              uint32_t stage_modes, NV2ACombinerState *state)
{
    int i;

    memset(state, 0, sizeof(*state));
    state->num_stages = (int)NV2A_RC_CTL_STAGES(control);
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->mux_msb = (int)NV2A_RC_CTL_MUX_MSB(control);
    state->uniq_c0 = (int)NV2A_RC_CTL_UNIQ_C0(control);
    state->uniq_c1 = (int)NV2A_RC_CTL_UNIQ_C1(control);

    /* Only the active stages count: the rest stay zero, so states that
     * differ only in unused stages share a shader. */
    for (i = 0; i < state->num_stages; i++) {
        parse_four_inputs(cicw[i], state->stages[i].rgb_input);
        parse_four_inputs(aicw[i], state->stages[i].alpha_input);
        parse_rgb_output(cocw[i], &state->stages[i].rgb_output);
        parse_alpha_output(aocw[i], &state->stages[i].alpha_output);
    }

    parse_four_inputs(fcw0, &state->final_input[0]);          /* A B C D */
    for (i = 0; i < 3; i++)                                     /* E F G */
        parse_combiner_input(NV2A_RC_IN(fcw1, i), &state->final_input[4 + i]);
    state->final_flags = fcw1 & (NV2A_RC_FC1_R0_INV | NV2A_RC_FC1_V1_INV
                                 | NV2A_RC_FC1_SUM_CLAMP);

    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        state->tex_mode[i] = (NV2ATextureMode)((stage_modes >> (5 * i)) & 0x1F);
}
