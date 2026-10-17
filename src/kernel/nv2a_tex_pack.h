/*
 * nv2a_tex_pack.h - a texture pack: host-side replacements for guest
 * textures, keyed by the hash the GPU backends already compute for their
 * per-flip change check (nv2a_tex_hash over the bytes nv2a_tex_extent
 * gives). The enhancements layer loads one (textures.pack); the Metal and
 * D3D11 uploads look their texel hash up here and, on a hit, create the
 * host texture at the pack entry's size from the pack's texels. The guest
 * texture, the dump and the game are untouched: a pack is a host-only
 * view, and a texture the game rewrites stops matching.
 *
 * File format (docs/formats/texture-pack.md), little-endian:
 *   header  "XRTP", u32 version (1 or 2), u32 count, u32 scale,
 *           u64 index offset, v1: u64 reserved; v2: u32 text scale,
 *           u32 reserved
 *   index   count x 32 bytes, sorted by hash: u64 hash, u32 width,
 *           u32 height, u32 levels, u32 flags, u64 payload offset
 *   payload per entry, level 0 then each further level, one A8R8G8B8
 *           texel per pixel (the bytes nv2a_tex_decode produces), or
 *           with XRTP_FLAG_BC3 the BC3 (DXT5) blocks of each level
 *
 * v1 flags are 0. v2 flags: bits 0-1 the kind (art, font atlas, baked
 * text: the unit a player turns off), bit 4 a BC3 payload; any other bit
 * is refused, so a later meaning is never misread. The header scale is
 * the art scale; each entry carries its own size, so text built at 2x and
 * art at 4x share one pack.
 *
 * The index is read at load; an entry's texels are read on first use and
 * kept. Lives in the kernel library like nv2a_host_opts, so the backends
 * never link the enhancements layer: without a pack nv2a_tex_pack_count()
 * is 0 and nothing else runs. Not thread-safe: load at startup, use from
 * the thread that uploads textures.
 */
#ifndef NV2A_TEX_PACK_H
#define NV2A_TEX_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_TEX_PACK_MAGIC   "XRTP"
#define NV2A_TEX_PACK_VERSION 2u   /* the newest read; 1 is read too */
#define NV2A_TEX_PACK_HDR     32u
#define NV2A_TEX_PACK_ENTRY   32u
#define NV2A_TEX_PACK_MAX_DIM 16384u   /* D3D11 and Metal texture limit */
#define NV2A_TEX_PACK_MAX_ENTRIES (1u << 20)

/* v2 entry flags. */
#define XRTP_KIND_MASK  0x3u
#define XRTP_KIND_ART   0u    /* HUD pieces and other images */
#define XRTP_KIND_FONT  1u    /* the font atlas the glyphs are drawn from */
#define XRTP_KIND_TEXT  2u    /* images of pre-rendered text */
#define XRTP_KIND_COUNT 3u
#define XRTP_FLAG_BC3   0x10u /* payload is BC3 blocks, not A8R8G8B8 texels */
#define XRTP_FLAGS_KNOWN (XRTP_KIND_MASK | XRTP_FLAG_BC3)

struct nv2a_tex_pack_entry {
    uint64_t  hash;
    uint32_t  width, height, levels, flags;
    uint64_t  offset;         /* of the payload in the file */
    size_t    texels;         /* over every level */
    size_t    bytes;          /* of the payload: texels * 4, or the BC3 blocks */
    uint32_t  kind;           /* XRTP_KIND_* */
    int       bc3;            /* payload is BC3 blocks */
    uint32_t *data;           /* read on first use; NULL until then */
};

/* Load `path`: validates the header, the index bounds against the file
 * size and the sort order, and keeps the index. Returns the entry count,
 * or -1 with the reason in `err` (the file is then not loaded). A second
 * load replaces the first. */
int nv2a_tex_pack_load(const char *path, char *err, size_t errlen);
void nv2a_tex_pack_unload(void);

unsigned nv2a_tex_pack_count(void);
unsigned nv2a_tex_pack_scale(void);
/* Entries of one XRTP_KIND_*, and the BC3 ones, for the log line. */
unsigned nv2a_tex_pack_kind_count(uint32_t kind);
unsigned nv2a_tex_pack_bc3_count(void);
/* The scale the TEXT entries were built at: v2's u32 at header +24 (0 in
 * the file means the header scale). Informational, like the scale. */
unsigned nv2a_tex_pack_text_scale(void);
/* Bytes of a BC3 payload of w x h over `levels` levels: 16 per 4x4 block. */
size_t nv2a_tex_pack_bc3_bytes(uint32_t w, uint32_t h, uint32_t levels);

/* Turn the whole pack, or one kind of entry, off and on: find then
 * returns NULL for it, and the guest texture is uploaded as without a
 * pack. Called at a flip (nv2a_host_opts_apply); a change of what matches
 * bumps nv2a_tex_pack_gen(), and a texture uploaded under another
 * generation is uploaded again. Unload is not offered at run time. */
void nv2a_tex_pack_set_enabled(int on);
void nv2a_tex_pack_set_kind_enabled(uint32_t kind, int on);
uint32_t nv2a_tex_pack_gen(void);
/* Hits of nv2a_tex_pack_find, for the backends' statistics. */
uint64_t nv2a_tex_pack_hits(void);

/* The entry for a texel hash, or NULL. The hash is the one the backend
 * computed for its cache entry; a P8 texture's folds its palette in and
 * so never matches, which is right (the pack holds no palettes). An entry
 * whose aspect differs from the guest texture's (guest_w x guest_h) is
 * not taken, nor counted as a hit. */
struct nv2a_tex_pack_entry *nv2a_tex_pack_find(uint64_t hash, uint32_t guest_w,
                                               uint32_t guest_h);

/* The entry's payload (e->bytes of it, level 0 first), read from the
 * file on the first call: e->texels A8R8G8B8 texels, or BC3 blocks when
 * e->bc3. NULL if the read failed (the caller then uploads the guest
 * texels as if there were no entry). */
const uint32_t *nv2a_tex_pack_texels(struct nv2a_tex_pack_entry *e);

/* ---- Misses: guest textures uploaded while a pack is loaded that it did
 * not replace, so an owner can see what a pack could still hold. A ring
 * of NV2A_TEX_PACK_MISSES distinct (hash, size, format) with a count each:
 * bounded memory, no allocation on the upload path. */
#define NV2A_TEX_PACK_MISSES 64

/* Why a miss went up from the guest (nv2a_tex_pack_miss.why). */
#define NV2A_TEX_PACK_MISS_ABSENT   0u   /* the pack has no entry for it */
#define NV2A_TEX_PACK_MISS_PACK_OFF 1u   /* it has one; the pack is off */
#define NV2A_TEX_PACK_MISS_KIND_OFF 2u   /* it has one of a kind turned off */
#define NV2A_TEX_PACK_MISS_REFUSED  3u   /* it has one the backend could not use */

struct nv2a_tex_pack_miss {
    uint64_t hash;
    uint32_t width, height, color, levels;
    uint32_t count;
    int      candidate;   /* swizzled or DXT without mips: packable */
    uint32_t why;         /* NV2A_TEX_PACK_MISS_*, as of the latest miss */
    uint32_t kind;        /* the entry's kind, for KIND_OFF */
};

/* Called by the backends' uploads when nv2a_tex_pack_count() and no entry
 * replaced the texture, including an entry found but not uploaded (the
 * device refused its size or format). `color` is the guest D3D format. */
void nv2a_tex_pack_miss(uint64_t hash, uint32_t w, uint32_t h, uint32_t color,
                        uint32_t levels);
/* Copy up to n misses, most counted first; returns how many. */
unsigned nv2a_tex_pack_misses(struct nv2a_tex_pack_miss *out, unsigned n);
/* Print the misses (RECOMP_TRACE=tex_pack), one "[TEXPACK] miss" line
 * each, to stderr, tagged " (text off)", " (pack off)" or " (entry
 * refused)" when the pack holds the texture. The backends call it every
 * 600 flips. */
void nv2a_tex_pack_print_misses(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEX_PACK_H */
