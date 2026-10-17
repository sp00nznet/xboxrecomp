/* Frozen oracle for nv2a_combiner_fuzz: src/d3d/d3d8_combiners_parse.c as
 * it was before the field table, with its own decoder (before it read nv2a_combiner.h's field
 * table). The fuzz requires the current parser to fill the same state. */
#define d3d8_combiners_from_regs oracle_combiners_from_regs
/**
 * NV2A register combiner decoder (see d3d8_combiners_parse.h).
 */

#include "d3d8_combiners_parse.h"
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
 * registers take, so both paths parse the same layout. It follows
 * xemu's psh.c and the CPU evaluator, rc_eval in nv2a_pb_exec.c.
 * ================================================================ */

static void parse_combiner_input(uint32_t packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/** An input word: [31:24]=A [23:16]=B [15:8]=C [7:0]=D. */
static void parse_four_inputs(uint32_t dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[3]); /* D */
}

/** RGB output word: see NV2ACombinerOutput. */
static void parse_rgb_output(uint32_t dword, NV2ACombinerOutput *output)
{
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
    output->cd_blue_to_alpha = (dword >> 18) & 1;
    output->ab_blue_to_alpha = (dword >> 19) & 1;
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
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
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
    state->num_stages = (int)(control & 0xFF);
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->mux_msb = (control >> 8) & 1;
    state->uniq_c0 = (control >> 12) & 1;
    state->uniq_c1 = (control >> 16) & 1;

    /* Only the active stages count: the rest stay zero, so states that
     * differ only in unused stages share a shader. */
    for (i = 0; i < state->num_stages; i++) {
        parse_four_inputs(cicw[i], state->stages[i].rgb_input);
        parse_four_inputs(aicw[i], state->stages[i].alpha_input);
        parse_rgb_output(cocw[i], &state->stages[i].rgb_output);
        parse_alpha_output(aocw[i], &state->stages[i].alpha_output);
    }

    parse_combiner_input((fcw0 >> 24) & 0xFF, &state->final_input[0]); /* A */
    parse_combiner_input((fcw0 >> 16) & 0xFF, &state->final_input[1]); /* B */
    parse_combiner_input((fcw0 >>  8) & 0xFF, &state->final_input[2]); /* C */
    parse_combiner_input((fcw0 >>  0) & 0xFF, &state->final_input[3]); /* D */
    parse_combiner_input((fcw1 >> 24) & 0xFF, &state->final_input[4]); /* E */
    parse_combiner_input((fcw1 >> 16) & 0xFF, &state->final_input[5]); /* F */
    parse_combiner_input((fcw1 >>  8) & 0xFF, &state->final_input[6]); /* G */
    state->final_flags = fcw1 & 0xE0;

    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        state->tex_mode[i] = (NV2ATextureMode)((stage_modes >> (5 * i)) & 0x1F);
}
