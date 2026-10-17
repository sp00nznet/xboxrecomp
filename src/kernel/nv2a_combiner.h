/**
 * NV2A register combiners: the field table and the CPU evaluator.
 *
 * Every pixel the NV2A writes goes through its register combiners: up to
 * eight general stages and a final stage that mix the vertex colours, the
 * four texture results, two constants per stage and fog. D3D's fixed-function
 * texture stages compile to them too, so a title never draws without them.
 *
 * This header is the one decoder of the combiner registers. The CPU
 * evaluator below, the software rasteriser's per-batch plan, and the parser
 * the HLSL and MSL generators share (src/d3d/d3d8_combiners_parse.c) all
 * read the register words through the NV2A_RC_* macros. The encoding (ICW
 * and OCW layout, input mappings, output scale/bias, mux, final combiner)
 * follows xemu's pixel-shader generator (hw/xbox/nv2a/pgraph/glsl/psh.c).
 *
 * Register numbers (an input or output byte's low nibble):
 *   0 zero/discard  1 c0  2 c1  3 fog  4 v0  5 v1  8-11 t0-t3  12 r0  13 r1
 *   14 v1+r0 sum and 15 E*F product (final combiner only)
 *
 * The CPU evaluator, the HLSL and the MSL generators compute the same
 * thing, operation for operation, so the CPU and GPU paths agree:
 *   - in each stage all inputs are read before any output is written; the
 *     RGB writes go AB, CD, sum, then alpha AB, CD, sum, then blue-to-alpha
 *   - outputs are mapped, then clamped to [-1, 1]
 *   - the final combiner always runs: A*B + (1-A)*C + D with clamped,
 *     optionally inverted inputs, and alpha G
 * Portable C, no graphics API.
 */
#ifndef XBOXRECOMP_NV2A_COMBINER_H
#define XBOXRECOMP_NV2A_COMBINER_H

#include <stdint.h>
#include <stdio.h>

/* ---- Field table ---------------------------------------------------------
 *
 * An input word (COLOR_ICW, ALPHA_ICW, SPECULAR_FOG_CW0/1) holds four input
 * bytes, A in [31:24] down to D in [7:0] (E, F, G for CW1). An input byte is
 * reg [3:0] | alpha [4] | mapping [7:5]. In an RGB portion the alpha bit
 * replicates .a; in an alpha portion it picks .a over .b. */
#define NV2A_RC_IN(word, k)     (((uint32_t)(word) >> (24 - 8 * (k))) & 0xFFu)
#define NV2A_RC_IN_REG(b)       ((uint32_t)(b) & 0xFu)
#define NV2A_RC_IN_ALPHA(b)     (((uint32_t)(b) >> 4) & 1u)
#define NV2A_RC_IN_MAP(b)       (((uint32_t)(b) >> 5) & 7u)

/* An output word (COLOR_OCW, ALPHA_OCW; the alpha word uses the same layout
 * without the dot-product and blue-to-alpha bits). */
#define NV2A_RC_OUT_CD(o)       ((uint32_t)(o) & 0xFu)
#define NV2A_RC_OUT_AB(o)       (((uint32_t)(o) >> 4) & 0xFu)
#define NV2A_RC_OUT_SUM(o)      (((uint32_t)(o) >> 8) & 0xFu)
#define NV2A_RC_OUT_CD_DOT(o)   (((uint32_t)(o) >> 12) & 1u)
#define NV2A_RC_OUT_AB_DOT(o)   (((uint32_t)(o) >> 13) & 1u)
#define NV2A_RC_OUT_MUX(o)      (((uint32_t)(o) >> 14) & 1u)
#define NV2A_RC_OUT_MAP(o)      (((uint32_t)(o) >> 15) & 7u)
#define NV2A_RC_OUT_CD_B2A(o)   (((uint32_t)(o) >> 18) & 1u)
#define NV2A_RC_OUT_AB_B2A(o)   (((uint32_t)(o) >> 19) & 1u)

/* COMBINER_CONTROL: stage count, mux on r0.a's MSB (else LSB), and one
 * c0/c1 per stage (else stage 0's for all). */
#define NV2A_RC_CTL_STAGES(c)   ((uint32_t)(c) & 0xFFu)
#define NV2A_RC_CTL_MUX_MSB(c)  (((uint32_t)(c) >> 8) & 1u)
#define NV2A_RC_CTL_UNIQ_C0(c)  (((uint32_t)(c) >> 12) & 1u)
#define NV2A_RC_CTL_UNIQ_C1(c)  (((uint32_t)(c) >> 16) & 1u)

/* SPECULAR_FOG_CW1 [7:0] (xemu psh_regs.h): the V1+R0 sum's options. */
#define NV2A_RC_FC1_R0_INV      0x20u
#define NV2A_RC_FC1_V1_INV      0x40u
#define NV2A_RC_FC1_SUM_CLAMP   0x80u

/* A final-combiner input byte: mapping bit 0 (0x20) inverts, nothing else. */
#define NV2A_RC_FC_IN_INV(b)    (((uint32_t)(b) >> 5) & 1u)

enum {
    NV2A_RC_REG_ZERO = 0, NV2A_RC_REG_C0 = 1, NV2A_RC_REG_C1 = 2,
    NV2A_RC_REG_FOG = 3, NV2A_RC_REG_V0 = 4, NV2A_RC_REG_V1 = 5,
    NV2A_RC_REG_T0 = 8, NV2A_RC_REG_R0 = 12, NV2A_RC_REG_R1 = 13,
    NV2A_RC_REG_SUM = 14, NV2A_RC_REG_EF = 15
};

/* The combiner registers, as the pushbuffer methods set them. */
typedef struct {
    uint32_t color_icw[8], alpha_icw[8];    /* SET_COMBINER_COLOR/ALPHA_ICW */
    uint32_t color_ocw[8], alpha_ocw[8];    /* SET_COMBINER_COLOR/ALPHA_OCW */
    uint32_t factor0[8], factor1[8];        /* SET_COMBINER_FACTOR0/1 (ARGB) */
    uint32_t final0, final1;                /* SET_COMBINER_SPECULAR_FOG_CW0/1 */
    uint32_t final_c0, final_c1;            /* SET_SPECULAR_FOG_FACTOR */
    uint32_t control;                       /* SET_COMBINER_CONTROL */
    uint32_t stage_program;                 /* SET_SHADER_STAGE_PROGRAM */
} Nv2aCombiner;

/* D3DCOLOR (0xAARRGGBB) to r,g,b,a floats, k / 255.0f. */
void nv2a_rc_unpack(uint32_t argb, float out[4]);

/* One pixel. Colours are r,g,b,a in [0,1]; `fog` is (fog colour, factor).
 * t[i] is what texture stage i produced (zero for a stage that is off);
 * r0.a starts as t0.a. The result is clamped to [0,1]. */
void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4]);

/* The same on a register file the caller filled (indexed by register code;
 * everything not an input zero, r0.a set). `r` is left as the program
 * leaves it. With `trace`, each stage's words and r0/r1/t3 are printed. */
void nv2a_rc_eval_regs(const Nv2aCombiner *rc, float r[16][4], float out[4],
                       FILE *trace);

/* ---- Per-batch plan ------------------------------------------------------
 *
 * The same evaluation with the decoding hoisted out of the pixel loop:
 * built once per batch, run per pixel with results bit-identical to
 * nv2a_rc_eval_regs. Inputs that read a register no pixel can change --
 * zero, the stage's factor constants, fog when nothing writes it and its
 * factor is fixed -- are mapped once and become constants. Values no output
 * consumes are not computed. Registers are evaluated in place. */
typedef struct {
    uint8_t reg, alpha, map, cst;
    float   cv[3];                       /* mapped value when cst */
} Nv2aRcPlanIn;

typedef struct {
    Nv2aRcPlanIn rgb[4], a[4];
    uint8_t ab, cd, sum, dot_ab, dot_cd, mux, op;
    uint8_t aab, acd, asum, amux, aop, b2a_ab, b2a_cd;
    uint8_t need_ab, need_cd, need_aab, need_acd, need_sel;
} Nv2aRcPlanStage;

typedef struct {
    uint32_t        stages;
    int             mux_msb;
    Nv2aRcPlanStage st[8];
    Nv2aRcPlanIn    e, f, a, b, c, d, g;   /* final combiner; map = invert */
    int             v1_inv, r0_inv, sum_clamp;
    float           tmpl[16][4];   /* register file at pixel start */
    uint32_t        dirty;         /* registers to reset from tmpl per pixel */
    int             fog_var;       /* the fog factor (r3.a) varies per pixel */
} Nv2aRcPlan;

/* `fog` is the fog register (colour, factor); `fog_var` says the caller
 * writes a per-pixel factor into r3.a. */
void nv2a_rc_plan_build(Nv2aRcPlan *p, const Nv2aCombiner *rc,
                        const float fog[4], int fog_var);

/* Per pixel: reset the `dirty` registers from `tmpl`, fill v0, v1, the
 * textures, r0.a (and r3.a when fog_var), then run. */
void nv2a_rc_plan_eval(const Nv2aRcPlan *p, float r[16][4], float out[4]);

#endif
