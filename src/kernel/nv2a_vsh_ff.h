/**
 * NV2A fixed-function transform and lighting, as a vertex program.
 *
 * In fixed-function mode (SET_TRANSFORM_EXECUTION_MODE 0) the transform unit
 * runs a built-in pipeline: position by the composite matrix, eye-space
 * normal and position by the model-view matrices, texgen and texture
 * matrices, up to eight lights and the fog distance. This module writes that
 * pipeline out as NV2A microcode, so every consumer of vertex programs (the
 * CPU interpreter, the MSL and HLSL translators) draws fixed-function
 * batches through the path it already has, and the backends cannot disagree
 * about what it computes.
 *
 * The semantics are xemu's (hw/xbox/nv2a/pgraph/glsl/vsh-ff.c); the
 * comments below say where a step follows it. Like the XDK epilogue, the
 * program leaves oPos in surface pixels with the clip w beside it: the
 * composite matrix already holds the viewport scale, so only the divide and
 * the viewport offset remain.
 *
 * Matrices, planes and the viewport live in the transform context, the same
 * memory as the program constants (rows 0x00..0x5F, see vsh_method). Light
 * colours and vectors do not (xemu keeps them in separate light contexts),
 * so the builder returns them as NV2A_FF_LC_ROWS extra constant rows that
 * the caller places at NV2A_FF_LC_BASE for the draw and restores after.
 *
 * Portable C, no graphics API, no executor state: tests link it alone.
 */
#ifndef XBOXRECOMP_NV2A_VSH_FF_H
#define XBOXRECOMP_NV2A_VSH_FF_H

#include <stdint.h>

#define NV2A_FF_LIGHTS 8

struct nv2a_ff_light {
    float amb[3], dif[3], spc[3];    /* SET_LIGHT_{AMBIENT,DIFFUSE,SPECULAR}_COLOR */
    float range;                     /* SET_LIGHT_LOCAL_RANGE */
    float half[3], dir[3];           /* SET_LIGHT_INFINITE_{HALF_VECTOR,DIRECTION} */
    float falloff[3], spot[4];       /* SET_LIGHT_SPOT_{FALLOFF,DIRECTION} */
    float pos[3], atten[3];          /* SET_LIGHT_LOCAL_{POSITION,ATTENUATION} */
};

/* The fixed-function registers, as the title last wrote them. */
struct nv2a_ff_regs {
    uint32_t lighting;               /* SET_LIGHTING_ENABLE */
    uint32_t skin;                   /* SET_SKIN_MODE, 0..6 */
    uint32_t normalize;              /* SET_NORMALIZATION_ENABLE */
    uint32_t light_mask;             /* SET_LIGHT_ENABLE_MASK, 2 bits a light */
    uint32_t color_material;         /* SET_COLOR_MATERIAL */
    uint32_t texgen[4][4];           /* SET_TEXGEN_S..Q per stage, raw */
    uint32_t texmat[4];              /* SET_TEXTURE_MATRIX_ENABLE */
    uint32_t texgen_view;            /* SET_TEXGEN_VIEW_MODEL: 1 infinite viewer */
    float    scene_amb[3];           /* SET_SCENE_AMBIENT_COLOR */
    float    emission[3];            /* SET_MATERIAL_EMISSION */
    float    mat_alpha;              /* SET_MATERIAL_ALPHA */
    float    spec_params[6];         /* SET_SPECULAR_PARAMS: L, M, N (front) */
    uint32_t point_size;             /* SET_POINT_SIZE, raw (pixels * 8) */
    uint32_t point_params_enable;    /* SET_POINT_PARAMS_ENABLE */
    float    point_params[8];        /* SET_POINT_PARAMS */
    struct nv2a_ff_light l[NV2A_FF_LIGHTS];
    /* Registers the executor keeps elsewhere, copied in before a build. */
    uint32_t light_ctl;              /* SET_LIGHT_CONTROL */
    uint32_t spec_enable;            /* SET_SPECULAR_ENABLE */
    uint32_t fog_enable;             /* SET_FOG_ENABLE */
    uint32_t fog_gen;                /* SET_FOG_GEN_MODE, raw */
    uint32_t written;                /* methods decoded, for the report */
};

/* Reset values: xemu's pgraph_init (specular L, M, N) and a zero mask. */
void nv2a_ff_init(struct nv2a_ff_regs *r);

/* Decodes one 3D-class method into r. Returns 1 if it was a fixed-function
 * register, 0 otherwise. */
int nv2a_ff_method(struct nv2a_ff_regs *r, uint32_t method, uint32_t param);

/* The register bits that change the program's shape (not its constants):
 * two states with equal fields build the same microcode. nv2a_ff_key_fields
 * writes them (unused words zero) for a cache to compare; nv2a_ff_key
 * hashes them. */
#define NV2A_FF_KEY_WORDS 32
void nv2a_ff_key_fields(const struct nv2a_ff_regs *r, uint32_t f[NV2A_FF_KEY_WORDS]);
uint64_t nv2a_ff_key(const struct nv2a_ff_regs *r);

/* Extra constant rows: k (0, 1, 0.5, material alpha), the scene ambient,
 * the material emission, the specular L, M, N (w 10000, the clamp), the
 * point size (x the fixed size, y and z the point-params clamp), the eight
 * point params, then NV2A_FF_LROWS rows per light. */
#define NV2A_FF_LROWS   9
#define NV2A_FF_LC_ROWS (7 + NV2A_FF_LROWS * NV2A_FF_LIGHTS)
#define NV2A_FF_LC_BASE (192 - NV2A_FF_LC_ROWS)

/* Writes the program for r into prog[0..] (at most max_slots slots, the
 * last one FINAL) and, unless lc is NULL, the light constants into lc.
 * Returns the slot count, or 0 if it did not fit. */
int nv2a_ff_build(const struct nv2a_ff_regs *r, uint32_t (*prog)[4],
                  int max_slots, float lc[NV2A_FF_LC_ROWS][4]);

/* The light constants alone: they change per draw where the program, cached
 * by nv2a_ff_key, does not. */
void nv2a_ff_consts(const struct nv2a_ff_regs *r, float lc[NV2A_FF_LC_ROWS][4]);

#endif
