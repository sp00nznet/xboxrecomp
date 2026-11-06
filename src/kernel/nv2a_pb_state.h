/**
 * The pushbuffer executor's decoded NV2A state, and the backend hook table.
 *
 * nv2a_pb_exec.c walks the title's pushbuffer and decodes it into the structs
 * below: surfaces, vertex arrays, the batch being built, texture registers,
 * the transform program and its constants, combiners and depth state. Drawing
 * is a separate concern. A backend gets events (clear, depth clear, draw,
 * flip, and optionally the visibility-test ones: zpass enable, report clear,
 * report, idle poll) and reads whatever else it needs from this state,
 * read-only.
 *
 * The default backend is the CPU rasteriser in nv2a_pb_exec.c. A GPU backend
 * (D3D11, Metal) registers itself with nv2a_pb_set_backend() and replaces it.
 *
 * Everything here runs on the NV2A ack thread, which drives the executor. The
 * state pointers stay valid for the life of the process, but the contents are
 * only consistent inside a hook call.
 *
 * Texture caches (one per backend: ARGB buffers for the CPU path, textures for
 * a GPU one) use the same key and invalidation policy, so the two agree on
 * when a texture is stale. Key: offset + format + width + height + pitch,
 * plus the mip level count for a backend that uploads more than level 0
 * (the CPU path samples level 0, so its count is always 1). Invalidation: a
 * per-flip generation, plus a hash of the whole texture, every uploaded
 * level included (nv2a_tex_hash, on every path). A first/last-page
 * fingerprint, which the CPU path once used, is not enough: a letterboxed
 * movie texture is black at both ends, so it changes only in the middle.
 */
#ifndef XBOXRECOMP_NV2A_PB_STATE_H
#define XBOXRECOMP_NV2A_PB_STATE_H

#include <stdint.h>
#include "nv2a_vsh_cpu.h"   /* NV2A_VSH_SLOTS / NV2A_VSH_CONSTANTS */
#include "nv2a_vsh_ff.h"    /* struct nv2a_ff_regs */

#ifdef __cplusplus
extern "C" {
#endif

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
/* Indices per BEGIN/END batch. The NV2A sets no limit a title would meet:
 * an Xbox 1.0 takes a batch of at least 0x0FFFFF elements, and retail titles
 * send at least 0x410FA in one draw (xemu's NV2A_MAX_BATCH_LENGTH, raised
 * from 0x1FFFF to 0x07FFFF when one did). At 4096, a water grid of 4592
 * indices (1148 quads) lost its farthest rows, which left the hard-edged
 * horizon split. 0x80000 is xemu's bound as a power of two; a
 * longer batch is still cut, with one warning. idx stays 16-bit: the hardware
 * faults on a DRAW_ARRAYS start above 0xFFFF, so no batch references more
 * than 65536 vertices. */
#define NV_MAX_INDICES  0x80000u
/* Dwords of INLINE_ARRAY (or immediate-mode vertices) per batch. 4096 held
 * a few hundred vertices; a batch past it was cut short with no sign. 64K
 * holds a thousand vertices of all sixteen attributes (upstream 5c3a42c). */
#define NV_MAX_INLINE   65536

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t border;                    /* SET_TEXTURE_BORDER_COLOR      */
    int      border_on;                 /* FORMAT BORDER_SOURCE: colour  */
    uint32_t palette, pal_len;          /* P8: guest palette (0 none), entries */
    uint32_t filter;                    /* SET_TEXTURE_FILTER as written */
    uint32_t face_stride;               /* cube map: bytes between faces, else 0 */
    int      valid;
} Texture;

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* Surfaces, the batch being built, stage-0 texture, blending, flips. */
struct nv2a_pb_gpu {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint16_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Immediate mode (SET_VERTEX_DATA*): how many complete vertices the
     * batch has produced. Each one is a snapshot of every attribute's
     * current inline value, nv2a_pb_vsh.inl, as 16 float4 in inline_buf. */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the CPU rasteriser last drew or cleared into. What a flip
     * presents is nv2a_pb_present_state(), not this: the last surface drawn
     * can be an offscreen target. */
    uint32_t drawn_offset;
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    /* SET_CLEAR_RECT_HORIZONTAL/VERTICAL: (max << 16) | min, both inclusive,
     * as xemu reads them. clear_rect_set: bit 0 horizontal, bit 1 vertical
     * sent; every path clears this rect alone once both have been, else the
     * clip (nv2a_clear_box): one half alone would read the other as 0 and
     * cover a single row or column. */
    uint32_t clear_rect_h, clear_rect_v;
    int      clear_rect_set;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    /* SET_BLEND_COLOR: the CONSTANT_COLOR/ALPHA factors' colour, packed
     * ARGB as the method carries it (xemu: NV_PGRAPH_BLENDCOLOR). */
    uint32_t blend_color, blend_color_writes;
    /* SET_BLEND_EQUATION, the raw GL code (0x8006 FUNC_ADD, 0x800A
     * FUNC_SUBTRACT, 0x800B FUNC_REVERSE_SUBTRACT, 0x8007 MIN, 0x8008 MAX,
     * 0xF005 REVERSE_SUBTRACT_SIGNED, 0xF006 ADD_SIGNED). 0, never written,
     * is ADD, the reset state. */
    uint32_t blend_equation;
    /* SET_COLOR_MASK, inverted: the ARGB bits a draw leaves alone. 0 (the
     * reset state) writes every channel; 0xFFFFFFFF writes none, as for a
     * stencil- or depth-only pass. */
    uint32_t color_keep;
    Texture  tex;
    uint32_t sema_dma, sema_offset;     /* SET_CONTEXT_DMA_SEMAPHORE / _OFFSET */
    int      sema_offset_set;           /* offset 0 is a real address (0x80000000) */
};

/* A vertex after the transform program: what the CPU rasteriser consumes. */
typedef struct {
    float x, y, z, w;           /* screen pixels, depth units, clip w */
    float d0[4], d1[4];
    float t[4][4];              /* oT0..oT3 */
    float fog;                  /* fog factor (FOG register alpha), unclamped */
    int   ok;
} XfVert;

/* The transform unit, combiners, depth and cull state, plus survey counters. */
struct nv2a_pb_vsh {
    uint32_t prog[NV2A_VSH_SLOTS][4];
    uint32_t load, start;
    float    c[NV2A_VSH_CONSTANTS][4];
    uint32_t cload;
    uint32_t mode;
    int      cxt_write;
    float    vp_off[4], vp_scale[4];
    float    inl[NV_VERTEX_ATTRS][4];   /* SET_VERTEX_DATA* current values */
    int      cull_enable, depth_enable, depth_mask;
    uint32_t cull_face, front_face, depth_func;
    uint32_t zeta_offset, zclear;
    /* Register combiners and the texture shader, as last programmed. */
    uint32_t rc_cicw[8], rc_aicw[8], rc_cocw[8], rc_aocw[8];
    uint32_t rc_f0[8], rc_f1[8], rc_ctl, rc_fcw0, rc_fcw1, rc_sf[2];
    uint32_t shader_prog, fog_color;
    uint32_t other_stage_input;         /* SET_SHADER_OTHER_STAGE_INPUT: the stage
                                         * a dependent read (BUMPENVMAP, ...)
                                         * takes its input from: stage 2 in
                                         * bits 16-19, stage 3 in 20-23 */
    uint32_t dot_rgbmapping;            /* SET_DOT_RGBMAPPING: how the dot-product
                                         * modes read their input texel, 3 bits
                                         * per stage, stage 1 in bits 0-2,
                                         * stage 2 in 4-6, stage 3 in 8-10 */
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE: per stage
                                         * and component, kill at >= 0 (1) or < 0 */
    int      rc_set, shader_set, fog_enable, atest_enable;
    uint32_t atest_func, atest_ref;
    int      poly_offset;
    float    poly_factor, poly_bias;
    /* SET_CLIP_MIN / SET_CLIP_MAX, in depth units (0..zmax), and
     * SET_ZMIN_MAX_CONTROL raw (bits 7:4 ZCLAMP_EN: 0 cull outside the
     * range, 1 clamp to it). clip_set: bit 0 CLIP_MIN was written, bit 1
     * CLIP_MAX; an unwritten one stays at its end of [0, zmax]
     * (nv2a_wdepth_consts_fill), as the state is zero-initialised and a max
     * of 0 would cull every W draw. */
    float    clip_min, clip_max;
    int      clip_set;
    uint32_t zminmax_ctl;
    /* SET_CONTROL0: bit 16 Z_PERSPECTIVE_ENABLE (w-buffered depth), bit 12
     * Z_FORMAT float, bit 0 STENCIL_WRITE_ENABLE. */
    uint32_t control0;
    /* Fog and specular, as xemu's vsh.c turns them into vtxFog and vtxD1.
     * fog_mode and fog_gen are the raw SET_FOG_MODE / SET_FOG_GEN_MODE
     * parameters (0x800 EXP, 0x801 EXP2, 0x802 EXP_ABS, 0x803 EXP2_ABS,
     * 0x804 LINEAR_ABS, 0x2601 LINEAR); fog_param is SET_FOG_PARAMS. */
    uint32_t fog_mode, fog_gen, spec_enable, light_ctl;
    float    fog_param[3];
    /* Stencil, as xemu's CONTROL_1/2 hold it: func is the low nibble of
     * SET_STENCIL_FUNC (0 NEVER .. 7 ALWAYS), ref and the masks are 8 bits,
     * the ops are the raw GL codes (0x1E00 KEEP, 0 ZERO, 0x1E01 REPLACE,
     * 0x1E02/3 INCR/DECR_SAT, 0x150A INVERT, 0x8507/8 INCR/DECR wrap) for
     * fail, zfail, zpass. Only a Z24S8 zeta surface has a stencil buffer. */
    int      stencil_enable;
    uint32_t stencil_func, stencil_ref, stencil_rmask, stencil_wmask;
    uint32_t stencil_op[3];
    /* The fixed-function registers (lights, material, texgen, skinning), for
     * the program nv2a_vsh_ff.c builds; ff_ready once nv2a_ff_init ran.
     * ff_composite: a COMPOSITE_MATRIX row was written, so a fixed-function
     * batch has a transform to run. */
    struct nv2a_ff_regs ff;
    int      ff_ready, ff_composite;
    /* survey */
    uint32_t prog_dwords, prog_loads, prog_starts, const_dwords, const_loads;
    uint32_t mode_sets, ff_matrix_dwords, vp_dwords;
    uint32_t batches_prog, batches_fixed, verts_run, verts_bad;
    uint32_t batches_ff, ff_too_long, ff_programs;
    uint32_t tris, tris_culled, tris_clipped, tris_textured;
    uint64_t z_rejected, a_rejected;
    uint32_t clears_seen, zclears, batches_3d, dumps_3d;
    uint32_t zclear_color_off, zclear_zeta_off, zclear_batch;
    uint32_t batches_rc, batches_norc, stage_mode_seen[4][32];
    uint32_t fmt_seen[NV_VERTEX_ATTRS][8];  /* per slot: batches by type */
    int      disabled;
};

/* Every texture-stage register (methods NV_TEX_FIRST..NV_TEX_LAST), as the
 * title last set it. Register i is method NV_TEX_FIRST + 4*i. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
#define NV_TEX_REGS  ((NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1)

/* Read-only views of the executor's state. */
const struct nv2a_pb_gpu *nv2a_pb_gpu_state(void);
/* The index-batch counters of the [GPU] summary line: the largest batch,
 * the batches that lost indices past NV_MAX_INDICES, and the indices past
 * 0xFFFF truncated to 16 bits. For the backend smoke test. */
void nv2a_pb_exec_idx_stats(uint32_t *max, uint32_t *overflowed, uint32_t *over_ffff);
const struct nv2a_pb_vsh *nv2a_pb_vsh_state(void);
/* Bumped whenever the executor writes nv2a_pb_vsh_state()->c, so a backend
 * can tell that c[] is unchanged since it last uploaded it without comparing
 * its 3 KB per batch. Equal values mean no write in between; a changed
 * value says nothing about the bytes (a title rewrites the same matrices). */
uint32_t nv2a_pb_vsh_consts_gen(void);
const uint32_t *nv2a_pb_tex_regs(void);       /* NV_TEX_REGS words */
const uint8_t  *nv2a_pb_tex_regs_set(void);   /* 1 where the title wrote one */
/* Bumped whenever a texture register or its set flag changes value, so a
 * backend can keep the stages it decoded from them (as with
 * nv2a_pb_vsh_consts_gen, equal means no change in between). */
uint32_t nv2a_pb_tex_regs_gen(void);

/* A DMA-object offset (surface, zeta, semaphore) as a guest address. Vertex
 * array and texture offsets in the state above are already resolved. */
uint32_t nv2a_pb_dma_resolve(uint32_t offset);

/* The colour surface a flip presents, as the walker picks it at FLIP_STALL
 * (nv2a_pb_exec.c, present_pick): of the surfaces the size of the display's,
 * the one drawn into most recently since the last flip, else the one cleared
 * most recently; else the previous choice stays. A surface is a colour offset
 * plus the size its clip gave it (clip_x + clip_w by clip_y + clip_h) when it
 * was drawn, with the pitch and format it had then. offset is 0 until the
 * first pick. Valid inside on_flip and after it.
 *
 * Walker thread only: present_pick writes it on the NV2A ack thread, and
 * every reader (the backends' on_flip, nv2a_pb_exec_report) runs there too.
 * There is no lock. The pick is stored with a single struct assignment, but
 * that is not atomic: a reader on another thread (a separate presenter, say)
 * needs a generation counter or a lock added here first. */
struct nv2a_pb_present {
    uint32_t offset;      /* DMA offset, as SET_SURFACE_COLOR_OFFSET set it */
    uint32_t w, h;
    uint32_t pitch, format;
};
const struct nv2a_pb_present *nv2a_pb_present_state(void);

/* The walker's flip count (FLIP_STALLs so far), the number a flip's
 * on_flip runs under and the HUD capture's requests name (nv2a_cap_take).
 * A backend's own present count can fall behind it (a flip before its
 * device is up never reaches its present). Walker thread only, as above. */
uint32_t nv2a_pb_flips(void);

/* RECOMP_FB_DUMP_AT: whether `flip` is listed. The executor dumps listed
 * flips for the CPU path and backends that write back; a backend that
 * presents from its own surfaces (D3D11) asks this at each present and dumps
 * the frame itself, as <prefix>flip_NNNNN.bmp. */
int nv2a_pb_dump_at_listed(uint32_t flip);

/* One batch, at SET_BEGIN_END(0).
 *
 * attr[i].offset is a guest address, unless `inline_data` is set: then the
 * batch came from INLINE_ARRAY or SET_VERTEX4F, and the offsets are byte
 * offsets into `inline_buf` (inline_bytes long). Vertices are idx[0..idx_count)
 * either way; DRAW_ARRAYS runs arrive expanded into indices. `program` is set
 * when the batch runs the vertex program in nv2a_pb_vsh_state()->prog from
 * ->start; otherwise attribute 0 is already in screen space. */
struct nv2a_pb_draw {
    uint32_t          prim;          /* NV_PRIM_* */
    const VertexAttr *attr;          /* NV_VERTEX_ATTRS streams */
    const uint16_t   *idx;
    uint32_t          idx_count;
    int               inline_data;
    const uint32_t   *inline_buf;
    uint32_t          inline_bytes;
    int               program;
    uint32_t          batches;       /* guest batches drawn: 1, or a merged
                                      * chain's length (nv2a_merge); the flip
                                      * log's "batches" adds these, as golden
                                      * anchors count guest batches */
};

/* One 2D-engine blit (NV09F IMAGE_BLIT through NV062 CONTEXT_SURFACES_2D,
 * nv2a_2d.h): copy w x h pixels at (in_x, in_y) of the source surface to
 * (out_x, out_y) of the destination. src and dst are the guest addresses of
 * each surface's first pixel (the DMA object's base plus its offset,
 * through nv2a_pb_dma_resolve); src_offset and dst_offset are those DMA
 * offsets before resolving, as a SET_SURFACE_COLOR_OFFSET would name the
 * same memory. */
struct nv2a_pb_blit {
    uint32_t src, dst;
    uint32_t src_offset, dst_offset;
    uint32_t src_pitch, dst_pitch;
    uint32_t format;      /* NV062_SET_COLOR_FORMAT */
    uint32_t bpp;         /* 1, 2 or 4 */
    uint32_t operation;   /* NV09F_OP_SRCCOPY or NV09F_OP_BLEND_AND */
    uint32_t in_x, in_y, out_x, out_y, w, h;
};

/* The blit in guest memory, as the CPU rasteriser has it: the rows copied,
 * the destination marked as written for the CPU texture cache. A GPU
 * backend whose on_blit cannot serve a blit calls this (after writing the
 * source back if it holds it), then drops what it caches over the
 * destination. Walker thread only. */
void nv2a_pb_blit_guest(const struct nv2a_pb_blit *b);

/* Context DMA and object lookups so far: full RAMHT scans, and lookups the
 * kept slot answered (nv2a_pb_exec.c, ctxdma_lookup). For tests. */
void nv2a_pb_ramht_stats(uint64_t *scans, uint64_t *hits);

/* The backend's events. A NULL member ignores that event.
 *
 * on_clear / on_zclear: NV097_CLEAR_SURFACE, with its parameter (colour mask
 *   0xF0, Z bit 0x1, stencil 0x2). Both run for every clear, zclear first, and
 *   each decides from `param` whether it has anything to do.
 * on_draw: one batch; see struct nv2a_pb_draw.
 * on_flip: NV097_FLIP_STALL. `surface` is the DMA offset of the finished frame
 *   and `pitch` its row pitch: nv2a_pb_present_state() (else, before its first
 *   pick, drawn_offset or color_offset; 0 if none). The walker has already
 *   completed the flip for the guest. */
struct nv2a_pb_backend {
    void (*on_clear)(uint32_t param);
    void (*on_zclear)(uint32_t param);
    void (*on_draw)(const struct nv2a_pb_draw *draw);
    void (*on_flip)(uint32_t surface, uint32_t pitch);

    /* Visibility tests (optional, NULL to leave them to the walker, which
     * counts fragments on the CPU path and reports RECOMP_ZPASS_FIXED for a
     * backend without these). on_zpass: SET_ZPASS_PIXEL_COUNT_ENABLE;
     * on_zpass_clear: CLEAR_REPORT_VALUE. on_report: GET_REPORT for the
     * 16-byte report at guest address va; returning 1 means the backend
     * writes it -- {timestamp, count, status 0} -- now or later, and no
     * later than the next on_poll returns; until it does the status keeps
     * the 0xFFFFFFFF D3D put there (TESTINCOMPLETE).
     * on_poll: called on the ack thread, inside the walk before a
     * BACK_END_WRITE_SEMAPHORE_RELEASE that follows an on_report returning
     * 1, at the end of every walk before GET moves (the title can read a
     * test once either reaches it), and every pass of the ack loop,
     * pushbuffer or not. */
    void (*on_zpass)(int enable);
    void (*on_zpass_clear)(void);
    int  (*on_report)(uint32_t va);
    void (*on_poll)(void);
    /* Capabilities. writes_back: the backend's frames are dumped from guest
     * memory (RECOMP_FB_DUMP, fb_dump_at, RECOMP_FB_DUMP_FLIPS), after
     * sync_guest when it has one (Metal); D3D11 presents and dumps from its
     * own swap chain and the null backend draws nothing. */
    int  writes_back;
    /* sync_guest (optional): the executor is about to read the guest bytes
     * of the surface at colour offset `offset`, w x h at `pitch` (a frame
     * dump); a backend that keeps the surface on the GPU writes it back now.
     * Called on the ack thread only, like on_flip. NULL: guest memory is
     * already current (CPU) or never read (D3D11 dumps itself). */
    void (*sync_guest)(uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h);
    /* merge_draws: on_draw may get back-to-back batches as one triangle
     * list (nv2a_merge); the backend keeps no per-batch rule that the
     * merged list would change. With display.aspect stock and
     * render.snap_2d off the walker merges every joinable batch; under
     * either, which place or snap each batch from its own vertices, only
     * the batches merge_placed accepts. */
    int  merge_draws;
    /* merge_placed (optional; NULL: no merging under Hor+ or snap_2d):
     * whether the batch about to end, drawn with the current state, would
     * be placed the same from any vertices (nv2a_merge_placed). `program`
     * is 1 for a title program batch (transform mode PROGRAM). A
     * fixed-function batch asks with 0, even one that will draw through
     * its synthesised program: ff_begin runs at the draw, after END, so
     * fixed-function batches never merge under Hor+ or snap_2d. Walker
     * thread, at the batch's END. */
    int  (*merge_placed)(int program);
    /* on_blit (optional; NULL: nv2a_pb_blit_guest): a 2D-engine blit, in
     * pushbuffer order with the draws. A backend that renders into its own
     * targets copies on the GPU when the source is one of them (the
     * guest bytes under it are stale) and leaves the destination bound as
     * a target holding the copy; otherwise it calls nv2a_pb_blit_guest.
     * The backend owns the whole blit: the executor writes nothing. */
    void (*on_blit)(const struct nv2a_pb_blit *b);
};

/* Route the executor's events to `backend`. NULL restores the CPU rasteriser.
 * Call before the executor starts, or from the ack thread. */
void nv2a_pb_set_backend(const struct nv2a_pb_backend *backend);
const struct nv2a_pb_backend *nv2a_pb_get_backend(void);
/* The CPU rasteriser, so another backend can fall back to it or A/B against it. */
const struct nv2a_pb_backend *nv2a_pb_cpu_backend(void);

/* The CPU rasteriser's decoded-texture cache (fast path), for tests: live
 * entries, and since startup the binds, the binds an entry served (hits),
 * the entries built or rebuilt for a bind that missed, and the entries whose
 * guest bytes changed (their decoded tiles dropped). Walker thread only. */
struct nv2a_pb_tc_stats {
    uint32_t entries;
    uint64_t binds, hits, builds, dropped;
};
void nv2a_pb_exec_tc_stats(struct nv2a_pb_tc_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_PB_STATE_H */
