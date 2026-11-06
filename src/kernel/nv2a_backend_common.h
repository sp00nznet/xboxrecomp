/**
 * NV2A state logic shared by the pushbuffer backends.
 *
 * Everything here is API-independent: it turns NV2A register values and
 * guest memory into plain numbers and texels, and leaves the graphics API to
 * the backend. The D3D11 backend (src/d3d/nv2a_pb_d3d11.c) and the CPU
 * rasteriser (nv2a_pb_exec.c) both use it, so a fix to a texel format, a
 * blend factor or a stage decode lands in both at once, and a new backend
 * (Metal) starts from the same tables.
 *
 * Builds on every platform: no OS or graphics headers. The per-texel and
 * per-vertex helpers are static inline because the CPU path calls them per
 * pixel.
 */
#ifndef NV2A_BACKEND_COMMON_H
#define NV2A_BACKEND_COMMON_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../d3d/d3d8_swizzle.h"
#include "../d3d/d3d8_vsh_parse.h"
#include "nv2a_pb_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */
/* ------------------------------------------------------------------------ */

#define NV2A_TEX_P8 0x0Bu               /* SZ_I8_A8R8G8B8, through a palette */
#define NV2A_TEX_MAGENTA 0xFFFF00FFu    /* what an undecodable texel reads as */

/* The linear format with the same texel encoding as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06)
 * and LIN_A8R8G8B8 (0x12) are the same four bytes in the same order, so the
 * whole difference is the address calculation, and one decoder serves both.
 * Pairs read off the table in d3d8_xbox.h. */
static inline uint32_t nv2a_tex_linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;             /* L8        -> LIN_L8        */
    case 0x02: return 0x10;             /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;             /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;             /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;             /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;             /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;             /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;             /* A8        -> LIN_A8        */
    case 0x01: return 0x1B;             /* AY8       -> LIN_AY8       */
    case 0x1A: return 0x20;             /* A8Y8      -> LIN_A8Y8      */
    case 0x27: return 0x37;             /* R6G5B5    -> LIN_R6G5B5    */
    case 0x28: return 0x17;             /* G8B8      -> LIN_G8B8      */
    case 0x29: return 0x16;             /* R8B8      -> LIN_R8B8      */
    case 0x32: return 0x35;             /* Y16       -> LIN_Y16       */
    case 0x38: return 0x3D;             /* R5G5B5A1  -> LIN_R5G5B5A1  */
    case 0x39: return 0x3E;             /* R4G4B4A4  -> LIN_R4G4B4A4  */
    case 0x3A: return 0x3F;             /* A8B8G8R8  -> LIN_A8B8G8R8  */
    case 0x3B: return 0x40;             /* B8G8R8A8  -> LIN_B8G8R8A8  */
    case 0x3C: return 0x41;             /* R8G8B8A8  -> LIN_R8G8B8A8  */
    default:   return fmt;              /* already linear, P8, or unhandled */
    }
}

/* Bytes per texel of a linear format nv2a_tex_decode_texel reads (P8
 * included), 0 if it has no decoder. */
static inline uint32_t nv2a_tex_texel_bytes(uint32_t lin)
{
    switch (lin) {
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x10: case 0x1C: case 0x11: case 0x1D: case 0x24: case 0x25:
    case 0x20: case 0x37: case 0x35: case 0x3D: case 0x3E:
    case 0x17: case 0x16: return 2;
    case 0x13: case 0x1F: case 0x1B: case 0x0B: return 1;
    default: return 0;
    }
}

/* Whether the size comes from the format register (swizzled and DXT, power
 * of two, mipmapped) rather than from the image-rect register (linear). */
static inline int nv2a_tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

/* Level l's size: halved per level, never below 1. */
static inline uint32_t nv2a_tex_level_dim(uint32_t d, uint32_t l)
{
    d >>= l;
    return d ? d : 1u;
}

/* Bytes of guest memory a w x h texture of `levels` levels occupies, 0 if it
 * cannot be decoded. Swizzled and DXT levels are packed one after another;
 * a linear texture is one level at `pitch` bytes per row. The caller checks
 * validity and its own size limit first. */
/* SET_TEXTURE_FILTER, as every path reads it: MIN in bits 16-21, MAG in
 * 24-27, the LOD bias in 0-12. MIN: 1 nearest and 2 linear on level 0, 3/4
 * nearest/linear within the nearest level, 5/6 nearest/linear blended
 * between two levels, 7 convolution on level 0 (taken as linear). MAG: 1
 * nearest, 2 linear, 4 convolution (linear). */
static inline int nv2a_tex_filter_linear(uint32_t f, int mag)
{
    uint32_t m = mag ? (f >> 24) & 0xF : (f >> 16) & 0x3F;
    if (mag)
        return m == 2 || m == 4;
    return m == 2 || m == 4 || m == 6 || m == 7;
}

/* 0: level 0 only, 1: nearest level, 2: blend two levels. */
static inline int nv2a_tex_filter_mip(uint32_t f)
{
    uint32_t m = (f >> 16) & 0x3F;
    return m == 3 || m == 4 ? 1 : m == 5 || m == 6 ? 2 : 0;
}

/* The mip LOD bias, signed 5.8 fixed point. */
static inline float nv2a_tex_lod_bias(uint32_t f)
{
    int b = (int)(f & 0x1FFFu);
    if (b & 0x1000)
        b -= 0x2000;
    return (float)b / 256.0f;
}

/* Cube-map face and its 2D coordinates for direction `c` (xemu, and the
 * D3D face order +X -X +Y -Y +Z -Z); s, t in [0,1] on the face. */
static inline uint32_t nv2a_cube_face(const float c[3], float *s, float *t)
{
    float x = c[0], y = c[1], z = c[2];
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
    uint32_t face;
    if (ax >= ay && ax >= az) {
        face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
    } else if (ay >= az) {
        face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
    } else {
        face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
    }
    if (ma == 0.0f)
        ma = 1.0f;
    *s = (sc / ma + 1.0f) * 0.5f;
    *t = (tc / ma + 1.0f) * 0.5f;
    return face;
}

/* Bytes from one cube-map face to the next: a face with all `levels` of
 * its levels, rounded up to the NV2A's 128-byte face alignment (xemu
 * texture.c). A linear face is `pitch` * h. */
uint32_t nv2a_tex_face_stride(uint32_t fmt, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t levels);

uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels);

/* Level l's byte offset from level 0 in a swizzled or DXT mip chain (the
 * levels before it, packed). */
static inline uint32_t nv2a_tex_level_offset(uint32_t fmt, uint32_t w,
                                             uint32_t h, uint32_t l)
{
    return l ? nv2a_tex_extent(fmt, w, h, 0, l) : 0u;
}

/* A sampler's level of detail, for a sampler that computes its own (the
 * GPU paths get it from their sampler state: MipLODBias, MinLOD, MaxLOD):
 * log2 of rho, the larger of the pixel's x and y footprints in level-0
 * texels (rho2x, rho2y are their squares), plus the filter's LOD bias,
 * clamped to CONTROL0's [lod_min, lod_max]. No footprint (0 or NaN) gives
 * lod_min. */
static inline float nv2a_tex_lod(float rho2x, float rho2y, float bias,
                                 float lod_min, float lod_max)
{
    float r2 = rho2x > rho2y ? rho2x : rho2y, l;
    if (!(r2 > 0.0f))
        return lod_min;
    l = 0.5f * log2f(r2) + bias;
    if (!(l > lod_min)) return lod_min;     /* NaN too */
    return l < lod_max ? l : lod_max;
}

/* The level(s) MIN mip mode `mip` (nv2a_tex_filter_mip) reads at a clamped
 * LOD (>= 0) of a chain of `levels`: mode 1 the nearest level (*l0), mode 2
 * the two around it, *l1 weighted *f. Level l1 == l0 with *f 0 when one
 * level is all there is. */
static inline void nv2a_tex_mip_levels(int mip, float lod, uint32_t levels,
                                       uint32_t *l0, uint32_t *l1, float *f)
{
    uint32_t top = levels ? levels - 1u : 0u, l;
    if (mip == 2) {
        l = (uint32_t)lod;                  /* lod >= 0: floor */
        if (l >= top) { *l0 = *l1 = top; *f = 0.0f; return; }
        *l0 = l; *l1 = l + 1u; *f = lod - (float)l;
        return;
    }
    l = mip == 1 ? (uint32_t)(lod + 0.5f) : 0u;
    *l0 = *l1 = l < top ? l : top;
    *f = 0.0f;
}

/* FNV-style over n bytes, four independent lanes so it runs at memory
 * speed. h is the seed (NV2A_TEX_HASH_SEED for a fresh hash). */
#define NV2A_TEX_HASH_SEED 0xCBF29CE484222325ull
uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h);

/* Render-target ownership. A GPU backend keeps a target per surface the
 * title draws into, but the title owns that memory: it can free it and load
 * a stage's textures, vertices or tables there, and the old image must then
 * neither be sampled nor written back over them. Recompiled stores go
 * straight to guest memory, so there is no write tracking to ask; a target
 * keeps instead the hash of its guest bytes (pitch x h at its resolved
 * address, the bytes the seed reads and a write-back writes, clamped to the
 * window it lies in) as of the last time the backend knew them: its
 * creation and each write-back. A target not drawn in the current flip whose
 * bytes no longer hash the same was overwritten by the title and is stale.
 * One check per target per flip at most (checked_flip caches a good
 * result), plus one after each reset. */
#define NV2A_RT_WINDOW 0x08000000u   /* address compares use the low 27 bits */
/* The windows a resolved address lies in, as kernel.h's XBOX_CONTIG_BASE and
 * XBOX_CONTIG_SIZE and the 128 MB low mapping (this header takes no kernel
 * headers). */
#define NV2A_RT_CONTIG_BASE 0x80000000u
#define NV2A_RT_CONTIG_SIZE 0x04000000u
struct nv2a_rt_own {
    uint64_t mem_hash;
    uint32_t draw_flip;      /* the flip of the last draw or clear into it */
    uint32_t checked_flip;   /* the flip of the last hash */
};

/* Hash now: the bytes are the target's (creation, write-back). */
void nv2a_rt_own_reset(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                       uint32_t pitch, uint32_t h, uint32_t flip);

static inline void nv2a_rt_own_drawn(struct nv2a_rt_own *o, uint32_t flip)
{
    o->draw_flip = flip;
}

/* 1 when the title has rewritten the target's bytes: never for a target
 * drawn this flip, nor rehashed twice in one flip. */
int nv2a_rt_own_stale(struct nv2a_rt_own *o, const uint8_t *mem, uint32_t addr,
                      uint32_t pitch, uint32_t h, uint32_t flip);

/* Hashes taken so far by the two above (tests and the profile). */
uint64_t nv2a_rt_own_hashes(void);

/* Whether [a, a + an) and [b, b + bn) share a byte, through the low 27 bits
 * (the backends' rule for "the same memory through either window"). */
static inline int nv2a_rt_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    a &= NV2A_RT_WINDOW - 1u;
    b &= NV2A_RT_WINDOW - 1u;
    return an && bn && a < b + bn && b < a + an;
}

/* Bytes per pixel of SET_SURFACE_FORMAT's colour field, 0 if unknown (the
 * table nv2a_pb_exec.c's surf_fmt_bpp reads). */
static inline uint32_t nv2a_surf_color_bytes(uint32_t format)
{
    switch (format & 0xFu) {
    case 0x1: case 0x2: case 0x3: return 2;    /* X1R5G5B5 x2, R5G6B5 */
    case 0x4: case 0x5: case 0x6: case 0x7:
    case 0x8: case 0xC: return 4;              /* X8R8G8B8 x2, X1A7.., A8R8G8B8, A8B8G8R8 */
    case 0x9: return 1;                        /* B8 */
    case 0xA: case 0xB: return 2;              /* G8B8 */
    default:  return 0;
    }
}

/* R5G6B5, X1R5G5B5 and X8R8G8B8 have no alpha, so a target read as one
 * reads alpha 1 as their decoders give it (nv2a_ps_consts' alpha_only 2);
 * the target's alpha is whatever the draws into it left. */
static inline int nv2a_rt_no_alpha(uint32_t lin)
{
    return lin == 0x11 || lin == 0x1C || lin == 0x1E;
}

/* Whether a texture stage at a render target's address (color: its format
 * register's colour field, tex_pitch: its pitch) samples that target in
 * place rather than decoding guest memory. rt_fmt is the SET_SURFACE_FORMAT
 * word the target was drawn with. The read must see the bytes the surface
 * holds: the same colour layout (R5G6B5 as R5G6B5, X1R5G5B5 as A1R5G5B5 or
 * X1R5G5B5, any 32-bit texture of a 32-bit surface), the same memory order
 * (swizzled as swizzled, linear as linear at the target's pitch), not DXT.
 * Read another way the same address is not the target's picture on the
 * console either. A4R4G4B4 matches no surface format.
 *
 * Hor+ depends on it: the target is padded and its guest bytes are not (a
 * write-back keeps the stock columns), so a frame copy decoded from them is
 * a 4:3 picture, centred, under a full-screen effect that should cover the
 * padded frame. Burnout 3 runs its speed blur through 16-bit R5G6B5
 * targets of 320x240 and 160x120. */
static inline int nv2a_rt_tex_reads(uint32_t color, uint32_t tex_pitch, uint32_t rt_fmt,
                                    uint32_t rt_pitch)
{
    uint32_t lin = nv2a_tex_linear_twin(color);
    uint32_t surf = rt_fmt & 0xFu;
    int rt_swizzled = ((rt_fmt >> 8) & 0xFu) == 2u;   /* SURFACE_FORMAT type */

    if (d3d8_format_dxt_block_bytes(color) || d3d8_format_is_swizzled(color) != rt_swizzled
        || (!rt_swizzled && tex_pitch != rt_pitch))
        return 0;
    switch (lin) {
    case 0x11: return surf == 0x3;                     /* R5G6B5 */
    case 0x10: case 0x1C: return surf == 0x1 || surf == 0x2;   /* X1R5G5B5 */
    default:   return nv2a_tex_texel_bytes(lin) == 4 && nv2a_surf_color_bytes(rt_fmt) == 4;
    }
}

/* Whether a texture over a dirty render target's memory that the target
 * cannot serve (nv2a_rt_tex_reads said no) decodes guest memory as it
 * stands instead of having the target written back first: a linear read of
 * a linear target, more than one row tall, at a narrower pitch than the
 * target's. Each texture row is then a slice of a target row and the next
 * texture row starts partway along the same target row, so the texels are
 * the target's rows cut up and restacked: on the console too that is no
 * picture of the target, and a write-back would only make the decode exact
 * for bytes no picture depends on. Kingdom Under Fire's front end reads its
 * 1280x960 pitch-5120 XDK supersample backbuffer through a 640x480
 * pitch-2560 header into a 256x256 copy it never samples; the
 * write-back of the whole target cost one readback per flip, worse at each
 * render scale. A wider pitch keeps the write-back: at k times the target's
 * pitch each texture row is a whole target row, so the texture is the
 * target's picture with rows skipped (a field read). So do a one-row read
 * (a span of one target row), same-pitch, swizzled and DXT reads. */
static inline int nv2a_rt_pitch_alias(uint32_t color, uint32_t tex_pitch, uint32_t tex_height,
                                      uint32_t rt_fmt, uint32_t rt_pitch)
{
    int rt_swizzled = ((rt_fmt >> 8) & 0xFu) == 2u;   /* SURFACE_FORMAT type */

    return !rt_swizzled && !d3d8_format_is_swizzled(color) && !d3d8_format_dxt_block_bytes(color)
           && tex_pitch && tex_pitch < rt_pitch && tex_height > 1;
}

/* Whether a (target format, target pitch, texture format, texture pitch)
 * key is new to one trace's seen-set, so a trace says each distinct pair
 * once. A small open-addressed set (NV2A_PAIR_SET slots, the caller's);
 * half full, it says nothing more. */
enum { NV2A_PAIR_SET = 256 };
struct nv2a_pair_set {
    uint64_t key[NV2A_PAIR_SET];
    unsigned used;
};

static inline int nv2a_pair_new(struct nv2a_pair_set *s, uint32_t rt_fmt, uint32_t rt_pitch,
                                uint32_t color, uint32_t tex_pitch)
{
    uint64_t key = (((uint64_t)rt_fmt << 32 | rt_pitch) * 0x9E3779B97F4A7C15ull
                    ^ ((uint64_t)color << 32 | tex_pitch)) | 1u;
    unsigned h = (unsigned)(key >> 56) & (NV2A_PAIR_SET - 1u);

    while (s->key[h] && s->key[h] != key)
        h = (h + 1u) & (NV2A_PAIR_SET - 1u);
    if (s->key[h] || s->used >= NV2A_PAIR_SET / 2u)
        return 0;
    s->key[h] = key;
    s->used++;
    return 1;
}

/* Whether this pair is new to the caller's trace of reads nv2a_rt_tex_reads
 * rejects. One set per translation unit. */
static inline int nv2a_rt_reject_new(uint32_t rt_fmt, uint32_t rt_pitch, uint32_t color,
                                     uint32_t tex_pitch)
{
    static struct nv2a_pair_set set;

    return nv2a_pair_new(&set, rt_fmt, rt_pitch, color, tex_pitch);
}

/* A swizzled surface's side from its SURFACE_FORMAT log2 field, at most
 * 4096 (the NV2A limit): a garbage format would otherwise overflow the
 * w*h*bpp extent the CPU clear checks before writing. */
static inline uint32_t nv2a_swizzle_side(uint32_t log2)
{
    return 1u << (log2 > 12u ? 12u : log2);
}

/* What a colour clear covers, in guest pixels, box = {x0, y0, x1, y1} with
 * x1/y1 exclusive: the clear rect once the title has set one (inclusive max),
 * else the surface clip, clamped to the surface and to a w x h target. The
 * rect alone, not its intersection with the clip: xemu clears the rect
 * clamped to the surface, and upstream #212 adopts that rule. The surface is
 * xemu's too: a swizzled one is its power-of-two FORMAT size (SSX 3 clears
 * 128x128 under a 124x124 clip and samples the ring), a pitch one ends at
 * the clip's far corner. A GPU target is only as large as the clips it was
 * drawn with, so there w x h can cut a swizzled rect short; neither BLiNX 2
 * nor Burnout 3 sends a rect past its clip. 0 when nothing is left.
 * One box for every path: since a GPU target serves every clip of its
 * surface (Burnout 3 clears 159x344 regions of its 640x480 back buffer), a
 * whole-target clear would wipe what another clip drew. */
static inline int nv2a_clear_box(const struct nv2a_pb_gpu *g, uint32_t w, uint32_t h,
                                 uint32_t box[4])
{
    uint32_t ax = g->clip_x, ay = g->clip_y;
    uint32_t bx = g->clip_x + g->clip_w, by = g->clip_y + g->clip_h;
    uint32_t sw = bx, sh = by;
    if (((g->format >> 8) & 0xFu) == 2u) {   /* SURFACE_FORMAT type: swizzled */
        sw = nv2a_swizzle_side((g->format >> 16) & 0x1Fu);
        sh = nv2a_swizzle_side((g->format >> 24) & 0x1Fu);
    }
    if ((g->clear_rect_set & 3) == 3) {
        ax = g->clear_rect_h & 0xFFFFu;
        bx = (g->clear_rect_h >> 16) + 1u;
        ay = g->clear_rect_v & 0xFFFFu;
        by = (g->clear_rect_v >> 16) + 1u;
    }
    if (w > sw) w = sw;
    if (h > sh) h = sh;
    if (bx > w) bx = w;
    if (by > h) by = h;
    if (ax >= bx || ay >= by)
        return 0;
    box[0] = ax;
    box[1] = ay;
    box[2] = bx;
    box[3] = by;
    return 1;
}

/* SET_COLOR_CLEAR_VALUE is in the colour surface's own format, not always
 * A8R8G8B8: on a 16-bit surface D3D packs the colour before sending it, as
 * xemu's clear unpacks it per format. JSRF (XDK 4134) draws its logos on an
 * R5G6B5 back buffer and clears to 0x0000FFFF for white: read as A8R8G8B8
 * that is cyan, and its near-black 0x20E4 came out blue.
 *
 * Nonzero, with the low 16 bits in *packed, when the surface is a 16-bit
 * RGB format, so the CPU path writes the value as given. */
static inline int nv2a_clear_packed16(const struct nv2a_pb_gpu *g, uint16_t *packed)
{
    switch (g->format & 0xFu) {
    case 0x1: case 0x2: case 0x3:          /* X1R5G5B5 x2, R5G6B5 */
        *packed = (uint16_t)g->clear_color;
        return 1;
    default:
        return 0;
    }
}

/* The clear colour as A8R8G8B8, for the GPU backends' 8-bit targets: a
 * packed 16-bit value widened per channel (a 5-bit 31 to 255), opaque. */
static inline uint32_t nv2a_clear_argb(const struct nv2a_pb_gpu *g)
{
    uint32_t v = g->clear_color & 0xFFFFu, r, gr, b;

    switch (g->format & 0xFu) {
    case 0x1: case 0x2:                    /* X1R5G5B5 */
        r = (v >> 10) & 0x1Fu;
        gr = (v >> 5) & 0x1Fu;
        b = v & 0x1Fu;
        r = (r << 3) | (r >> 2);
        gr = (gr << 3) | (gr >> 2);
        b = (b << 3) | (b >> 2);
        break;
    case 0x3:                              /* R5G6B5 */
        r = (v >> 11) & 0x1Fu;
        gr = (v >> 5) & 0x3Fu;
        b = v & 0x1Fu;
        r = (r << 3) | (r >> 2);
        gr = (gr << 2) | (gr >> 4);
        b = (b << 3) | (b >> 2);
        break;
    default:
        return g->clear_color;
    }
    return 0xFF000000u | (r << 16) | (gr << 8) | b;
}

/* What a draw may write, in guest pixels, box as nv2a_clear_box's: the
 * surface clip, clamped to a w x h target. The GPU scissors every draw to
 * the clip, and the CPU rasteriser bounds its spans by it. A GPU target
 * serves every clip of its surface (rt_grow), so a backend has to scissor
 * too: Burnout 3 draws its 640x448 frame under a clip at y 16 of a 640x480
 * buffer, and geometry running past the clip (its HUD panels, the menu
 * frame) otherwise lands in the 16 rows above and below, which no clear
 * covers, and stays there frame after frame. 0 when nothing is left. */
static inline int nv2a_draw_box(const struct nv2a_pb_gpu *g, uint32_t w, uint32_t h,
                                uint32_t box[4])
{
    uint32_t ax = g->clip_x, ay = g->clip_y;
    uint32_t bx = g->clip_x + g->clip_w, by = g->clip_y + g->clip_h;
    if (bx > w) bx = w;
    if (by > h) by = h;
    if (ax >= bx || ay >= by)
        return 0;
    box[0] = ax;
    box[1] = ay;
    box[2] = bx;
    box[3] = by;
    return 1;
}

/* A guest-pixel box on a w x h target made hw x hh host pixels
 * (render.scale): the host box covering the same pixels, x1/y1 exclusive. */
static inline void nv2a_host_box(const uint32_t box[4], uint32_t w, uint32_t h,
                                 uint32_t hw, uint32_t hh, uint32_t out[4])
{
    out[0] = (uint32_t)((uint64_t)box[0] * hw / w);
    out[1] = (uint32_t)((uint64_t)box[1] * hh / h);
    out[2] = (uint32_t)((uint64_t)box[2] * hw / w);
    out[3] = (uint32_t)((uint64_t)box[3] * hh / h);
}

static inline uint8_t nv2a_clamp255(int x)
{
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* One texel as A8R8G8B8 into *argb, from texel index u of the row at p
 * (linear formats; for a swizzled one p is the image base and u the Morton
 * index). P8 reads pal[0..pal_len), magenta for a missing palette or an
 * index past it. Returns 0, *argb untouched, for a format with no decoder. */
static inline int nv2a_tex_decode_texel(uint32_t fmt, const uint8_t *p,
                                        uint32_t u, const uint32_t *pal,
                                        uint32_t pal_len, uint32_t *argb)
{
    uint32_t t;
    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F:                                      /* LIN_A8B8G8R8 */
        t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    case 0x40:                                      /* LIN_B8G8R8A8 */
        t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (((t >> 8) & 0xFFu) << 16)
              | (((t >> 16) & 0xFFu) << 8) | ((t >> 24) & 0xFFu);
        return 1;
    case 0x41:                                      /* LIN_R8G8B8A8 */
        t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;

    /* 16-bit. */
    case 0x10:                                      /* LIN_A1R5G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x1C:                                      /* LIN_X1R5G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x11:                                      /* LIN_R5G6B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x3F, 6) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x1D:                                      /* LIN_A4R4G4B4 */
        t = ((const uint16_t *)p)[u];
        *argb = (d3d8_expand_channel((t >> 12) & 0xF, 4) << 24)
              | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 16)
              | (d3d8_expand_channel((t >> 4) & 0xF, 4) << 8)
              |  d3d8_expand_channel(t & 0xF, 4);
        return 1;

    /* 8-bit. */
    case 0x13:                                      /* LIN_L8 */
        t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;
    case 0x1B:                                      /* LIN_AY8: Y in all four */
        *argb = (uint32_t)p[u] * 0x01010101u;
        return 1;
    case 0x0B:                                      /* P8, through the palette */
        t = p[u];
        *argb = (pal && t < pal_len) ? pal[t] : NV2A_TEX_MAGENTA;
        return 1;

    /* 16-bit, the rest of the D3D8 set. */
    case 0x20:                                      /* LIN_A8Y8: Y low, A high */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | (t & 0xFFu) * 0x010101u;
        return 1;
    case 0x17:                                      /* LIN_G8B8 (and V8U8) */
        /* xemu's table loads it as GL_RG8 (low byte red, high byte green)
         * and swizzles to (R, G, R, G): so B and R are the low byte, G and
         * A the high one. A bump-map stage reads it back from b and g. */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | ((t & 0xFFu) << 16) | ((t >> 8) << 8)
              | (t & 0xFFu);
        return 1;
    case 0x16:                                      /* LIN_R8B8 */
        /* G8B8's sibling with the high byte in red instead of green: B low,
         * R high. xemu loads it as GL_RG8 swizzled (G, R, R, G) and Cxbx
         * expands it the same way, so R and A carry the high byte, G and B
         * the low one. */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | ((t >> 8) << 16) | ((t & 0xFFu) << 8)
              | (t & 0xFFu);
        return 1;
    case 0x35:                                      /* LIN_Y16: top byte */
        t = ((const uint16_t *)p)[u] >> 8;
        *argb = 0xFF000000u | t * 0x010101u;
        return 1;
    case 0x37:                                      /* LIN_R6G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 10) & 0x3F, 6) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x3D:                                      /* LIN_R5G5B5A1 */
        t = ((const uint16_t *)p)[u];
        *argb = ((t & 1u) ? 0xFF000000u : 0u)
              | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 6) & 0x1F, 5) << 8)
              |  d3d8_expand_channel((t >> 1) & 0x1F, 5);
        return 1;
    case 0x3E:                                      /* LIN_R4G4B4A4 */
        t = ((const uint16_t *)p)[u];
        *argb = (d3d8_expand_channel(t & 0xF, 4) << 24)
              | (d3d8_expand_channel((t >> 12) & 0xF, 4) << 16)
              | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 8)
              |  d3d8_expand_channel((t >> 4) & 0xF, 4);
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes: how a title hands over a
     * decoded video frame. The chroma pair is shared between an even texel
     * and the one after it, so the group is found by masking the bottom bit
     * of the index. BT.601, the coefficients the D3D8 upload path uses. */
    case 0x24:                                      /* YUY2 */
    case 0x25: {                                    /* UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        *argb = 0xFF000000u
              | ((uint32_t)nv2a_clamp255((298 * c + 409 * cv + 128) >> 8) << 16)
              | ((uint32_t)nv2a_clamp255((298 * c - 100 * cu - 208 * cv + 128) >> 8) << 8)
              |  (uint32_t)nv2a_clamp255((298 * c + 516 * cu + 128) >> 8);
        return 1;
    }

    default:
        return 0;
    }
}

/* Texels in levels 0..levels-1 of a w x h texture. */
size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels);

/* One swizzled or DXT level of w x h at mem into out (A8R8G8B8, row-major,
 * 0 for a texel with no decoder); returns the guest bytes it took, which is
 * where the next level starts. */
uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out);

/* A whole texture into out (nv2a_tex_texels() entries, A8R8G8B8): level 0
 * (w x h), then each further level packed after it. Swizzled and DXT
 * formats read `levels` levels from mem; a linear one reads one level at
 * `pitch` bytes per row. */
void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out);

/* ------------------------------------------------------------------------ */
/* Texture stages                                                            */
/* ------------------------------------------------------------------------ */

/* One texture stage as the registers program it. */
struct nv2a_stage {
    uint32_t offset, color, width, height, pitch;
    uint32_t addr_u, addr_v;    /* SET_TEXTURE_ADDRESS per axis, as sampled:
                                 * NV2A_ADDR_BORDER only with the border
                                 * colour as the border source */
    uint32_t border;            /* SET_TEXTURE_BORDER_COLOR, A8R8G8B8 */
    uint32_t palette, pal_len;  /* P8: resolved palette address, entries */
    uint32_t filter;            /* SET_TEXTURE_FILTER as written */
    uint32_t levels;            /* mip levels (1 for linear, or without
                                 * NV2A_STAGE_MIPS) */
    uint32_t lod_min, lod_max;  /* CONTROL0 LOD clamps, within levels    */
    uint32_t fmt_levels;        /* FORMAT's MIPMAP_LEVELS as stored (>= 1):
                                 * the memory layout, whatever is sampled */
    int      cube;              /* FORMAT CUBEMAP_ENABLE: six faces      */
    uint32_t dims;              /* FORMAT DIMENSIONALITY: 1, 2 or 3      */
    int      valid;
    int      on;        /* CONTROL0 enable as used (never-set stage 0 counts on) */
    int      mode;      /* 0 none, 1 2D, 4 pass-through, other: sampled as 2D */
    /* SET_TEXTURE_SET_BUMP_ENV_MAT (0x1B28..0x1B34), as stored: the four
     * slots hold 00, 01, 11, 10 (see nv2a_ps_consts.bump); then
     * BUMP_ENV_SCALE (0x1B38) and BUMP_ENV_OFFSET (0x1B3C). */
    float    bump[4], bump_scale, bump_offset;
    int      raw_mode;  /* the shader program's mode before the gating */
    /* Set by the GPU backend for each draw: a CUBEMAP_ENABLE texture whose
     * six-face span (nv2a_stage_cube_extent) overlaps a live render target.
     * Such a stage is not bound as a cube (nv2a_stage_cube_bind), so its
     * key, its shader and its bind all take the 2D read, the render
     * target's own where one starts at t->offset, rather than decode faces
     * from guest bytes the target has not written back. */
    int      cube_rt;
};

/* SET_TEXTURE_ADDRESS modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border,
 * 5 clamp (OpenGL's, taken as clamp to edge). */
#define NV2A_ADDR_BORDER       4u

#define NV2A_STAGE_MIPS        1u   /* count mip levels as xemu does */
#define NV2A_STAGE_P8_PALETTE  2u   /* P8 is valid only with a palette */
#define NV2A_STAGE_PITCH_LINEAR 4u  /* pitch 0 for a swizzled or DXT texture */

typedef uint32_t (*nv2a_dma_resolve_fn)(uint32_t offset);

/* SET_TEXTURE_PALETTE (0x1B20 + 0x40 * stage): DMA offset in bits 6-31,
 * length 256 >> bits 2-3 entries of A8R8G8B8. *pal is the resolved guest
 * address, 0 for none. */
static inline void nv2a_tex_palette_decode(uint32_t reg,
                                           nv2a_dma_resolve_fn resolve,
                                           uint32_t *pal, uint32_t *len)
{
    *pal = (reg & ~0x3Fu) ? resolve(reg & ~0x3Fu) : 0;
    *len = 256u >> ((reg >> 2) & 3u);
}

/* Stage n from its 16 registers r (SET_TEXTURE_OFFSET 0x1B00 + 0x40 * n
 * onwards) and their written flags, with the shader-stage program
 * (shader_set false: stage 0 2D, the rest off). mode is the program's mode,
 * 0 where it asks for a texture the stage cannot give (disabled or
 * invalid; pass-through, 4, needs none). */
void nv2a_stage_decode(int n, const uint32_t *r, const uint8_t *set,
                       int shader_set, uint32_t shader_prog,
                       nv2a_dma_resolve_fn resolve, unsigned flags,
                       struct nv2a_stage *t);

/* ------------------------------------------------------------------------ */
/* Vertex attributes                                                         */
/* ------------------------------------------------------------------------ */

/* SET_VERTEX_DATA_ARRAY_FORMAT type codes: NV2A_VTX_* (d3d8_vsh_parse.h). */

/* One attribute of `size` components of `type` at p as float4, missing
 * components (0,0,0,1). Returns 0 for a type with no decoder (out is then
 * (0,0,0,1)). Decoded the way xemu hands them to GL. */
static inline int nv2a_vtx_decode(uint32_t type, uint32_t size,
                                  const uint8_t *p, float out[4])
{
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case NV2A_VTX_D3DCOLOR:
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV2A_VTX_FLOAT:
        for (i = 0; i < size && i < 4; i++)
            memcpy(&out[i], p + 4 * i, 4);
        return 1;
    case NV2A_VTX_UBYTE:
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case NV2A_VTX_S1:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            float f;
            memcpy(&s, p + 2 * i, 2);
            f = (float)s / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case NV2A_VTX_S32K:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = (float)s;
        }
        return 1;
    case NV2A_VTX_CMP: {
        uint32_t w;
        int32_t x, y, z;
        memcpy(&w, p, 4);
        x = (int32_t)(w << 21) >> 21;
        y = (int32_t)(w << 10) >> 21;
        z = (int32_t)w >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Primitives and programs                                                   */
/* ------------------------------------------------------------------------ */

enum nv2a_topology {
    NV2A_TOPO_NONE = 0,
    NV2A_TOPO_POINTS,
    NV2A_TOPO_LINES,
    NV2A_TOPO_TRIANGLES
};

/* Point, line and triangle lists from any NV097 primitive (NV_PRIM_*):
 * the n indices at v, each rebased by -base, into out (room for 3 * n).
 * Returns the index count and sets *topo; 0 and NV2A_TOPO_NONE for an
 * unknown primitive. Strips keep their winding, loops close. */
uint32_t nv2a_prim_to_list(uint32_t prim, const uint16_t *v, uint32_t n,
                           uint32_t base, uint32_t *out,
                           enum nv2a_topology *topo);

/* Draw merging (nv2a_pb_exec.c): back-to-back batches of one triangle
 * primitive with no method between them are drawn as one triangle list.
 * Titles that draw every particle or sprite as its own 3- or 4-vertex
 * strip (BLiNX 1's snow: 18k batches a flip, most of them with nothing in
 * between) pay a backend draw call per strip otherwise. The list is
 * nv2a_prim_to_list of each batch in turn, so the triangles, their order,
 * winding and last vertex are the ones the batches would have drawn.
 *
 * A batch joins with up to NV2A_MERGE_BATCH_MAX indices; the list holds
 * NV2A_MERGE_LIST_MAX, and the chain's indices span at most
 * NV2A_MERGE_SPAN_MAX vertices, since a backend fetches the whole range
 * lo..hi of a draw. */
#define NV2A_MERGE_BATCH_MAX 256u
#define NV2A_MERGE_LIST_MAX  0x6000u
#define NV2A_MERGE_SPAN_MAX  0x1000u
struct nv2a_merge {
    uint32_t prim;        /* the chain's primitive (NV_PRIM_*) */
    uint32_t batches;     /* guest batches held, 0: none */
    uint32_t verts;       /* indices they carried */
    uint32_t raw_n;       /* the first batch, drawn as it came if it stays alone */
    uint16_t raw[NV2A_MERGE_BATCH_MAX];
    uint32_t n;           /* list indices, once batches >= 2 */
    uint16_t list[NV2A_MERGE_LIST_MAX];
    uint16_t lo, hi;      /* vertex range of the chain */
};
/* Whether a batch of n indices of prim may join a chain at all. */
int nv2a_merge_joinable(uint32_t prim, uint32_t n);
/* Add a joinable batch to m (batches 0 starts a chain). Returns 0, with m
 * unchanged, when it does not fit: another primitive, the list full or the
 * span too wide; the caller draws m and starts again. */
int nv2a_merge_add(struct nv2a_merge *m, uint32_t prim, const uint16_t *v, uint32_t n);

/* FNV-1a over the vertex program's slots, from v->start through the slot
 * with the FINAL bit (or the end). *len, if given, gets the slot count. */
uint32_t nv2a_vsh_program_hash(const struct nv2a_pb_vsh *v, uint32_t *len);

/* ------------------------------------------------------------------------ */
/* Depth, stencil, alpha test and blending                                   */
/* ------------------------------------------------------------------------ */

/* Compare functions in GL's order (SET_DEPTH_FUNC / SET_ALPHA_FUNC take GL's
 * 0x200 NEVER .. 0x207 ALWAYS, SET_STENCIL_FUNC its low bits), which is also
 * D3D11_COMPARISON_* - 1 and MTLCompareFunction's order. */
enum nv2a_cmp {
    NV2A_CMP_NEVER = 0, NV2A_CMP_LESS, NV2A_CMP_EQUAL, NV2A_CMP_LEQUAL,
    NV2A_CMP_GREATER, NV2A_CMP_NOTEQUAL, NV2A_CMP_GEQUAL, NV2A_CMP_ALWAYS
};

/* A GL compare code as nv2a_cmp; anything else tests as ALWAYS. */
static inline uint32_t nv2a_cmp_from_gl(uint32_t gl)
{
    return (gl >= 0x200 && gl <= 0x207) ? gl - 0x200 : NV2A_CMP_ALWAYS;
}

/* Whether `a cmp b` passes (a the incoming value, b the stored or ref). */
static inline int nv2a_cmp_test_u(uint32_t cmp, uint32_t a, uint32_t b)
{
    switch (cmp) {
    case NV2A_CMP_NEVER:    return 0;
    case NV2A_CMP_LESS:     return a <  b;
    case NV2A_CMP_EQUAL:    return a == b;
    case NV2A_CMP_LEQUAL:   return a <= b;
    case NV2A_CMP_GREATER:  return a >  b;
    case NV2A_CMP_NOTEQUAL: return a != b;
    case NV2A_CMP_GEQUAL:   return a >= b;
    default:                return 1;
    }
}

static inline int nv2a_cmp_test_f(uint32_t cmp, float a, float b)
{
    switch (cmp) {
    case NV2A_CMP_NEVER:    return 0;
    case NV2A_CMP_LESS:     return a <  b;
    case NV2A_CMP_EQUAL:    return a == b;
    case NV2A_CMP_LEQUAL:   return a <= b;
    case NV2A_CMP_GREATER:  return a >  b;
    case NV2A_CMP_NOTEQUAL: return a != b;
    case NV2A_CMP_GEQUAL:   return a >= b;
    default:                return 1;
    }
}

/* Stencil ops, in D3D11_STENCIL_OP_* - 1 (and MTLStencilOperation) order. */
enum nv2a_stencil_op {
    NV2A_SOP_KEEP = 0, NV2A_SOP_ZERO, NV2A_SOP_REPLACE, NV2A_SOP_INCR_SAT,
    NV2A_SOP_DECR_SAT, NV2A_SOP_INVERT, NV2A_SOP_INCR, NV2A_SOP_DECR
};

/* GL's stencil op codes; anything else keeps. */
static inline uint32_t nv2a_stencil_op_from_gl(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return NV2A_SOP_ZERO;
    case 0x1E01: return NV2A_SOP_REPLACE;
    case 0x1E02: return NV2A_SOP_INCR_SAT;
    case 0x1E03: return NV2A_SOP_DECR_SAT;
    case 0x150A: return NV2A_SOP_INVERT;
    case 0x8507: return NV2A_SOP_INCR;
    case 0x8508: return NV2A_SOP_DECR;
    default:     return NV2A_SOP_KEEP;
    }
}

/* The stencil value op makes of v (before the write mask). */
static inline uint32_t nv2a_stencil_op_apply(uint32_t op, uint32_t v,
                                             uint32_t ref)
{
    switch (op) {
    case NV2A_SOP_ZERO:     return 0;
    case NV2A_SOP_REPLACE:  return ref;
    case NV2A_SOP_INCR_SAT: return v < 255u ? v + 1u : 255u;
    case NV2A_SOP_DECR_SAT: return v ? v - 1u : 0u;
    case NV2A_SOP_INVERT:   return ~v;
    case NV2A_SOP_INCR:     return v + 1u;
    case NV2A_SOP_DECR:     return v - 1u;
    default:                return v;
    }
}

/* SET_SURFACE_FORMAT's zeta field: 1 = Z16, else Z24S8. The largest depth
 * value, and a CLEAR_SURFACE value as depth in [0,1]. */
static inline float nv2a_zmax(uint32_t surface_format)
{
    return ((surface_format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
}

static inline float nv2a_zclear_depth(uint32_t surface_format, uint32_t zclear)
{
    return ((surface_format >> 4) & 0xF) == 1
         ? (float)(zclear & 0xFFFFu) / 65535.0f
         : (float)(zclear >> 8) / 16777215.0f;
}

/* Blend factors (SET_BLEND_FUNC_SFACTOR / DFACTOR take GL's codes). */
enum nv2a_blend {
    NV2A_BF_ZERO = 0, NV2A_BF_ONE,
    NV2A_BF_SRC_COLOR, NV2A_BF_INV_SRC_COLOR,
    NV2A_BF_SRC_ALPHA, NV2A_BF_INV_SRC_ALPHA,
    NV2A_BF_DST_ALPHA, NV2A_BF_INV_DST_ALPHA,
    NV2A_BF_DST_COLOR, NV2A_BF_INV_DST_COLOR,
    NV2A_BF_SRC_ALPHA_SAT,
    NV2A_BF_CONST_COLOR, NV2A_BF_INV_CONST_COLOR,
    NV2A_BF_CONST_ALPHA, NV2A_BF_INV_CONST_ALPHA,
    NV2A_BF_UNKNOWN
};

static inline uint32_t nv2a_blend_from_gl(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return NV2A_BF_ZERO;
    case 0x0001: return NV2A_BF_ONE;
    case 0x0300: return NV2A_BF_SRC_COLOR;
    case 0x0301: return NV2A_BF_INV_SRC_COLOR;
    case 0x0302: return NV2A_BF_SRC_ALPHA;
    case 0x0303: return NV2A_BF_INV_SRC_ALPHA;
    case 0x0304: return NV2A_BF_DST_ALPHA;
    case 0x0305: return NV2A_BF_INV_DST_ALPHA;
    case 0x0306: return NV2A_BF_DST_COLOR;
    case 0x0307: return NV2A_BF_INV_DST_COLOR;
    case 0x0308: return NV2A_BF_SRC_ALPHA_SAT;
    case 0x8001: return NV2A_BF_CONST_COLOR;
    case 0x8002: return NV2A_BF_INV_CONST_COLOR;
    case 0x8003: return NV2A_BF_CONST_ALPHA;
    case 0x8004: return NV2A_BF_INV_CONST_ALPHA;
    default:     return NV2A_BF_UNKNOWN;
    }
}

/* The factor as used on the alpha channel: a _COLOR factor reads alpha. */
static inline uint32_t nv2a_blend_for_alpha(uint32_t bf)
{
    switch (bf) {
    case NV2A_BF_SRC_COLOR:     return NV2A_BF_SRC_ALPHA;
    case NV2A_BF_INV_SRC_COLOR: return NV2A_BF_INV_SRC_ALPHA;
    case NV2A_BF_DST_COLOR:     return NV2A_BF_DST_ALPHA;
    case NV2A_BF_INV_DST_COLOR: return NV2A_BF_INV_DST_ALPHA;
    default:                    return bf;
    }
}

/* A factor for the channel at bit `sh` of the ARGB words, 0..255, as the CPU
 * blender computes it. ponytail: SRC_ALPHA_SAT and unknown codes read as
 * INV_DST_COLOR here, as the CPU path always has. */
static inline uint32_t nv2a_blend_factor_u8(uint32_t bf, uint32_t src,
                                            uint32_t dst, uint32_t constant,
                                            uint32_t sh)
{
    switch (bf) {
    case NV2A_BF_ZERO:            return 0;
    case NV2A_BF_ONE:             return 255;
    case NV2A_BF_SRC_COLOR:       return (src >> sh) & 0xFF;
    case NV2A_BF_INV_SRC_COLOR:   return 255 - ((src >> sh) & 0xFF);
    case NV2A_BF_SRC_ALPHA:       return src >> 24;
    case NV2A_BF_INV_SRC_ALPHA:   return 255 - (src >> 24);
    case NV2A_BF_DST_ALPHA:       return dst >> 24;
    case NV2A_BF_INV_DST_ALPHA:   return 255 - (dst >> 24);
    case NV2A_BF_DST_COLOR:       return (dst >> sh) & 0xFF;
    case NV2A_BF_CONST_COLOR:     return (constant >> sh) & 0xFF;
    case NV2A_BF_INV_CONST_COLOR: return 255 - ((constant >> sh) & 0xFF);
    case NV2A_BF_CONST_ALPHA:     return constant >> 24;
    case NV2A_BF_INV_CONST_ALPHA: return 255 - (constant >> 24);
    case NV2A_BF_SRC_ALPHA_SAT: {         /* min(As, 1 - Ad); 1 for alpha */
        uint32_t sa = src >> 24, ida = 255 - (dst >> 24);
        return sh == 24 ? 255 : (sa < ida ? sa : ida);
    }
    default:                      return 255 - ((dst >> sh) & 0xFF);
    }
}

/* The constant blend factor an API with one RGBA constant needs, from
 * SET_BLEND_COLOR (ARGB) and the two factors: with only _ALPHA constants the
 * alpha is replicated into all four, a mix of _COLOR and _ALPHA keeps the
 * colour (*mixed set, for the caller to report). With no constant factor in
 * use, (1,1,1,1) and 0 is returned. */
int nv2a_blend_constant(int enable, uint32_t sfactor, uint32_t dfactor,
                        uint32_t color, float bf[4], int *mixed);

/* ------------------------------------------------------------------------ */
/* Fog                                                                       */
/* ------------------------------------------------------------------------ */

/* SET_FOG_MODE, decoded: func 0 linear, 1 exp, 2 exp2; abs_ for the _ABS
 * forms; edge_one when an infinite or NaN fog distance gives 1 (LINEAR,
 * LINEAR_ABS, EXP) rather than 0. Returns 0 for a mode that is not set. */
static inline int nv2a_fog_mode(uint32_t mode, uint32_t *func, uint32_t *abs_,
                                uint32_t *edge_one)
{
    switch (mode) {
    case 0x2601: *func = 0; *abs_ = 0; *edge_one = 1; return 1;  /* LINEAR */
    case 0x0804: *func = 0; *abs_ = 1; *edge_one = 1; return 1;  /* LINEAR_ABS */
    case 0x0800: *func = 1; *abs_ = 0; *edge_one = 1; return 1;  /* EXP */
    case 0x0802: *func = 1; *abs_ = 1; *edge_one = 0; return 1;  /* EXP_ABS */
    case 0x0801: *func = 2; *abs_ = 0; *edge_one = 0; return 1;  /* EXP2 */
    case 0x0803: *func = 2; *abs_ = 1; *edge_one = 0; return 1;  /* EXP2_ABS */
    default:     *func = 0; *abs_ = 0; *edge_one = 0; return 0;
    }
}

/* The fog factor for fog distance d (xemu vsh.c), with SET_FOG_PARAMS p0,
 * p1; 1 when fog is off or the mode is not set. */
static inline float nv2a_fog_factor(int enable, uint32_t mode, float p0,
                                    float p1, float d)
{
    uint32_t func, abs_, edge_one;
    int known;
    float f;

    if (!enable)
        return 1.0f;
    known = nv2a_fog_mode(mode, &func, &abs_, &edge_one);
    if (isinf(d))
        return edge_one ? 1.0f : 0.0f;
    if (!known)
        return 1.0f;
    if (func == 0)
        f = p0 + d * p1 - 1.0f;
    else if (func == 1)
        f = p0 + exp2f(d * p1 * 16.0f) - 1.5f;
    else
        f = p0 + exp2f(-d * d * p1 * p1 * 32.0f) - 1.5f;
    if (abs_)
        f = fabsf(f);
    if (isnan(f))
        return edge_one ? 1.0f : 0.0f;
    return f;
}

/* ------------------------------------------------------------------------ */
/* Shader constants                                                          */
/* ------------------------------------------------------------------------ */

static inline void nv2a_argb_to_float4(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

/* The combiner pixel shader's constants. This layout is the shader ABI any
 * emitter (HLSL now, MSL later) must honour: the same layout as
 * NV2APSConstants in d3d8_combiners.h, which nv2a_pb_d3d11.c checks field by
 * field with _Static_assert, and the cbuffer PS_CB_DECL declares there. */
struct nv2a_ps_consts {
    float    c0[8][4], c1[8][4], fc0[4], fc1[4], fog_color[4];
    float    alpha_ref;
    uint32_t alpha_func;        /* enum nv2a_cmp + 1: 1 NEVER .. 8 ALWAYS */
    uint32_t alpha_test_enable, fog_enable;
    uint32_t alpha_only[4];     /* 1: rgb reads 1 (A8); 2: alpha reads 1 (no alpha) */
    float    tex_scale[4][4];   /* xy: texcoord multiplier (1/size for linear);
                                 * z: nv2a_uv_snap (n, -n for 2D), 0 for none;
                                 * w: hor+ x offset into a padded target
                                 * (rt_offset), 0 otherwise */
    uint32_t tex_mode[4];       /* stage texture-shader mode, 0 = none */
    /* BUMPENVMAP stages: the bump matrix as the method slots hold it --
     * 00, 01, 11, 10, not the 00 01 10 11 their order suggests. The XDK's
     * texture-stage states run BUMPENVMAT00, 01, 11, 10 (22..25) and are
     * written to the slots in index order, and xemu's
     * SET_TEXTURE_SET_BUMP_ENV_MAT handler stores slot 2 in BUMPMAT11 and
     * slot 3 in BUMPMAT10 (its swizzle[]): du' = dot(dudv, bump.xw),
     * dv' = dot(dudv, bump.yz) -- and BUMPENVMAP_LUMINANCE's scale (x) and
     * offset (y). */
    float    bump[4][4];
    float    bumpls[4][4];
    /* CLIPPLANE (mode 5) stages: 0x10 | the stage's nibble of
     * SET_SHADER_CLIP_PLANE_MODE, else 0. Bit j set kills the pixel where
     * the interpolated coordinate's component j is >= 0, clear where it is
     * < 0 (stage_clip_kills). The flag is there because nibble 0 is a real
     * setting and tex_mode reads 0 for a CLIPPLANE stage, which binds no
     * texture: without it fs_basic could not tell the stage from NONE. */
    uint32_t clip_mode[4];
};

/* tex_scale[n].z for stage t drawn at render scale n: 0 when the shaders'
 * nv2a_snap_uv leave the sample at the host pixel, else how they move it to
 * its guest pixel's 1x sample point. Only a sampled (mode not 0 or 4),
 * bilinear-filtered stage at n > 1: there the N host pixels of a guest
 * pixel sample between texel centres, and near a picture's edge bilinear
 * blends in what lies past it -- a rect texture's padding, or through WRAP
 * the far edge of a tile (BLiNX 2's loading screen, swizzled DXT1 quads:
 * a light line at each tile edge). A point-filtered stage reads
 * one texel and never bleeds.
 *   flat2d 0 (a 3D batch, or a 2D one off the guest grid): n for a
 *   linear-format stage (coordinates in texels), whose shader test also
 *   asks for a unity blit; 0 for any other, so textured 3D surfaces keep
 *   their scaled detail.
 *   flat2d 1 (nv2a_snap_flat2d): -n for any format; the shader then asks
 *   only for an axis-aligned mapping (magnified or minified tiles, a
 *   constant u or v), and the batch samples exactly as it does at 1x. */
float nv2a_uv_snap(const struct nv2a_stage *t, unsigned n, int flat2d);

/* Every stage's .z (nv2a_uv_snap at n, the target's render scale) but the
 * render-target ones (rt_mask bit k): their texels are at host scale
 * already, and nv2a_ps_consts_rt leaves them 0. */
void nv2a_ps_consts_snap(struct nv2a_ps_consts *pc, const struct nv2a_stage st[4],
                         unsigned rt_mask, unsigned n, int flat2d);

/* Fill pc from the combiner registers and the four stages. A stage that
 * samples a render target instead of guest memory is then corrected with
 * nv2a_ps_consts_rt. */
void nv2a_ps_consts_fill(const struct nv2a_pb_vsh *v,
                         const struct nv2a_stage st[4],
                         struct nv2a_ps_consts *pc);

/* SET_SHADER_OTHER_STAGE_INPUT: the earlier stage whose texel a dependent
 * read in stage n (BUMPENVMAP, ...) takes. Stage 1 always reads stage 0;
 * stages 2 and 3 name theirs in bits 16-19 and 20-23, as xemu's
 * input_tex[]. An input at or past n is a title bug: xemu asserts, and it
 * is clamped to n - 1 here so the shader still builds. 0 for stage 0. */
static inline int nv2a_stage_input(int n, uint32_t other_stage_input)
{
    uint32_t in;
    if (n <= 1)
        return 0;
    in = (other_stage_input >> (n == 2 ? 16 : 20)) & 0xFu;
    return in < (uint32_t)n ? (int)in : n - 1;
}

/* The texture-shader dot-product family (xemu psh.c, PS_TEXTUREMODES 8 to
 * 0x12) and the stages each mode may sit in: a mode outside its stages is a
 * title bug the hardware would not run, and is sampled as 2D instead.
 * DOTPRODUCT (0x11), DOT_ZW (0x0A), BRDF (8) and DOT_RFLCT_SPEC_CONST
 * (0x12) sample nothing: they read 0, as xemu's. */
static inline int nv2a_dot_mode_ok(uint32_t m, int n)
{
    switch (m) {
    case 0x08: case 0x09: case 0x0A: return n >= 2;   /* BRDF, DOT_ST, DOT_ZW */
    case 0x0B: return n == 2;                          /* DOT_RFLCT_DIFF */
    case 0x0C: case 0x0D: case 0x0E: case 0x12:
        return n == 3;      /* DOT_RFLCT_SPEC, DOT_STR_3D, DOT_STR_CUBE, _SPEC_CONST */
    case 0x0F: case 0x10: return n >= 1;               /* DPNDNT_AR, DPNDNT_GB */
    case 0x11: return n == 1 || n == 2;                /* DOTPRODUCT */
    default: return 0;
    }
}

/* Modes that look up a cube map with a 3D vector: CUBEMAP, DOT_RFLCT_DIFF,
 * DOT_RFLCT_SPEC and DOT_STR_CUBE. */
static inline int nv2a_mode_cube(uint32_t m)
{
    return m == 3 || m == 0x0B || m == 0x0C || m == 0x0E;
}

/* Stage n's texture goes up as a real cube map: a cube-sampling mode over a
 * CUBEMAP_ENABLE texture that is bound (t->mode) and square, as every
 * NV2A cube face is. Both GPU backends bind it as a cube, and their shaders
 * declare it so (NV2ACombinerState.tex_cube); any other stage over a cube
 * texture samples its face 0 as 2D. */
static inline int nv2a_stage_cube_bind(const struct nv2a_stage *t, uint32_t ps_mode)
{
    return t->mode && t->cube && !t->cube_rt && t->width == t->height
        && nv2a_mode_cube(ps_mode);
}

/* Bytes from one face of t's cube map to the next, by FORMAT's level count
 * (the memory layout, whatever is sampled), and the guest span of all six
 * faces as t samples them. */
static inline uint32_t nv2a_stage_face_stride(const struct nv2a_stage *t)
{
    return nv2a_tex_face_stride(t->color, t->width, t->height, t->pitch, t->fmt_levels);
}
static inline uint32_t nv2a_stage_cube_extent(const struct nv2a_stage *t)
{
    uint32_t face = nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
    return face ? 5u * nv2a_stage_face_stride(t) + face : 0u;
}

/* t's cube faces overlap a render target of `bytes` bytes at rt_addr: the
 * backend's test for nv2a_stage.cube_rt, over each live target. */
static inline int nv2a_stage_cube_over_rt(const struct nv2a_stage *t, uint32_t rt_addr,
                                          uint32_t bytes)
{
    return t->mode && t->cube && t->offset
        && nv2a_rt_overlap(rt_addr, bytes, t->offset, nv2a_stage_cube_extent(t));
}

/* Stage n's dot mapping (SET_DOT_RGBMAPPING): 0 for stage 0, which has no
 * dot-product mode. */
static inline int nv2a_stage_dot_map(int n, uint32_t dot_rgbmapping)
{
    return n > 0 ? (int)((dot_rgbmapping >> (4 * (n - 1))) & 7u) : 0;
}

/* The pixel shader's mode for stage n of a backend that generates the
 * texture shader: NONE, PASSTHRU and CLIPPLANE (5, from raw_mode) stay; 6
 * and 7 stay from stage 1 on (stage 0 has no earlier stage to read); the
 * dot-product family stays in its stages (nv2a_dot_mode_ok), from raw_mode,
 * since a DOTPRODUCT stage reads no texture and titles leave it disabled
 * (Quantum Redshift's water); CUBEMAP stays over a cube it can bind
 * (nv2a_stage_cube_bind); every other mode is sampled as 2D. Both GPU
 * backends key their shaders on it. */
static inline uint32_t nv2a_stage_ps_mode(const struct nv2a_stage *t, int n)
{
    uint32_t m = (uint32_t)t->mode;
    if (t->raw_mode == 5)
        return 5;   /* CLIPPLANE reads 0, not NONE's (0, 0, 0, 1) */
    if (nv2a_dot_mode_ok((uint32_t)t->raw_mode, n))
        return (uint32_t)t->raw_mode;
    if ((m == 6 || m == 7) && n > 0)
        return m;
    if (m == 3 && nv2a_stage_cube_bind(t, m))
        return 3;
    return m && m != 4 ? 1u : m;
}

/* The texture-shader half of a GPU backend's pixel-shader key: *modes
 * holds nv2a_stage_ps_mode, 5 bits a stage; *extra the input stage of each
 * stage that reads one (BUMPENVMAP and the dot-product family), 4 bits a
 * stage from bit 0, the dot mappings of stages 1-3, 4 bits each from bit
 * 16 (nv2a_stage_dot_map), and from bit 28 a bit a stage bound as a cube
 * map (nv2a_stage_cube_bind). Stage 3's input and map go in too under a
 * DOT_RFLCT_DIFF stage 2, whose third dot product they make whatever stage
 * 3's own mode. The backend unpacks *extra into the combiner state's
 * tex_input, dot_map and tex_cube. */
static inline void nv2a_ps_stage_key(const struct nv2a_stage st[4], uint32_t other_stage_input,
                                     uint32_t dot_rgbmapping, uint32_t *modes, uint32_t *extra)
{
    int n;
    *modes = 0;
    *extra = 0;
    for (n = 0; n < 4; n++) {
        uint32_t m = nv2a_stage_ps_mode(&st[n], n);
        int diff3 = n == 3 && (*modes >> 10 & 31u) == 0x0Bu;
        if (m >= 6 || diff3)
            *extra |= (uint32_t)nv2a_stage_input(n, other_stage_input) << (4 * n);
        if ((m >= 8 && n > 0) || diff3)
            *extra |= (uint32_t)nv2a_stage_dot_map(n, dot_rgbmapping) << (16 + 4 * (n - 1));
        if (nv2a_stage_cube_bind(&st[n], m))
            *extra |= 1u << (28 + n);
        *modes |= m << (5 * n);
    }
}

/* RECOMP_TRACE=bump: one line on stderr the first time a batch shows a
 * distinct BUMPENVMAP configuration (stage, mode, input stage, the matrix as
 * stored, scale, offset, the stage's and the input's format and size), with
 * the flip. The caller checks the key. Printing only. */
void nv2a_bump_trace(const struct nv2a_pb_vsh *v, const struct nv2a_stage st[4],
                     uint32_t flip);

/* Stage n samples a w x h render target instead of guest memory: linear
 * coordinates are texels of the surface as drawn (1/size), and no snap,
 * since a scaled target holds host-rate detail the 1x rule would discard. */
void nv2a_ps_consts_rt(struct nv2a_ps_consts *pc, int n,
                       const struct nv2a_stage *t, uint32_t w, uint32_t h);

/* A screen-space vertex coordinate on the NV2A rasteriser's grid: 4 fraction
 * bits, truncated (xemu's roundScreenCoords). XDK D3D adds 0.53125 to every
 * position so that, after this, a D3D pixel-aligned edge lands exactly on a
 * host pixel centre and the top-left rule keeps the first row and column. A
 * host rasteriser with finer sub-pixel precision kept the 1/32 and drew every
 * full-screen pass one pixel right and down. The MSL and HLSL epilogues and
 * the Metal and D3D11 pass-through shaders apply the same expression.
 *
 * trunc, not floor, on purpose: a negative coordinate rounds toward zero
 * (-0.53125 -> -0.5, -0.03 -> 0), as xemu's roundScreenCoords does. Geometry
 * that starts left of or above the surface keeps the same edge the hardware
 * gives it; floor would move it a sixteenth out and change which column the
 * top-left rule includes. */
static inline float nv2a_screen_snap(float p)
{
    return truncf(p * 16.0f) / 16.0f;
}

/* The vertex stage's viewport and fog constants: c = 2/w, -2/h, 1/zmax, 0,
 * -1, 1, 0, 0, fog params 0..2, 0; u = fog on, fog func, fog abs, fog
 * edge-one, specular on, light_ctl bit 17 (separate specular), 0, 0.
 *
 * `pad` is the hor+ padding of the target (nv2a_host_pad), guest pixels on
 * each side: with it the x mapping is 2/(w + 2 pad), -w/(w + 2 pad), so the
 * title's columns 0..w land centred in the wider target and a perspective
 * batch's geometry beyond them is drawn instead of clipped. 0 is stock. */
struct nv2a_vp_consts {
    float    c[12];
    uint32_t u[8];
};

void nv2a_vp_consts_fill(const struct nv2a_pb_vsh *v, uint32_t surface_format,
                         uint32_t w, uint32_t h, uint32_t pad,
                         struct nv2a_vp_consts *o);

/* ------------------------------------------------------------------------ */
/* Visibility-test reports (GET_REPORT)                                      */
/* ------------------------------------------------------------------------ */

/* A report's 16 bytes at guest va: timestamp (two words), count, status.
 * The count is stored before the status, with a fence between, so a title
 * polling the status from another core never reads a stale count. */
void nv2a_report_write(uint8_t *mem, uint32_t va, uint32_t count);

/* Mark a report pending: status 0xFFFFFFFF, which GetVisibilityTestResult
 * reads as D3DERR_TESTINCOMPLETE. */
void nv2a_report_set_pending(uint8_t *mem, uint32_t va);

/* API-independent bookkeeping for occlusion queries.
 *
 * Each stretch with SET_ZPASS_PIXEL_COUNT_ENABLE on is one query (begun at
 * enable, or at a clear while enabled; ended at disable or the report), and
 * a report sums the stretches since the last clear. Reports go on a pending
 * list and are polled; one still pending after NV2A_OCC_LATE_US, or with
 * more than NV2A_OCC_SEG stretches, completes as visible (NV2A_OCC_VISIBLE).
 * A GET_REPORT naming a va that still has one pending supersedes it: the old
 * queries go back unread, and nothing is written for the old test. The
 * backend supplies the queries through nv2a_occ_ops; handles are opaque.
 *
 * Settled before the title can read (nv2a_occ_settle): a report still pending
 * there is written complete with the last completed count for its address,
 * or NV2A_OCC_VISIBLE when the address has none younger than
 * NV2A_OCC_LATE_US. The executor has the backend settle (its on_poll) at the
 * end of each walk, before GET is stored, and before a semaphore release
 * that follows a GET_REPORT; the backend settles again at the flip
 * (nv2a_occ_flip). On the console the GPU writes a test's count ahead of
 * anything later in the stream, so the title never reads one incomplete;
 * BLiNX 2's LOAD GAME screen reads its two tests 4-12 ms after their
 * GET_REPORT, in the same frame, and skips its characters on an incomplete
 * one. A host GPU slower than that lost them on every late frame.
 * The title can read once the walker hands back GET or a semaphore it waits
 * on, so those are the last points before a read; the flip alone is too
 * late. Before settling, the tracker waits for the results, up to settle_us
 * a frame in all (NV2A_OCC_SETTLE_US; 0: no wait), so a host that keeps up
 * gives exact counts and one far behind pays at most that per frame. 8 ms,
 * as Metal's stage1 results often land 2-8 ms after the walk and D3D11's
 * within 2 ms; the wait ends when they do. The address's last count is what
 * a lagging GPU can give: at most one frame stale when the title reuses one
 * address a frame. A settled report keeps its queries on retired[]; when the
 * late result comes in it updates the address's last count and amends the
 * report: the true count replaces the settled one, but only while the slot
 * still holds what the settle wrote (status 0, the same count) and no later
 * GET_REPORT has named the address. A slot the title has reset for its next
 * test stays reset. Burnout 3 needs the amend: it rotates its sun
 * test over ten report slots and reads each one frames after its GET_REPORT,
 * as the console allows, so with no amend it read the settle's value, and at
 * the start of every sun shot (no count for those slots within
 * NV2A_OCC_LATE_US) that was NV2A_OCC_VISIBLE, hundreds of times the sun's
 * true count: the lens-flare rings drew at full strength. A slot with no
 * count of its own within NV2A_OCC_LATE_US settles to the youngest count of
 * a slot beside it (within NV2A_OCC_NEAR) before falling back to visible, so
 * a read that comes before the amend sees the previous flip's sun, not
 * NV2A_OCC_VISIBLE. B3 reads some slots before the late result lands, so
 * the amend alone still left 148 flare frames in the delayed A/B where the
 * borrow brings it to 3 (one with no delay): the borrow carries most of the
 * fix. It applies only to one test rotating: when another report within
 * NV2A_OCC_NEAR is pending in the same settle or was issued in the same
 * flip, the slots hold different objects at once (JSRF's lights, SSX 3's
 * flares), a neighbour's count says nothing about this one, and the
 * report settles to visible as before.
 * One window is accepted: a title that reset the slot for frame N+1 before
 * frame N's report settles reads frame N's count where the console would
 * read incomplete; the next GET_REPORT makes it pending again.
 *
 * Everything here runs on the NV2A ack thread; the title's reads see a
 * report through nv2a_report_write()'s release store. */
#define NV2A_OCC_SEG      8         /* stretches per report */
#define NV2A_OCC_PENDING  128
#define NV2A_OCC_VISIBLE  0x10000u
#define NV2A_OCC_LATE_US  250000u
#define NV2A_OCC_LAST     64        /* report addresses with a last count */
#define NV2A_OCC_AGES     8         /* result-age histogram buckets */
#define NV2A_OCC_SETTLE_US 8000u    /* settle wait budget a frame, default */
#define NV2A_OCC_NEAR     0x100u    /* "beside": report slots this close */

struct nv2a_occ_ops {
    void *(*get)(void *ctx);                        /* NULL if none */
    void  (*put)(void *ctx, void *q);               /* back to the pool */
    void  (*begin)(void *ctx, void *q);
    void  (*end)(void *ctx, void *q);
    /* 1 and *count when q's result is in; flush: may push the GPU. */
    int   (*result)(void *ctx, void *q, int flush, uint64_t *count);
    uint64_t (*now_us)(void *ctx);
    void *ctx;
};

struct nv2a_occ_report {
    uint32_t va;
    int      n, overflow;
    uint64_t t0;
    uint64_t flip;                      /* o->flips at GET_REPORT */
    /* Retired: the count the settle wrote, and whether a late result may
     * still amend the report (no GET_REPORT at va since). */
    uint32_t settled;
    int      amend;
    void    *q[NV2A_OCC_SEG];
};

/* The last completed count at a report address, guest size (after the
 * render-scale division), and the push time and flip of the report it came
 * from, so a late result never replaces a newer one. */
struct nv2a_occ_last {
    uint32_t va, count;
    uint64_t t0, flip;
};

struct nv2a_occ {
    const struct nv2a_occ_ops *ops;
    uint8_t *mem;                       /* guest memory base */
    void    *seg[NV2A_OCC_SEG];         /* since the last clear */
    int      nseg, open, en, ovf;
    struct nv2a_occ_report pend[NV2A_OCC_PENDING];
    int      npend;
    /* Settled, their queries still out: a result updates last[] and amends
     * the report (see above). The oldest is dropped unread when full. */
    struct nv2a_occ_report retired[NV2A_OCC_PENDING];
    int      nret;
    struct nv2a_occ_last last[NV2A_OCC_LAST];
    int      nlast, last_next;          /* filled, next to replace when full */
    /* For the flip log: reports, completed visible (count != 0, fallbacks
     * and settles included), late fallbacks, superseded, and microseconds
     * waited in sync mode. */
    uint64_t reports, vis, late, super, sync_us;
    /* Settled (stale: with the address's last count; beside: with the
     * youngest count of a slot beside it; first: no count yet, visible),
     * retired results dropped unread, and microseconds waited
     * before settling (settle_us), in all and in the current frame. */
    uint64_t settled, stale, first, ret_drop, settle_wait_us, frame_wait_us;
    /* Settled reports a late result amended. */
    uint64_t amended, beside;
    /* nv2a_occ_flip calls, and the age (GET_REPORT to the poll that found
     * it) of every result taken: <1, <2, <4, <8, <16, <33, <66, >=66 ms. */
    uint64_t flips, age[NV2A_OCC_AGES];
    /* Render scale of the targets counted (0 or 1 = guest size): a count
     * is samples, N*N of them per guest pixel, so it is divided back. */
    unsigned scale;
    /* Test hook: a result is not taken before it is delay_us old. The
     * settles of one frame wait up to settle_us in all for results (0: no
     * wait; NV2A_OCC_SETTLE_US from nv2a_occ_init). */
    uint64_t delay_us, settle_us;
    /* RECOMP_TRACE=zpass: print the first 64 settles and, in the summary,
     * the age histogram. */
    int      trace;
    unsigned trace_n;
};

void nv2a_occ_init(struct nv2a_occ *o, const struct nv2a_occ_ops *ops,
                   uint8_t *mem);
void nv2a_occ_zpass(struct nv2a_occ *o, int enable);
void nv2a_occ_zpass_clear(struct nv2a_occ *o);
/* GET_REPORT at va; sync waits (up to NV2A_OCC_LATE_US) for the result. */
void nv2a_occ_report(struct nv2a_occ *o, uint32_t va, int sync);
void nv2a_occ_poll(struct nv2a_occ *o, int flush);
/* Take every result that is in (with a flush), wait for the rest while the
 * frame's settle_us lasts, then settle each report still pending (see
 * above). After it nothing is pending. With nothing pending it only polls
 * retired[], without a flush. The backends call it from on_poll. */
void nv2a_occ_settle(struct nv2a_occ *o);
/* The same at the flip, once the frame is submitted; counts the flip and
 * starts the next frame's wait budget. */
void nv2a_occ_flip(struct nv2a_occ *o);
/* nv2a_occ_init takes the scale from nv2a_host_opts(); this overrides it.
 * Retired reports are dropped unread: a result counted at the old scale and
 * divided at the new one would be off by the square of the ratio. Call
 * nv2a_occ_settle first, so nothing pending is read at the new scale either. */
void nv2a_occ_set_scale(struct nv2a_occ *o, unsigned scale);
void nv2a_occ_set_delay(struct nv2a_occ *o, uint64_t us);
void nv2a_occ_set_settle(struct nv2a_occ *o, uint64_t us);
void nv2a_occ_set_trace(struct nv2a_occ *o, int on);
/* The running totals under `tag` ("[D3D11]"), and with trace the age
 * histogram: what a run states of its GPU latency. */
void nv2a_occ_summary(const struct nv2a_occ *o, const char *tag);
/* A summed sample count at `scale` as the guest-size count: / scale^2,
 * rounded, a non-zero count staying >= 1 (visible never turns hidden),
 * saturated to 32 bits. */
uint32_t nv2a_occ_scale_count(uint64_t samples, unsigned scale);

/* ---- Host options: enhancements the backends and the presenter honour ----
 *
 * Filled once by the opt-in enhancements layer (src/enhance, xbox_enhance_init)
 * before the guest starts. All zero is stock: guest-size targets, nearest
 * letterbox, windowed. Without the layer nothing sets them. After init the
 * struct is written only on the walker thread, by nv2a_host_opts_apply() at a
 * flip: the backends read it in many places, and a change between a target's
 * size and its scale mid-frame would be a torn frame. */
enum nv2a_present_filter {
    NV2A_PRESENT_NEAREST = 0,   /* stock: letterbox, nearest */
    NV2A_PRESENT_LINEAR,        /* letterbox, bilinear */
    NV2A_PRESENT_INTEGER        /* largest whole multiple that fits, centred */
};

/* present.surface, resolved (never "auto"). Read when the window is made,
 * so it has no NV2A_OPT_* bit: the menu cannot change it. */
enum nv2a_present_surface {
    NV2A_PRESENT_SURFACE_WINDOW = 0,    /* stock: the swap chain follows the window */
    NV2A_PRESENT_SURFACE_INTERNAL       /* swap chain = the frame; the compositor scales */
};

struct nv2a_host_opts {
    unsigned render_scale;      /* 0 or 1 = guest size; 2..4 */
    unsigned aniso;             /* 0 = stock; 2..16: anisotropic filtering
                                 * on mipmapped textures */
    int      present_filter;    /* enum nv2a_present_filter */
    int      fullscreen;        /* borderless desktop fullscreen at start */
    unsigned pack_off;          /* NV2A_PACK_OFF_* bits: 0 = every kind on */
    int      present_surface;   /* enum nv2a_present_surface */
    /* display.aspect as a ratio, 0:0 = stock (4:3, no padding): 4:3-shaped
     * and square targets get hor+ padding (nv2a_host_pad). */
    unsigned aspect_w, aspect_h;
    int      movie_mode;        /* enum nv2a_movie_mode: display.movie */
    unsigned overscan;          /* display.overscan: guest px cropped from each
                                 * side edge at present, 0 = stock (nv2a_present_crop) */
    int      snap_2d;           /* render.snap_2d: above scale 1, 2D batches sample
                                 * on the guest grid (nv2a_snap_flat2d); 0 = off */
};

/* pack_off bits: the whole pack, or one kind of entry (nv2a_tex_pack.h). */
#define NV2A_PACK_OFF_ALL   0x1u
#define NV2A_PACK_OFF_FONT  0x2u
#define NV2A_PACK_OFF_TEXT  0x4u
#define NV2A_PACK_OFF_ART   0x8u

/* Field masks for nv2a_host_opts_request / _apply. ANISO, MOVIE and
 * OVERSCAN need nothing from a backend at the flip: the samplers are keyed
 * on the aniso value, the movie placement is read at each movie draw and the
 * crop at each present, so the stored value is the whole change. SNAP2D
 * is read at each draw (nv2a_snap_flat2d) the same way. */
#define NV2A_OPT_SCALE      0x1u
#define NV2A_OPT_FILTER     0x2u
#define NV2A_OPT_FULLSCREEN 0x4u
#define NV2A_OPT_PACK       0x8u
#define NV2A_OPT_ANISO      0x10u
#define NV2A_OPT_MOVIE      0x20u
#define NV2A_OPT_OVERSCAN   0x40u
#define NV2A_OPT_SNAP2D     0x80u
/* display.overscan's range (enhance.c clamps the key to it): 10% of a
 * 640-wide frame a side, past any TV's overscan. */
#define NV2A_OVERSCAN_MAX   64u

void nv2a_host_opts_set(const struct nv2a_host_opts *o);
const struct nv2a_host_opts *nv2a_host_opts(void);
/* Queue a change of the fields in `mask` (any thread); the backend applies it
 * at its next flip. A second request before that flip merges into the first,
 * the later value winning per field. */
void nv2a_host_opts_request(const struct nv2a_host_opts *o, unsigned mask);
/* At the flip, on the walker thread: apply the pending request. Returns the
 * mask of fields whose value changed (0 when nothing was pending, which costs
 * one atomic load). A PACK change also sets the pack's enable mask and bumps
 * nv2a_tex_pack_gen(), so every backend's texture cache re-uploads. Values
 * are taken as xbox_enhance_init takes them: aniso below 2 is off and above
 * 16 is 16, overscan above NV2A_OVERSCAN_MAX is that, and a movie mode out
 * of range or a non-zero overscan with a wide display.aspect is refused
 * with one line (the latter's 4:3 edges are inside the padded frame). */
unsigned nv2a_host_opts_apply(void);
/* Bumped by every apply that changed a field, after the fields are stored:
 * a reader on another thread (the SDL presenter) that sees a new gen with
 * acquire sees the new values. */
uint32_t nv2a_host_opts_gen(void);
/* render_scale as a factor: 1 when unset. */
unsigned nv2a_host_render_scale(void);

/* The stages of a draw that a later stage reads as its input
 * (OTHER_STAGE_INPUT): BUMPENVMAP(_LUMINANCE)'s offsets, the dot-product
 * family's normal and DEPENDENT_AR/GB's coordinates (every mode from 8 on),
 * and stage 3's under a DOT_RFLCT_DIFF stage 2 (nv2a_ps_stage_key); as bits
 * 1 << n. 0 for a draw without register combiners, whose basic shader
 * samples every stage as plain 2D. combiner is v->rc_set && (v->rc_ctl &
 * 0xFF). */
unsigned nv2a_bump_sources(const struct nv2a_stage st[4], int combiner,
                           uint32_t other_stage_input);
/* render.aniso's max anisotropy for stage t's sampler, 0 = off: only a
 * mipmapped texture that filters on minification (min_linear), is not a cube
 * map and is not a bump source (bump_source: its bit in nv2a_bump_sources).
 * Textures without mips are render targets read back (bloom, blur), linear-format movie
 * frames and lookup tables, screen-aligned or data, which shimmer under it. A
 * bump source's texels are offsets, not colour: at stock its mips average a
 * ripple map to ~0 at a distance, so a far sea is calm; anisotropic filtering
 * keeps the sharp ripple at grazing angles and scatters the dependent lookups
 * into noise (BLiNX 2's stage 1 sea). Both GPU backends key their samplers
 * on the result. */
unsigned nv2a_stage_aniso(const struct nv2a_stage *t, int mip, int min_linear,
                          int bump_source);
/* Hor+ padding of a guest w x h colour target, guest pixels on each side:
 * 0 unless display.aspect is set and the target is 4:3 within 1% (the frame
 * and its half-size post buffers), then round((h * aw/ah - w) / 2): 107 for
 * 640x480 at 16:9 (an 854-wide target), 53 for 320x240. A square target
 * (screen-space reflections) is widened by the same ratio plus w/8 of
 * overscan for bumped reads, round(w * (3aw / 4ah - 1) / 2 + w / 8): 115
 * for 512x512 at 16:10, 149 at 16:9. Other shapes keep theirs. */
uint32_t nv2a_host_pad(uint32_t w, uint32_t h);
/* The ratio's share of nv2a_host_pad, without a square's overscan: what a
 * stretched pass spans (nv2a_aspect_map, nv2a_aspect_box,
 * nv2a_rt_stretch_read), so a full-width copy between the frame and a
 * square keeps lining up. Equal to nv2a_host_pad for other shapes. */
uint32_t nv2a_host_pad_ratio(uint32_t w, uint32_t h);

/* Width of the window for a guest w x h frame shown `scale` times larger:
 * the padded target's, (w + 2 nv2a_host_pad) * scale, so a 16:9 window
 * shows the pads 1:1 (854 for 640x480) instead of letterboxing them into a
 * 4:3 window. Even, since the pad is doubled; w * scale at stock. */
uint32_t nv2a_host_window_w(uint32_t w, uint32_t h, unsigned scale);

/* Hor+ placement of one batch on a padded target (display.aspect). The
 * title's 4:3 frame sits centred in the wider target; each batch is mapped
 * one of these ways (nv2a_aspect_map) and scissored to match
 * (nv2a_aspect_box). */
enum nv2a_aspect_place {
    NV2A_PLACE_CENTRE = 0,  /* 3D and 2D: columns 0..w centred, x + pad */
    NV2A_PLACE_STRETCH,     /* a full-width screen pass, or a movie under
                             * display.movie=fit: 0..w over the whole target */
    NV2A_PLACE_COVER        /* display.movie=fill: 0..w over the whole width,
                             * y scaled the same about the centre (cropped) */
};

/* display.movie (nv2a_host_opts). */
enum nv2a_movie_mode { NV2A_MOVIE_BLACK = 0, NV2A_MOVIE_FIT, NV2A_MOVIE_FILL };

/* A 2D batch whose vertices span lo_x..hi_x (surface pixels) on a w-wide
 * target covers the whole guest width: a fade, a post quad, a letterbox
 * bar, a movie frame. 2 pixels of slack each side. */
static inline int nv2a_aspect_full_width(float lo_x, float hi_x, uint32_t w)
{
    return lo_x <= 2.0f && hi_x >= (float)w - 2.0f;
}

/* Whether a guest texture under a full-width 2D batch is a picture (a
 * movie frame, a still, a menu backdrop) for nv2a_aspect_place's guest_tex.
 * One at most a quarter of the frame's width is magnified 4x or more over
 * it: a flat colour or a gradient, which is a fade (BLiNX 2 fades through
 * a 16x16 white texture, every frame of a stage), and a fade is stretched
 * like an untextured one so the pads fade too. Nor is a bump stage's
 * offset map (bump_src: the stage is in nv2a_bump_sources), whatever its
 * size: JSRF's Can boost warps its blurred square over the frame through a
 * 256 wide one. */
static inline int nv2a_aspect_picture(uint32_t tex_w, uint32_t w, int bump_src)
{
    return !bump_src && tex_w * 4u > w;
}

/* Whether a program vertex's oPos.w puts it on a screen layer. oPos.xy
 * are surface pixels whatever w is (w only steers perspective-correct
 * interpolation and the depth), so a batch is 2D when its vertices share
 * one w. Orthographic output has w 1; BLiNX 2 also draws parts of its
 * stage HUD (the status panel's frame, its red lamps) through the world's
 * projection at a fixed view depth, every vertex at w 2 or 3, and its
 * title movie at w 100. Those are exact whole floats. A world sprite also
 * shares one w, but a computed depth lands exactly on a whole number about
 * once in 10^4 or less below 1024 (float spacing there is 6e-5 or finer),
 * so a layer is an exact whole w from 1 to 1024. */
static inline int nv2a_aspect_layer_w(float w)
{
    return w >= 1.0f && w <= 1024.0f && w == floorf(w);
}

/* How a batch is placed. two_d: the batch is 2D (pass-through, or every
 * vertex at one nv2a_aspect_layer_w); full_w: nv2a_aspect_full_width;
 * guest_tex: a stage samples a guest texture (not a render target), and
 * nv2a_aspect_picture says it is one; flat_rt: a stage samples a render
 * target without pads, one neither 4:3 nor square (a 512x256 shadow or
 * blur buffer; squares are padded, nv2a_host_pad). A full-width
 * pass that samples only padded render targets, or nothing, is stretched
 * (its source is padded too, or it is a flat fade); one over an unpadded
 * target is centred, since stretching it would widen a 4:3 picture over
 * the padded frame. One over a guest texture is a movie or a still and
 * keeps its shape unless display.movie says otherwise. Everything else is
 * centred, the HUD included: it keeps its 4:3 layout. */
static inline int nv2a_aspect_place(int two_d, int full_w, int guest_tex, int flat_rt,
                                    int movie_mode)
{
    if (!two_d || !full_w)
        return NV2A_PLACE_CENTRE;
    if (!guest_tex)
        return flat_rt ? NV2A_PLACE_CENTRE : NV2A_PLACE_STRETCH;
    return movie_mode == NV2A_MOVIE_FIT ? NV2A_PLACE_STRETCH
         : movie_mode == NV2A_MOVIE_FILL ? NV2A_PLACE_COVER : NV2A_PLACE_CENTRE;
}

/* The guest_tex that nv2a_aspect_place takes: a guest picture counts only
 * when the batch samples no padded render target (padded_rt). A pass that
 * reads the padded frame is a post pass, whatever else it binds: BLiNX 1's
 * glow downsamples the padded 640x480 frame into a padded 320x240 target
 * with a 640x480 guest mask on stage 1. Centred as a "movie", it filled
 * only the small target's guest columns, its pads stayed black, and the
 * composite brightened the 4:3 centre alone, a step at the old edges. */
static inline int nv2a_aspect_place_tex(int guest_tex, int padded_rt)
{
    return guest_tex && !padded_rt;
}

/* Hor+: the placement of a 2D batch that samples a frame copy (copy_rt: a
 * padded render target a 2D-engine blit made and nothing has drawn into
 * since). Such a batch is a lens effect over the frame, not HUD art:
 * Quantum Redshift's raindrops refract the copy of the back buffer each one
 * lies over. Centred, its texture reads were the copy's guest
 * columns while the frame under it spans the pads, and every drop was
 * scissored to the 4:3 centre. Stretched, it covers the wide frame and
 * reads the whole padded copy, so a drop at guest x sits where it reads:
 * each drop is (w + 2 pad) / w wider (1.33x at 16:9) and the drops reach
 * the wings. The HUD never samples a blit copy and keeps its placement. */
static inline int nv2a_aspect_place_copy(int place, int two_d, int copy_rt)
{
    return place == NV2A_PLACE_CENTRE && two_d && copy_rt ? NV2A_PLACE_STRETCH : place;
}

/* Whether a 2D batch reads a frame copy where it lies: each of its n
 * sampled vertices (screen xy, that stage's texcoord uv) maps to guest
 * pixels by about the identity. su, sv take uv to guest texels (1 for a
 * linear texture's texel coordinates, its size for a swizzled one's
 * normalised ones). The offset uv * s - xy may drift across the batch by
 * an eighth of the batch's extent plus 2 pixels (a drop magnifies what it
 * lies over a little), and its mean stays within a sixteenth of the w x h
 * frame (a refraction shifts it a little). Quantum Redshift's raindrops do.
 * A batch that lays the copy elsewhere or at another scale
 * (a picture in picture, a mirror, a blur pass into a smaller target)
 * reads pixels the stretch would not move with it, and keeps its place.
 * 0 for no vertices. */
int nv2a_aspect_uv_tracks(const float (*xy)[2], const float (*uv)[2], uint32_t n, float su,
                          float sv, uint32_t w, uint32_t h);

/* Whether a batch's stages take nv2a_uv_snap's 2D form: the batch is 2D
 * (pass-through, or a program on one screen layer, nv2a_aspect_scan), so
 * its mapping is affine and textured 3D is left alone, and it is centred,
 * so host pixel p lies in guest pixel floor(p / n): the hor+ offset is the
 * pad, a whole number of guest pixels. A stretch or cover maps guest
 * pixels to fractional host spans, where there is no 1x point to snap to.
 * The backends pass two_d 0 for a program batch whose vertices do not
 * share one oPos.z (nv2a_aspect_gate's vary_z). */
static inline int nv2a_snap_flat2d(int two_d, int place)
{
    return two_d == 1 && place == NV2A_PLACE_CENTRE;
}

/* Whether a target drawn at render scale n gets the 2D snap at all:
 * render.snap_2d (default off) above scale 1. Off, the backends neither
 * scan for 2D batches without hor+ nor touch tex_scale.z, so every scale
 * draws as it did before the 2D form: only the movie-style unity blits
 * snap (nv2a_uv_snap with flat2d 0). The snap trades host-rate smoothing
 * of magnified 2D art for 1x-exact edges (no seams between WRAP tiles):
 * a player choice, toggled live from the menu. */
static inline int nv2a_snap_2d_on(unsigned n)
{
    return n > 1 && nv2a_host_opts()->snap_2d;
}

/* Hor+: how a batch that is not stretched reads a padded render target.
 * The target's guest columns sit pad in from the left of w + 2 pad, so a
 * read at u (0..1 over the stock target) is taken to u * scale + off. A
 * read past 0..1 lands in the pads: a screen-space reflection sampled from
 * the frame's pad finds the mirrored pass's pad, since both are padded by
 * the same share of their width (nv2a_host_pad). */
static inline void nv2a_rt_guest_read(uint32_t w, uint32_t pad, float *scale, float *off)
{
    float pw = (float)(w + 2u * pad);
    *scale = (float)w / pw;
    *off = (float)pad / pw;
}

/* Hor+: how a stretched batch reads a padded w x h render target: across
 * its ratio's pad (nv2a_host_pad_ratio), as a stretch draws into it, so the
 * whole target except a square's overscan columns. Returns 0 when that is
 * the whole target (nothing to change). */
static inline int nv2a_rt_stretch_read(uint32_t w, uint32_t h, uint32_t pad, float *scale,
                                       float *off)
{
    uint32_t r = nv2a_host_pad_ratio(w, h);
    float pw = (float)(w + 2u * pad);

    if (!r || r >= pad)
        return 0;
    *scale = (float)(w + 2u * r) / pw;
    *off = (float)(pad - r) / pw;
    return 1;
}

/* Per vertex program: whether its batches are 2D. Every BLiNX 2 batch is a
 * program (the XDK's fixed function is one too), so the program's own
 * output decides: every vertex on one screen layer (nv2a_aspect_layer_w)
 * after an orthographic transform, a depth per vertex after a perspective
 * one. The interpreter costs about a draw, so it is
 * gated: two_d is -1 until a batch is checked; a program whose first 16
 * batches were all 3D is checked every 256th batch after (the same program
 * with an orthographic matrix is caught then, late); one seen 2D once is
 * checked every batch. */
struct nv2a_aspect_gate {
    int      two_d;
    uint32_t n;
    /* After a scan that returned 1: the scanned vertices did not share one
     * oPos.z. Hor+ ignores it; render.snap_2d leaves such a batch alone
     * (nv2a_snap_flat2d), since an orthographic 3D mesh (a menu model at
     * w 1) is on one layer too, and snapping only its axis-aligned faces
     * would mix snapped and smooth faces in one mesh. */
    int      vary_z;
};

/* Run a program batch's vertices through the interpreter (nv2a_vsh_run) if
 * the gate says so. vf holds count vertices of nf4 float4 inputs each,
 * input k of a vertex going to register slot[k]; c is the constant file.
 * With xy (count entries) every vertex runs and its x, y land there
 * (for nv2a_aspect_full_tris); without, up to 33 evenly
 * spaced. Returns 1 for a 2D batch with its x range in *lo, *hi (surface
 * pixels, as the epilogue leaves them), 0 for 3D or not checked. */
/* Whether the next nv2a_aspect_scan through this gate runs the program, so
 * a backend that has to gather the inputs first skips that when it won't. */
int nv2a_aspect_due(const struct nv2a_aspect_gate *gate);

/* Under Hor+ or render.snap_2d: whether a batch may join a merged list
 * (nv2a_merge), its placement being one no set of its own vertices could
 * change. A program batch whose program's first 16 batches were all 3D
 * (gate two_d 0) is centred and never snapped whatever its vertices; with
 * the depth test on it is no flat near layer (nv2a_aspect_flat) nor a
 * letterbox band (nv2a_aspect_band); sampling no render target it is no
 * screen quad (nv2a_aspect_screen_quad). Everything else (pass-through,
 * a program not yet settled or seen 2D, a batch reading a target) is
 * placed from its own vertices and drawn alone. The walker asks at every
 * batch's END: a batch answering 0 flushes the chain held so far and draws
 * alone, so a chain only ever holds batches that each answered 1. With
 * merging, the gate's n counts draws, not batches, so its every-256th
 * recheck of a settled 3D program comes later in guest batches, and scans
 * the merged list: a program that turns orthographic is caught later, and
 * a merged list whose vertices span several depths sets vary_z, so the
 * snap leaves that one draw alone. Merging used to stop altogether under
 * Hor+, so wide views drew every guest batch on its own. */
static inline int nv2a_merge_placed(int program, int depth_enable,
                                    const struct nv2a_aspect_gate *gate, int samples_rt)
{
    return program && depth_enable && gate && gate->two_d == 0 && gate->n >= 16 && !samples_rt;
}

int nv2a_aspect_scan(struct nv2a_aspect_gate *gate,
                     const uint32_t prog[][4], uint32_t start, float c[][4],
                     const float *vf, unsigned nf4, const int *slot, uint32_t count,
                     float (*xy)[2], float *lo, float *hi);

/* A screen quad through a perspective program: count (at most
 * NV2A_ASPECT_QUAD_MAX) vertices that all share one positive oPos.w, run
 * without the gate, their x, y in xy and x range in *lo, *hi. JSRF's Can
 * boost renders the world into 512 and 128 squares and lays them over the
 * frame with a quad of four vertices at one w that is not a whole number
 * (0.836, 1.413), from a program that also draws the world, so neither
 * nv2a_aspect_layer_w nor the gate catches it. The caller takes it as 2D
 * only when its stages pass nv2a_aspect_quad_ok, its x range
 * nv2a_aspect_quad_span and one triangle nv2a_aspect_full_tris: a sprite
 * in the world shares one w too, but it does not read the frame or a
 * square across exactly the frame's width. Returns 1 for a quad of one
 * positive finite w, its positions in xy and x range in lo..hi. */
#define NV2A_ASPECT_QUAD_MAX 6u

/* Whether a batch's stages let it be a screen quad: it samples a padded
 * render target (padded_rt) and nothing nv2a_aspect_place would centre for
 * (flat_rt, guest_tex). */
static inline int nv2a_aspect_quad_ok(int padded_rt, int flat_rt, int guest_tex)
{
    return padded_rt && !flat_rt && !guest_tex;
}

/* Whether a screen quad's x range lo..hi covers the w-wide frame and no
 * more (2 pixels of slack each side). A pass laid over the frame is
 * exactly frame-sized (JSRF's spans 0.53..640.53); a camera-facing quad in
 * the world (a haze plane, a near sprite) that overhangs both edges is not
 * one and keeps its place. */
static inline int nv2a_aspect_quad_span(float lo_x, float hi_x, uint32_t w)
{
    return nv2a_aspect_full_width(lo_x, hi_x, w) && lo_x >= -2.0f && hi_x <= (float)w + 2.0f;
}
int nv2a_aspect_screen_quad(const uint32_t prog[][4], uint32_t start, float c[][4],
                            const float *vf, unsigned nf4, const int *slot, uint32_t count,
                            float (*xy)[2], float *lo, float *hi);

/* Up to max of a batch's count vertices, spread evenly: their screen xy
 * and stage's texcoord (s, t) into uv, for nv2a_aspect_uv_tracks. prog
 * NULL is a pass-through batch, read straight from vf (position in input
 * 0, the texcoord in 9 + stage); otherwise the program runs on each. The
 * number sampled, 0 when the stage has no texcoord input or the program
 * fails. */
#define NV2A_ASPECT_UV_MAX 32u
uint32_t nv2a_aspect_uv_sample(const uint32_t prog[][4], uint32_t start, float c[][4],
                               const float *vf, unsigned nf4, const int *slot, uint32_t count,
                               unsigned stage, float (*xy)[2], float (*uv)[2], uint32_t max);

/* The thirds of a w x h frame a letterbox band lies in (nv2a_aspect_third). */
#define NV2A_BAND_TOP 1u
#define NV2A_BAND_BOT 2u

/* Which third a screen quad (n vertices at xy, finite, within the frame's
 * height with 2 pixels of slack) lies wholly in: NV2A_BAND_TOP,
 * NV2A_BAND_BOT or 0. The bottom third, not quarter: Burnout 3's crash-cam
 * list backing has a raised right end that starts at y 349. */
unsigned nv2a_aspect_third(const float (*xy)[2], uint32_t n, uint32_t h);

/* A target's band bars: stamp[t] is the flip (plus one, 0 for never) in
 * which a full-width screen quad lying in third t (nv2a_aspect_third) was
 * stretched on it. nv2a_aspect_bars gives the thirds stamped this flip or
 * the last, so a piece drawn before its bar in a flip still finds it. */
static inline void nv2a_aspect_bar_mark(uint32_t stamp[2], unsigned third, uint32_t flips)
{
    if (third & NV2A_BAND_TOP)
        stamp[0] = flips + 1u;
    if (third & NV2A_BAND_BOT)
        stamp[1] = flips + 1u;
}
static inline unsigned nv2a_aspect_bars(const uint32_t stamp[2], uint32_t flips)
{
    return (stamp[0] && flips + 1u - stamp[0] <= 1u ? NV2A_BAND_TOP : 0u)
           | (stamp[1] && flips + 1u - stamp[1] <= 1u ? NV2A_BAND_BOT : 0u);
}

/* Whether a screen quad (nv2a_aspect_screen_quad, n vertices at xy) drawn
 * with the depth test off, over no guest picture or unpadded target, is a
 * piece of a letterbox band on a w x h frame: it keeps within the frame's
 * width, reaches its left or right edge, and lies in a third that holds a
 * stretched bar (bars, nv2a_aspect_bars). Such a piece is stretched with
 * the band. Burnout 3's crash cam draws its bars, their thin edge lines
 * and the list backing as quads at one fractional w (24.37) from the
 * program that draws its HUD: each bar spans the frame, but each line is
 * two half-width quads drawn apart and the backing samples a 32x32
 * texture, so only the bars passed as full-width passes and the rest
 * stayed 4:3 inside them. A stretch is one map about the frame's centre,
 * so pieces that met at stock still meet. Without the bar the rule takes
 * the front end's menu frame too: its header strips and bottom rule reach
 * an edge at the same w and broke away into the pads. Text and icons
 * inside the band reach neither edge and keep their place, unless they
 * sample a texture at most a quarter of the frame's width
 * (nv2a_aspect_picture) and touch an edge; lens flares likewise keep their
 * place unless one touches an edge inside a band. */
int nv2a_aspect_band(const float (*xy)[2], uint32_t n, uint32_t w, uint32_t h, unsigned bars);

/* Whether one triangle of a 2D triangle list (ntri triangles, idx into xy)
 * spans the w-wide frame (nv2a_aspect_full_width). A batch's bounds are not
 * enough: BLiNX 2 draws its blue corner triangles, one in each corner, as
 * one batch whose bounds are the whole screen, and stretching it would
 * widen them a third. A full-screen quad, a fade, a post pass or a movie
 * frame has triangles that span the width. */
int nv2a_aspect_full_tris(const float (*xy)[2], const uint32_t *idx, uint32_t ntri,
                          uint32_t w);

/* Whether w is a near layer's (nv2a_aspect_flat): under 1 and the
 * reciprocal of a whole number from 2 to 1024. A game that draws a screen
 * layer in front of everything writes a literal reciprocal w (rhw): Burnout
 * 3's front end draws all of its menus at rhw 10, w 0.1. Its race tags the
 * rival cars with their place ("3rd") through the same program at the
 * car's depth, every vertex of a tag at one w that drifts from 0.54 to 1;
 * a tag rides its car into the pads and must not be cut at the old 4:3
 * edge, and it can't be: no 1/n lies between 0.5 and 1, so the range,
 * not rarity, keeps the tags out. Further off, a computed depth landing
 * within a few ulps of a 1/n is rare, as nv2a_aspect_layer_w's whole
 * numbers are. The known limit: camera-locked geometry drawn without depth
 * at a round view distance (w 0.5, 0.25, 0.1, often the near plane), such
 * as a full-screen flash or tint quad or a lens or speed-lines overlay,
 * passes too. Such a quad that overhangs the 4:3 frame on purpose is cut
 * at the old 4:3 edge, and shows as a half-tinted pad. */
static inline int nv2a_aspect_flat_w(float w)
{
    float n;

    if (!(w > 0.0f && w < 1.0f))
        return 0;
    n = floorf(1.0f / w + 0.5f);
    return n >= 2.0f && n <= 1024.0f && fabsf(w * n - 1.0f) <= 1e-6f;
}

/* The step between the vertices nv2a_aspect_flat samples from a count-
 * vertex batch: all of them under 64, about 32 evenly spaced beyond. It
 * samples at most NV2A_ASPECT_FLAT_MAX vertices (63, at step 1; 48 at
 * most from step 2), so a backend that gathers the sample first passes
 * at most that many and the helper takes them all. */
#define NV2A_ASPECT_FLAT_MAX 63u
static inline uint32_t nv2a_aspect_flat_step(uint32_t count)
{
    return count > 32 ? count / 32 : 1;
}

/* Whether a program batch drawn without the depth test is a flat near
 * layer: every sampled vertex (nv2a_aspect_flat_step: all of them under
 * 64, about 32 evenly spaced beyond) at one w that passes
 * nv2a_aspect_flat_w. Such a batch keeps its centred place but is clipped
 * like a 2D one (nv2a_aspect_box's two_d), to the guest columns. Burnout
 * 3's front end draws its menu grid and selection lines through the
 * program that draws its HUD, at w 0.1: not a whole number, so not 2D
 * (nv2a_aspect_layer_w), and the grid's right edge overhangs the frame to
 * x 656 and the lines to -271..911, which stock hides past the screen's
 * edge. Clipped as 3D, the overhang reached into the pads as a strip at
 * the old 4:3 edge, and menu text sliding in from off screen (x -235..-5)
 * showed there too. A world batch has a depth per vertex, or, a camera-
 * facing sprite, flare or tag, one computed w, and still reaches the pads.
 * Returns 1 for a flat batch and its w in *w (when w is not NULL). */
int nv2a_aspect_flat(const uint32_t prog[][4], uint32_t start, float c[][4], const float *vf,
                     unsigned nf4, const int *slot, uint32_t count, float *w);

/* Adjust vertex constants filled with `pad` (nv2a_vp_consts_fill) to a
 * placement. CENTRE leaves them; with pad 0 every placement is stock. */
void nv2a_aspect_map(struct nv2a_vp_consts *o, uint32_t w, uint32_t h, uint32_t pad,
                     int place);

/* The tie shift, vp_off.zw (c[6], c[7]), of constants already mapped
 * (nv2a_vp_consts_fill, nv2a_aspect_map) onto a hw x hh host viewport.
 *
 * A screen coordinate that the snap leaves on a guest pixel centre (a
 * fraction of exactly 0.5, the XDK's 0.53125 bias on a D3D pixel-aligned
 * edge, nv2a_screen_snap) is a tie the NV2A's top-left rule settles: the
 * first row or column is kept, the last dropped. Scaled by s host pixels a
 * guest pixel, the same edge cuts that guest pixel's host block, and the
 * host pixels before it stay uncovered: every full-screen pass drawn at
 * 0.5 left host row and column 0 (render.scale 2 and up) or column 0
 * (a hor+ stretch at 1x, 768/640 moving 0.5 to 0.6) showing what lay
 * under it, the "1 px line" on the top and left. Moving a tie by
 * 0.5 - 0.25 / s guest pixels puts it a quarter host pixel into its block,
 * between two host centres rather than on one, so the host covers exactly
 * the guest pixels the NV2A does without a host tie of its own (s need not
 * be whole: 1.2 under the stretch). An edge off the centres keeps its
 * place, and so does any vertex with w != 1: the shaders move only flat
 * screen-space ones, so perspective 3D edges never step (shimmer, cracks
 * at T-junctions). Scales within 1% of 1 are stock, both shifts 0, so 1x
 * 4:3 is unchanged. */
void nv2a_vp_tie_fill(struct nv2a_vp_consts *o, uint32_t hw, uint32_t hh);

/* The host scissor of a batch placed `place` on a guest w x h target padded
 * by pad and made n times larger (host (w + 2 pad) n x h n), from its guest
 * surface-clip box (nv2a_draw_box). The clip box is in the batch's own
 * coordinates, so it moves with the placement. A centred 2D batch keeps to
 * the guest columns: a HUD piece slid off-screen (x < 0) must not show in
 * the pads. A centred 3D batch's clip edges at 0 and w reach out to the
 * target's edges, so the world fills the pads. A stretched or covering one
 * scales with the mapping (a stretch over the ratio's pad only,
 * nv2a_host_pad_ratio). pad 0 gives nv2a_host_box's result. lift
 * (nv2a_aspect_hud_lift) boxes a centred 2D batch like a 3D one. */
void nv2a_aspect_box(const uint32_t box[4], uint32_t w, uint32_t h, uint32_t pad,
                     unsigned n, int place, int two_d, int lift, uint32_t out[4]);

/* ---- The HUD layer (src/enhance/hud_anchor.h) as the backends see it ----
 * While a HUD mod is active (its table registered, the mod on, the pad
 * above 0) the layer shifts and clips its elements in guest memory, so on
 * the canvas's target the backends lift the 4:3 clip of centred 2D and
 * bound the full-width test. With the flag clear everything is as stock. */

/* The layer sets its flag and its canvas's target size (guest pixels). */
void nv2a_aspect_hud_set(int on, uint32_t tw, uint32_t th);
/* 1 while the flag is set and a w x h target is the canvas's target. */
int nv2a_aspect_hud_on(uint32_t w, uint32_t h);
/* The lift input of nv2a_aspect_box for one batch on a rt_w x rt_h
 * target: 1 only while the layer is on that target, for a 2D batch that
 * is not a flat near layer (Burnout 3's menus stay clipped), not full width
 * (fades, backings and movies keep their H5 box) and not wholly inside the
 * guest columns x_lo..x_hi in [0, rt_w] (no clip to lift). Pass 0..rt_w
 * when the batch's x range is not known. */
int nv2a_aspect_hud_lift(uint32_t rt_w, uint32_t rt_h, int two_d, int flat, int full_w,
                         float x_lo, float x_hi);
/* The full-width input of the placement (nv2a_aspect_place): the batch's
 * bounds lo_x..hi_x span the w-wide frame (nv2a_aspect_full_width) and,
 * for a triangle list (xy and idx not NULL, ntri triangles), one triangle
 * does (nv2a_aspect_full_tris). While the layer is on this w x h target a
 * triangle counts only when it is not past both -NV2A_HUD_WIDE_BAND and
 * w + NV2A_HUD_WIDE_BAND guest pixels: the layer's stretch map is
 * symmetric and puts its sprites past both on purpose, and H5 must not
 * stretch them again. An oversized full-screen triangle (0..2w, KUF's
 * menu resolve) passes one edge only and stays full width; a symmetric
 * overscan draw still passes both and is cleared. Without a list the
 * whole batch must lie inside both bounds (one edge proves nothing of a
 * mixed batch). *wide (may be NULL) is 1 when that bound alone cleared
 * the batch's full width. */
#define NV2A_HUD_WIDE_BAND 8.0f
int nv2a_aspect_full(float lo_x, float hi_x, const float (*xy)[2], const uint32_t *idx,
                     uint32_t ntri, uint32_t w, uint32_t h, int *wide);
/* nv2a_aspect_full_tris counting only a triangle not past both lo and hi
 * (its min x >= lo or its max x <= hi). */
int nv2a_aspect_full_tris_in(const float (*xy)[2], const uint32_t *idx, uint32_t ntri,
                             uint32_t w, float lo, float hi);
/* The presented frame's guest size: a backend publishes it at each flip,
 * the layer reads it for a canvas whose target is the frame. 0 before the
 * first flip. */
void nv2a_aspect_frame_publish(uint32_t w, uint32_t h);
int nv2a_aspect_frame(uint32_t *w, uint32_t *h);

/* Host size of a guest w x h target at `scale`: w*N x h*N, N lowered for
 * this target until both fit max_dim. Returns the N used (>= 1). */
unsigned nv2a_host_size(uint32_t w, uint32_t h, unsigned scale, uint32_t max_dim,
                        uint32_t *hw, uint32_t *hh);

/* Where a src_w x src_h frame goes in a dst_w x dst_h output (device
 * pixels). NEAREST/LINEAR: the largest rectangle of the frame's aspect that
 * fits, centred; an axis short of the output by under 1/256 takes the whole
 * axis (a hor+ frame's whole-column pad rounding) unless it is already 1:1.
 * INTEGER: the largest whole multiple that fits, centred; if 1x does not
 * fit, the NEAREST/LINEAR fit. Returns 1 when the result is the integer
 * placement, 0 for a fit (the caller filters a downscaled fit linearly). A
 * zero size gives an empty rectangle. */
struct nv2a_rect { int32_t x, y, w, h; };
int nv2a_present_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int filter, struct nv2a_rect *r);

/* present.shader: the rectangle the chain fills, the one the stock present
 * shows on the screen. Under present.surface=window that is
 * nv2a_present_rect's; under internal the stock frame goes whole to the
 * compositor (gamescope, or the Metal layer), which fits it to the output,
 * so INTEGER fits there too instead of leaving a 1x frame in a corner of
 * the output-sized surface the chain draws to. surface_internal:
 * the surface the config asked for, not the one the chain switched to. */
int nv2a_present_chain_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                            int filter, int surface_internal, struct nv2a_rect *r);

/* present.shader: the chain's input for a frame of host size hw x hh at
 * render scale n. A preset that draws one scanline per source line (lines:
 * a CRT) gets the guest's line count, hh / n rows of the whole host width,
 * each the mean of its n host rows: fed the host rows it drew 2 or 3
 * scanlines per guest line, finer than the output's rows at 800 or 1080,
 * and they beat into bands. Anything else (fsr), scale 1, and a
 * height that is not a multiple of n (no whole guest line per n rows) get
 * the host frame as it is. The one owner of that rule: a backend resolves
 * exactly when *sh < hh. */
void nv2a_present_chain_source(uint32_t hw, uint32_t hh, unsigned n, int lines,
                               uint32_t *sw, uint32_t *sh);

/* display.overscan: the part of a frame of host size fw x fh (render scale
 * n, hor+ pad guest px) that a crop of ov guest px a side shows, in a
 * texture of tw x th. The top and bottom lose ov * fh / fw so the picture
 * keeps its shape, a TV's overscan zoom. x/y/w/h are host px, the u_ and
 * v_ terms map the presenter's 0..1 quad onto that part of the texture,
 * and fit_w x fit_h is what to hand nv2a_present_rect: the whole frame for
 * NEAREST/LINEAR (the crop has its aspect exactly; a rounded cropped size
 * would leave a one-column bar), the cropped size for INTEGER, whose cut is
 * rounded to whole host rows so whole multiples exist. ov 0, a pad (hor+
 * refuses the key) or a frame no wider than 2 * ov guest px gives the whole
 * frame. */
struct nv2a_crop {
    float x, y, w, h;
    float u_scale, v_scale, u_off, v_off;
    uint32_t fit_w, fit_h;
};
void nv2a_present_crop(uint32_t fw, uint32_t fh, uint32_t tw, uint32_t th, unsigned n,
                       unsigned pad, unsigned ov, int filter, struct nv2a_crop *c);

/* 32-bit pixels: nearest N x upscale of a w x h image (src_pitch bytes a
 * row) into dst (w*n x h*n, dst_pitch bytes a row), and the box filter back:
 * each n x n block of src (w*n x h*n) averaged per byte channel, rounded,
 * into dst (w x h). */
void nv2a_upscale_nearest32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                            unsigned n, uint8_t *dst, uint32_t dst_pitch);
void nv2a_downscale_box32(const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                          unsigned n, uint8_t *dst, uint32_t dst_pitch);

/* w BGRA8 host pixels (every target's texture format) as a row of guest
 * surface in SET_SURFACE_FORMAT's colour format: R5G6B5; X1R5G5B5 with X 0
 * (Z1R5G5B5) or 1 (O1R5G5B5); the X8R8G8B8 pair with the alpha byte 0 (Z)
 * or 0xFF (O); X1A7R8G8B8 with X 0 or 1 over the top 7 bits of the host
 * alpha, the layout xemu's clear gives them; A8R8G8B8 and A8B8G8R8 as they
 * are. The constant bits are what the title reads back from such a
 * surface, not what the host drew. in and out may be the same buffer for
 * the 32-bit formats. The bytes written a pixel (2 or 4), 0 when the
 * format has no encoding here (out untouched); w 0 with NULL buffers asks
 * only that. */
uint32_t nv2a_surf_pack_row(uint32_t format, const uint8_t *in, uint32_t w, uint8_t *out);

/* ------------------------------------------------------------------------ */
/* Batch filter (enhancements layer only)                                    */
/* ------------------------------------------------------------------------ */

/* A predicate the enhancements layer's mods install (src/enhance/mod.h,
 * batch_filter) to drop whole batches, such as a HUD hidden for a photo.
 * Both GPU backends ask it at the top of on_draw, after their init, so
 * Metal and D3D11 drop the same batches; the executor has already counted
 * the batch and noted its surface, so the flip still presents what it
 * would. The CPU rasteriser does not ask.
 *
 * With XBOXRECOMP_ENHANCE off this compiles to nothing. On, with no filter
 * installed, the cost is one load and one well-predicted branch per batch;
 * nothing is computed and nothing changes. Installed before the guest
 * starts (mod init) and removed at shutdown, while the ack thread may still
 * be drawing: the pointer is stored with release and loaded with acquire.
 *
 * A skipped batch inside a visibility test (GET_REPORT) adds no samples, so
 * the test reads it as occluded: a title that hides a lens flare or a sprite
 * behind such a test hides it too when its bounding draw is filtered. */
struct nv2a_batch_info {
    uint32_t program_hash;  /* nv2a_vsh_program_hash; 0 for a fixed-function batch */
    int      program;       /* the batch runs the vertex program */
    uint32_t prim;          /* NV_PRIM_*; NV_PRIM_TRIANGLES for a merged chain */
    uint32_t idx_count;     /* a merged chain's list (nv2a_merge), not one batch's */
    uint32_t color_offset;  /* SET_SURFACE_COLOR_OFFSET, as the title set it */
    uint32_t surface_w, surface_h;   /* the clip's extent: clip_x + clip_w, clip_y + clip_h */
    uint32_t surface_format;         /* SET_SURFACE_FORMAT */
    int      depth_test, depth_write, stencil_test, alpha_test;
    int      blend;                  /* BLEND_ENABLE */
    uint32_t blend_sfactor, blend_dfactor;   /* raw GL codes */
    uint32_t color_keep;    /* SET_COLOR_MASK inverted: 0 writes every channel */
    uint32_t flip;          /* flips completed so far */
    /* The colour surface the last flip presented (nv2a_pb_present_state),
     * 0 before the first. A title swaps two or three back buffers, so a
     * batch drawing into one of them has color_offset equal to the offset
     * one or two flips ago; a filter that wants "into a back buffer, not an
     * offscreen target" keeps the offsets it has seen here. */
    uint32_t present_offset;
    const struct nv2a_pb_draw *draw;   /* the batch itself */
};

/* Returns 1 to drop the batch. */
typedef int (*nv2a_batch_filter_fn)(const struct nv2a_batch_info *b);

#ifdef XBOXRECOMP_ENHANCE
extern nv2a_batch_filter_fn nv2a_batch_filter_installed;
/* Install (NULL removes) the filter. One: the mod table composes its own. */
void nv2a_batch_filter_set(nv2a_batch_filter_fn fn);
/* The slow half of nv2a_batch_skip: fills the info and asks the filter. */
int nv2a_batch_filter_ask(const struct nv2a_pb_draw *d, const struct nv2a_pb_gpu *g,
                          const struct nv2a_pb_vsh *v);
#endif

/* 1 when the installed filter drops this batch. The backends pass the
 * executor's state they already hold. */
static inline int nv2a_batch_skip(const struct nv2a_pb_draw *d, const struct nv2a_pb_gpu *g,
                                  const struct nv2a_pb_vsh *v)
{
#ifdef XBOXRECOMP_ENHANCE
    if (__atomic_load_n(&nv2a_batch_filter_installed, __ATOMIC_ACQUIRE))
        return nv2a_batch_filter_ask(d, g, v);
#endif
    (void)d; (void)g; (void)v;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Flip hook (enhancements layer only)                                      */
/* ------------------------------------------------------------------------ */

/* Called once per flip on the ack thread, after the backend's on_flip, for
 * the host actions' tick (src/enhance/enhance_actions.c): a title that stops
 * polling its pad (a loading screen) still opens the menu. Installed only
 * when a feature key arms the actions; with none installed the cost is one
 * load and a branch per flip, and with XBOXRECOMP_ENHANCE off nothing. */
typedef void (*nv2a_flip_hook_fn)(void);

#ifdef XBOXRECOMP_ENHANCE
extern nv2a_flip_hook_fn nv2a_flip_hook_installed;
/* Install (NULL removes) the hook. One. */
void nv2a_flip_hook_set(nv2a_flip_hook_fn fn);
#endif

static inline void nv2a_flip_hook_run(void)
{
#ifdef XBOXRECOMP_ENHANCE
    nv2a_flip_hook_fn fn = __atomic_load_n(&nv2a_flip_hook_installed, __ATOMIC_ACQUIRE);
    if (fn)
        fn();
#endif
}

/* ------------------------------------------------------------------------ */
/* HUD capture glue (enhancements layer only)                               */
/* ------------------------------------------------------------------------ */

/* The HUD mapping capture (src/enhance/hud_capture.c, RECOMP_DEBUG=
 * hud_capture) asks for one BMP per captured guest frame, taken at the flip
 * that presents it. The guest's frame thread posts a request naming that
 * flip; the walker takes it at the flip and writes the picture (the CPU
 * path and write-back backends from guest memory in nv2a_pb_exec.c, D3D11
 * from its render target). It lives here, not in the layer, because the
 * layer links the runtime and never the other way round. While nothing is
 * posted a flip costs one load. */
#define NV2A_CAP_SLOTS 64u
#define NV2A_CAP_PATH  512u

enum {
    NV2A_CAP_FREE = 0,
    NV2A_CAP_LIVE,              /* posted, its flip not reached */
    NV2A_CAP_TAKEN,             /* the walker is writing it */
    NV2A_CAP_DONE,
    NV2A_CAP_FAILED,
    NV2A_CAP_DEAD               /* its flip passed without a take */
};

/* What only the kernel knows (xbox_memory_layout.c registers it at the ack
 * thread's start): whether the walker has executed everything the guest
 * kicked (poking it when not), and where the title's code lies. */
struct nv2a_cap_kernel {
    int (*walk_idle)(void);
    void (*code_range)(uint32_t *lo, uint32_t *hi);
};

#ifdef XBOXRECOMP_ENHANCE
extern int nv2a_cap_live;       /* atomic: LIVE requests */

void nv2a_cap_set_kernel(const struct nv2a_cap_kernel *k);
const struct nv2a_cap_kernel *nv2a_cap_kernel(void);
/* The walker, at every flip (FLIP_STALL), after the backend's present. */
void nv2a_cap_note_flip(uint32_t flip);
/* The last flip the walker reached. */
uint32_t nv2a_cap_flips(void);
/* Guest side: a request for `flip` into `path`; the slot, or -1 when the
 * ring is full or the path too long. */
int nv2a_cap_post(uint32_t flip, const char *path);
/* Guest side: the slot's end state (DONE, FAILED, DEAD), the slot then
 * free again; or -1 while it is LIVE or TAKEN. With `retract`, a LIVE slot
 * is taken back and reads as DEAD. */
int nv2a_cap_poll(int slot, int retract);
/* Walker side, at `flip`: the slot to write (its path copied out), or -1.
 * Requests for an earlier flip are marked DEAD on the way. */
int nv2a_cap_take(uint32_t flip, char *path, size_t len);
void nv2a_cap_done(int slot, int ok);
/* Tests: every slot free, no kernel, flip 0. */
void nv2a_cap_reset(void);
#endif

static inline int nv2a_cap_pending(void)
{
#ifdef XBOXRECOMP_ENHANCE
    return __atomic_load_n(&nv2a_cap_live, __ATOMIC_ACQUIRE) != 0;
#else
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* NV2A_BACKEND_COMMON_H */
