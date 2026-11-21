/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * The exception is a back end that asks for them with
 * nv2a_backend_set_program_batches(1): it receives each
 * vertex-program batch before transform instead (Nv2aBatch.program), and runs
 * the program on the GPU itself (nv2a_vsh_emit_hlsl). Fixed-function and
 * pre-transformed batches still arrive as surface pixels.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    /* Swizzled (SURFACE_FORMAT type 2, render-to-texture): pixel (x, y) is
     * at the Morton offset of a swz_width x swz_height texture (powers of
     * two), not at row y; pitch does not apply. */
    uint32_t swizzled;
    uint32_t swz_width, swz_height;
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    uint32_t palette;           /* P8 (format 0x0B): guest address of the CLUT */
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SET_SURFACE_FORMAT's zeta format: 1 Z16, 2 Z24S8. Window z is in that
     * format's units (0..65535 or 0..16777215), as is the clear value's depth
     * (zstencil & 0xFFFF, or zstencil >> 8). */
    uint32_t zeta_format;
} Nv2aRenderState;

/* A vertex-program batch before transform, for a back end that runs the
 * program itself (nv2a_backend_set_program_batches). The executor sends one only
 * while _CXT_WRITE_EN is 0 (a program that writes c[] runs on the CPU) and for
 * the triangle topologies; anything else arrives as surface pixels.
 *
 * The program's vertex shader is `hlsl`, the library nv2a_vsh_emit_hlsl
 * writes; the back end adds main(). After the program, as the executor does
 * after nv2a_vsh_run, a back end must turn oPos.w == +0 into 1 (a program
 * that never divides by w) and multiply oPos.x and .y by aa_sx and aa_sy.
 * oPos is then in surface pixels with w the clip w, as the executor's own
 * path sees it: clip at w = 0.001 and, when near_clip is set, at window z =
 * near_z (z * w >= near_z * w), then divide. */
typedef struct {
    const float    (*inputs)[4];    /* per distinct vertex, the inputs the program
                                       reads (inputs_read), in ascending v order */
    uint32_t         input_count;   /* distinct vertices */
    uint32_t         inputs_read;   /* bit k: v[k] is carried (nv2a_vsh_inputs_read) */
    const uint32_t  *indices;       /* triangle list over the distinct vertices, in
                                       the order the executor would rasterise */
    uint32_t         index_count;   /* a multiple of 3 */
    const float    (*constants)[4]; /* c[0..191] as the batch sees them */
    const char      *hlsl;          /* the program (nv2a_vsh_emit_hlsl) */
    uint32_t         hlsl_len;
    uint32_t         program_revision;  /* nv2a_vsh_program_revision: hlsl changes with it */
    uint64_t         program_key;   /* nv2a_vsh_program_key: equal keys, equal hlsl */
    float            aa_sx, aa_sy;  /* oPos.x / .y scale after the program */
    float            near_z;        /* window z of the near plane (c[59].z) */
    uint32_t         near_clip;     /* 0: clip at w only (RECOMP_CLIP_NEAR_Z=0) */
    /* Texture coordinates are what the program writes (o[9..12]). The surface
     * pixel path hands stage 0's over multiplied by this (Nv2aVertex.u/v are
     * texels): the texture's size for the formats addressed in 0..1, 1 for
     * the linear ones addressed in texels. */
    float            tex0_scale[2];
} Nv2aProgramBatch;

/* One texture stage as the register combiners read it. */
typedef struct {
    Nv2aTexture tex;            /* valid only when `valid` */
    uint32_t mode;              /* SET_SHADER_STAGE_PROGRAM's 5 bits for this stage */
    uint32_t valid;             /* a texture is bound */
    uint32_t size_from_format;  /* normalised coordinates (swizzled/DXT): times width/height */
    uint32_t bilinear;          /* the executor's tent filter: MAG 2, MIN 2/4/6, or RECOMP_TEX_BILINEAR */
    uint32_t border_u, border_v;/* this axis reads border_argb outside the texture */
    uint32_t border_argb;       /* SET_TEXTURE_BORDER_COLOR */
    uint32_t cube;              /* six faces (mode 3 picks one); decode gives face 0 */
} Nv2aStage;

/* The pixel pipeline when the title has programmed the register combiners:
 * what nv2a_rc_eval reads, the four stages and fog. See the executor's
 * raster_xf_triangle and xf_rows for the CPU reference a back end matches. */
typedef struct {
    Nv2aCombiner rc;            /* as the title set it (stage_program included) */
    Nv2aStage    stage[4];
    uint32_t     clip_plane_mode;   /* stage mode 5: which coordinates kill the pixel */
    uint32_t     fog_enable, fog_mode, fog_color;   /* fog colour R in bits 0-7 */
    float        fog_param[2];
} Nv2aPixelState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    /* Non-NULL: a vertex-program batch before transform (only for a back end
     * that asked, nv2a_backend_set_program_batches); vertices and count are
     * then NULL and 0. */
    const Nv2aProgramBatch *program;
    /* Non-NULL: draw through the register combiners (the executor's CPU
     * rasteriser would). NULL: texture stage 0 times diffuse, as before. */
    const Nv2aPixelState *pixel;
    /* With pixel, for a surface-pixel batch: each vertex's full outputs, in
     * Nv2aVshOutput's layout (pos as `vertices` has it, d0, d1, fog, tex[4]),
     * parallel to `vertices`. NULL for program batches. */
    const float (*xf)[32];
} Nv2aBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* Nonzero: the registered back end's draw() also takes vertex-program batches
 * before transform (Nv2aBatch.program, Nv2aProgramBatch). Off by default and
 * after every nv2a_backend_register*: call it after registering. */
void nv2a_backend_set_program_batches(int enable);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* Optional title-specific HLE of consumed software methods. Nonzero means handled.
 * Install before guest execution. It runs on the executor thread, not a guest CPU. */
void nv2a_pb_set_software_method_handler(int (*handler)(uint32_t));

#endif /* NV2A_BACKEND_H */
