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
 *   header  "XRTP", u32 version (1 to 5), u32 count, u32 scale,
 *           u64 index offset, v1: u64 reserved; v2: u32 text scale,
 *           u32 reserved; v3 to v5: u32 text scale, u32 classes
 *   index   count x 32 bytes, sorted by hash: u64 hash, u32 width,
 *           u32 height, u32 levels, u32 flags, u64 payload offset
 *   payload per entry, level 0 then each further level, one A8R8G8B8
 *           texel per pixel (the bytes nv2a_tex_decode produces), or
 *           with XRTP_FLAG_BC3 or XRTP_FLAG_BC1 the blocks of each level
 *
 * v1 flags are 0. v2 flags: bits 0-1 the kind (art, font atlas, baked
 * text: the unit a player turns off), bit 4 a BC3 payload. v3 adds kind 3
 * (world) and bit 5 (a BC1 payload), and gives the header word at +28 to
 * `classes`, a bit per kind present, informational. v4 adds bit 6, a world
 * entry the backends sample hex-tiled under textures.detile
 * (nv2a_hex_tile.h); the CLI writes v4 only for a pack that marks one, so
 * a stock pack stays v3. v5 adds bit 7, a recolour entry: keyed by a P8
 * texture's index plane alone (nv2a_stage_plane_hash), its payload one
 * (i0, i1, w) per upscaled texel that nv2a_tex_pack_recolour turns into
 * texels with whatever palette the game has bound, so one entry serves
 * every palette of the plane (BLiNX 2's custom characters). The exact
 * entry of a (plane, palette) still wins (nv2a_stage_pack_find). Each
 * version refuses
 * what it does not define, so a later meaning is never misread. The header
 * scale is the art scale; each entry carries its own size, so text built
 * at 2x and art at 4x share one pack.
 *
 * The index is read at load; an entry's payload is read when an upload
 * needs it and released once the backend holds the GPU copy
 * (nv2a_tex_pack_release), so a large pack costs VRAM, not RAM twice over.
 * Held payloads stay within NV2A_TEX_PACK_RESIDENT_MAX, the least recently
 * used freed past it. Lives in the kernel library like nv2a_host_opts, so the backends
 * never link the enhancements layer: without a pack nv2a_tex_pack_count()
 * is 0 and nothing else runs. Not thread-safe: load at startup or at a
 * flip on the walker (nv2a_host_opts_apply), use from the thread that
 * uploads textures.
 */
#ifndef NV2A_TEX_PACK_H
#define NV2A_TEX_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_TEX_PACK_MAGIC   "XRTP"
#define NV2A_TEX_PACK_VERSION 6u   /* the newest read; 1 to 5 are read too */
#define NV2A_TEX_PACK_HDR     32u
#define NV2A_TEX_PACK_ENTRY   32u
#define NV2A_TEX_PACK_MAX_DIM 16384u   /* D3D11 and Metal texture limit */
#define NV2A_TEX_PACK_MAX_ENTRIES (1u << 20)
/* CPU bytes of read payloads kept at most. The CPU copy is needed only to
 * upload (each backend's texture cache holds the GPU copy), and a pack with
 * an art tier (KUF's, 2.5 GB) would otherwise end up whole in memory over
 * a long session. A small pack (BLiNX 2's) never reaches it. */
#define NV2A_TEX_PACK_RESIDENT_MAX ((size_t)512u << 20)
/* A recolour entry's plane whose palette keeps changing (a fade, a cycling
 * palette: water, fire, a flashing HUD) would be recoloured on the CPU at
 * every change and add an upscaled copy to the GPU cache each time. More
 * than BURST uploads under a palette other than the plane's previous one
 * within WINDOW flips mark it volatile: its palettes then go up from the
 * guest, as with no pack, until QUIET flips pass with no change; the flip
 * that ends it bumps the generation so its guest copies go up recoloured
 * again. Each later return doubles the plane's quiet time, up to
 * QUIET_MAX, since each bump re-uploads every cached texture. A player
 * picking colours in a menu changes a few a second at most; an animation
 * changes one each frame. */
#define NV2A_TEX_PACK_RECOLOUR_BURST     12u
#define NV2A_TEX_PACK_RECOLOUR_WINDOW    30u
#define NV2A_TEX_PACK_RECOLOUR_QUIET     60u
#define NV2A_TEX_PACK_RECOLOUR_QUIET_MAX 1920u
/* The recolour buffer is kept between uploads up to this size (a 512x512
 * plane at 2x with mips) and freed after the upload of a larger one; it
 * counts in the resident bytes. */
#define NV2A_TEX_PACK_RECOLOUR_KEEP ((size_t)8u << 20)

/* Entry flags: v2's, and v3's on top. */
#define XRTP_KIND_MASK  0x3u
#define XRTP_KIND_ART   0u    /* HUD pieces and other images */
#define XRTP_KIND_FONT  1u    /* the font atlas the glyphs are drawn from */
#define XRTP_KIND_TEXT  2u    /* images of pre-rendered text */
#define XRTP_KIND_WORLD 3u    /* v3: textures of the 3D scene */
#define XRTP_KIND_COUNT 4u
#define XRTP_FLAG_BC3   0x10u /* payload is BC3 blocks, not A8R8G8B8 texels */
#define XRTP_FLAG_BC1   0x20u /* v3: payload is BC1 blocks, always opaque */
#define XRTP_FLAGS_KNOWN_V2 (XRTP_KIND_MASK | XRTP_FLAG_BC3)
#define XRTP_FLAG_DETILE_HEX 0x40u /* v4: a world entry, hex-tiled (textures.detile) */
#define XRTP_FLAGS_KNOWN_V3 (XRTP_KIND_MASK | XRTP_FLAG_BC3 | XRTP_FLAG_BC1)
#define XRTP_FLAGS_KNOWN_V4 (XRTP_FLAGS_KNOWN_V3 | XRTP_FLAG_DETILE_HEX)
/* v5: the hash is a P8 index plane's, and each payload texel (u32, little-
 * endian) holds two palette indices and a weight: bits 0-7 i0, 8-15 i1,
 * 16-23 w, 24-31 zero. Never with a block format. */
#define XRTP_FLAG_RECOLOUR 0x80u
#define XRTP_FLAGS_KNOWN_V5 (XRTP_FLAGS_KNOWN_V4 | XRTP_FLAG_RECOLOUR)
/* v6: the entry's chain is for a guest of one level drawn minified (2D art,
 * a font atlas): a sampler that forces the chain on such a guest blends two
 * levels, with nv2a_stage_blend_bias, instead of taking the nearest one
 * (nv2a_stage_lod). Meaningless on an entry of one level. */
#define XRTP_FLAG_MIP_BLEND 0x100u
#define XRTP_FLAGS_KNOWN_V6 (XRTP_FLAGS_KNOWN_V5 | XRTP_FLAG_MIP_BLEND)

/* nv2a_tex_pack_entry.bc: the payload's block format. */
#define XRTP_BC_NONE 0
#define XRTP_BC_1    1
#define XRTP_BC_3    3

struct nv2a_tex_pack_entry {
    uint64_t  hash;
    uint32_t  width, height, levels, flags;
    uint64_t  offset;         /* of the payload in the file */
    size_t    texels;         /* over every level */
    size_t    bytes;          /* of the payload: texels * 4, or the blocks */
    uint32_t  kind;           /* XRTP_KIND_* */
    int       bc;             /* XRTP_BC_*: the payload's block format */
    uint32_t *data;           /* read on use; NULL until then or once freed */
    /* Read entries, most recently used first (the resident budget). */
    struct nv2a_tex_pack_entry *lru_prev, *lru_next;
    /* A recolour entry's recent palettes and volatile state, allocated at
     * its first find_recolour. */
    struct nv2a_tex_pack_palettes *pal;
};

/* Load `path`: validates the header, the index bounds against the file
 * size and the sort order, and keeps the index. Returns the entry count,
 * or -1 with the reason in `err` (the file is then not loaded). A second
 * load replaces the first. Load and unload bump nv2a_tex_pack_gen(), so a
 * pack changed at a flip on the walker thread (nv2a_host_pack_request,
 * the menu's picker) re-uploads every cached texture. */
int nv2a_tex_pack_load(const char *path, char *err, size_t errlen);
void nv2a_tex_pack_unload(void);
/* 1 if `path` (UTF-8, as the loader opens it) is a regular file: a folder
 * or a dangling link named *.pack is not a pack. */
int nv2a_tex_pack_is_file(const char *path);

unsigned nv2a_tex_pack_count(void);
unsigned nv2a_tex_pack_scale(void);
/* The loaded file's version, for the log line. */
unsigned nv2a_tex_pack_version(void);
/* Entries of one XRTP_KIND_*, and the BC1 and BC3 ones, for the log line. */
unsigned nv2a_tex_pack_kind_count(uint32_t kind);
unsigned nv2a_tex_pack_bc1_count(void);
unsigned nv2a_tex_pack_bc3_count(void);
/* Entries marked XRTP_FLAG_DETILE_HEX, for the log line and the backends:
 * 0, and no draw takes the hex variant. */
unsigned nv2a_tex_pack_detile_count(void);
/* Entries marked XRTP_FLAG_RECOLOUR: 0, and no upload computes a plane
 * hash or looks one up. */
unsigned nv2a_tex_pack_recolour_count(void);
/* Entries marked XRTP_FLAG_MIP_BLEND, for the log line. */
unsigned nv2a_tex_pack_blend_count(void);
/* The scale the TEXT entries were built at: the u32 at header +24 from v2
 * on (0 in the file means the header scale). Informational, like the scale. */
unsigned nv2a_tex_pack_text_scale(void);
/* Bytes of a BC3 payload of w x h over `levels` levels: 16 per 4x4 block. */
size_t nv2a_tex_pack_bc3_bytes(uint32_t w, uint32_t h, uint32_t levels);
/* Bytes of a BC1 payload: 8 per 4x4 block. */
size_t nv2a_tex_pack_bc1_bytes(uint32_t w, uint32_t h, uint32_t levels);

/* Turn the whole pack, or one kind of entry, off and on: find then
 * returns NULL for it, and the guest texture is uploaded as without a
 * pack. Called at a flip (nv2a_host_opts_apply); a change of what matches
 * bumps nv2a_tex_pack_gen(), and a texture uploaded under another
 * generation is uploaded again. A load or unload at run time goes through
 * nv2a_host_pack_request, applied at a flip like these. */
void nv2a_tex_pack_set_enabled(int on);
void nv2a_tex_pack_set_kind_enabled(uint32_t kind, int on);
uint32_t nv2a_tex_pack_gen(void);
/* Hits of nv2a_tex_pack_find, for the backends' statistics. */
uint64_t nv2a_tex_pack_hits(void);

/* The entry for a texel hash, or NULL. The hash is the one the backend
 * computed for its cache entry and the dump names its files by; a P8
 * texture's folds its palette in, so an entry built from a dump matches
 * that one palette (the pack holds decoded texels, never a palette, and the
 * archive path cannot name such a hash). An entry whose aspect differs from the guest texture's (guest_w x guest_h) is
 * not taken, nor counted as a hit. A recolour entry is never returned here. */
struct nv2a_tex_pack_entry *nv2a_tex_pack_find(uint64_t hash, uint32_t guest_w,
                                               uint32_t guest_h);
/* The recolour entry for a P8 index plane's hash (nv2a_stage_plane_hash)
 * under the palette whose hash is palette_hash (nv2a_stage_palette_hash),
 * or NULL; the same switches and aspect rule as find, and NULL while the
 * plane is volatile (NV2A_TEX_PACK_RECOLOUR_BURST). A plane turning
 * volatile bumps nv2a_tex_pack_gen(), so the copies it went up with
 * recoloured go up again from the guest and an animation never mixes the
 * two. The backends go through nv2a_stage_pack_find, which tries the exact
 * hash first. */
struct nv2a_tex_pack_entry *nv2a_tex_pack_find_recolour(uint64_t plane_hash,
                                                        uint64_t palette_hash,
                                                        uint32_t guest_w, uint32_t guest_h);
/* The pack's flip clock for the volatile test, called once a flip on the
 * walker (nv2a_pb_exec's flip, beside nv2a_tex_pack_snapshot). While a
 * plane is volatile it also ends the ones quiet for QUIET flips. */
void nv2a_tex_pack_flip(void);
/* Planes volatile now, for the tests (the trace tags each miss instead). */
unsigned nv2a_tex_pack_volatile_count(void);
/* A recolour entry's texels under palette pal[0..pal_len): per texel
 * (c0 * (255 - w) + c1 * w + 127) / 255 on each A8R8G8B8 channel of
 * pal[i0] and pal[i1], an index past the palette magenta as in the decode.
 * Written to a buffer the pack owns, good until the next call, the
 * release of a recolour entry or unload; NULL if the payload cannot be
 * read or the buffer cannot grow. The payload stays held under the
 * resident budget (release keeps it): the plane's next palette reads it
 * again. */
const uint32_t *nv2a_tex_pack_recolour(struct nv2a_tex_pack_entry *e, const uint32_t *pal,
                                       uint32_t pal_len);
/* The recolour of n texels, for the tests and the CLI's reference: out[i]
 * from idx[i] as above. */
void nv2a_tex_pack_recolour_texels(const uint32_t *idx, size_t n, const uint32_t *pal,
                                   uint32_t pal_len, uint32_t *out);

/* The entry's payload (e->bytes of it, level 0 first), read from the
 * file when it is not held: e->texels A8R8G8B8 texels, or blocks when
 * e->bc. NULL if the read failed (the caller then uploads the guest
 * texels as if there were no entry). A read that takes the held bytes past
 * the budget frees the least recently used other entries first, so the
 * pointer is good until the next call: upload from it, never keep it. An
 * entry larger than the budget is still read, and is then the only one
 * held. The file is opened before anything is freed, so a pack that cannot
 * be opened keeps what is held; one whose size or mtime differs from the
 * load's (rebuilt in place) is never read again, said once, until the next
 * load. */
const uint32_t *nv2a_tex_pack_texels(struct nv2a_tex_pack_entry *e);
/* The backend's GPU copy of the entry exists: the payload is freed, and a
 * later upload reads it again. A recolour entry's is kept (see
 * nv2a_tex_pack_recolour), and the recolour buffer is freed when it is
 * past NV2A_TEX_PACK_RECOLOUR_KEEP. */
void nv2a_tex_pack_release(struct nv2a_tex_pack_entry *e);
/* Release does nothing and the payloads stay held under the budget, for A/B
 * (RECOMP_DEBUG=tex_pack_keep, set by the layer that loads a pack). Kept
 * across unload and load. */
void nv2a_tex_pack_set_keep(int keep);
/* Payload bytes held now, for the tests and the stats. */
size_t nv2a_tex_pack_resident_bytes(void);
/* Tests only: the budget in bytes (NV2A_TEX_PACK_RESIDENT_MAX by default),
 * applied at the next read. Kept across unload and load. */
void nv2a_tex_pack_set_resident_max(size_t bytes);

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
#define NV2A_TEX_PACK_MISS_VOLATILE 4u   /* a recolour entry; its palette keeps changing */

struct nv2a_tex_pack_miss {
    uint64_t hash;
    uint32_t width, height, color, levels;
    uint32_t count;
    int      candidate;   /* swizzled or DXT, palette or mips or not: packable */
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
/* Every miss so far, from any thread (the stats). */
uint64_t nv2a_tex_pack_miss_total(void);

/* The ring is the upload path's (the walker thread), written without a
 * lock; a reader on another thread (the in-game menu's Pack and Debug
 * pages) reads a copy the walker takes at the flip. snapshot_want(1) while
 * a reader is shown, (0) when it is gone: with no users the flip's call is
 * one load. misses_snapshot copies up to n of the last copy, most counted
 * first; 0 when none was taken yet, or the walker was writing it each of
 * the few times the reader looked (it keeps what it drew). */
void nv2a_tex_pack_snapshot_want(int on);
void nv2a_tex_pack_snapshot(void);
unsigned nv2a_tex_pack_misses_snapshot(struct nv2a_tex_pack_miss *out, unsigned n);

/* Print the resident bytes, the budget and the payloads freed for it
 * ("[TEXPACK] resident"), then the misses (RECOMP_TRACE=tex_pack), one
 * "[TEXPACK] miss" line each, to stderr, tagged " (text off)", " (pack off)" or " (entry
 * refused)" when the pack holds the texture. The backends call it every
 * 600 flips. */
void nv2a_tex_pack_print_misses(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEX_PACK_H */
