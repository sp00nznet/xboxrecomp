/*
 * nv2a_tex_dump.h - the runtime texture dump (RECOMP_DEBUG=tex_dump=<dir>):
 * every texture a GPU backend uploads, written once per texel hash, so an
 * owner can see what a game draws and build a texture pack for it from the
 * texels the runtime decoded (openspec texture-upscale, design D1).
 *
 * The hash is the one the backend already computed for its texture cache
 * and passes to nv2a_tex_pack_find (nv2a_tex_hash over nv2a_tex_extent's
 * bytes, a P8 palette folded in), so a dumped <hash> is the key a pack
 * entry replaces. Per hash, in the directory (docs/formats/texture-dump.md):
 *   <hash>.xtd   a 64-byte header, then the decoded A8R8G8B8 levels, level
 *                0 first: exactly what nv2a_tex_decode produced, the
 *                payload layout of a raw pack entry
 *   <hash>.json  the facts: size, levels, format, pitch, layout, palette,
 *                wrap, filters, address, the first flip it was seen at
 * and, rewritten at each summary, counts.json (uploads per hash) and
 * volatile.json (the addresses given up on, with their hashes).
 *
 * Not written, counted instead: a stage bound to a render target (it never
 * hashes; the backend calls nv2a_tex_dump_rt), an address whose texels
 * took more than NV2A_TEX_DUMP_VOLATILE distinct hashes (CPU-written
 * surfaces, movie frames: the first ones stay written, the address is
 * named in volatile.json and nothing more of it is written), a linear
 * texture's texels (its sidecar is written, with "texels": false: movie
 * frames and CPU surfaces, which a pack leaves out by default, D3), and a
 * texture over NV2A_TEX_PACK_MAX_DIM. A "[TEXDUMP]" line every 600 flips
 * and at a normal exit gives the counts (a signal's default action skips
 * the exit one; the per-hash files are written at the upload).
 *
 * Lives in the kernel library beside nv2a_tex_pack, so a build without the
 * enhancements layer has it too. Off unless the entry names a directory;
 * off, the backends' one test per upload is all it costs. The upload calls
 * come from the walker thread; the exit summary takes the same lock, but
 * waits for it only about 200 ms (ExitProcess can end the walker holding
 * it) and is skipped when it does not come.
 */
#ifndef NV2A_TEX_DUMP_H
#define NV2A_TEX_DUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nv2a_stage;

#define NV2A_TEX_DUMP_MAGIC    "XRTD"
#define NV2A_TEX_DUMP_VERSION  1u
#define NV2A_TEX_DUMP_HDR      64u
/* Distinct hashes one (offset, width, height) may take before it is
 * volatile. A texture a title loads once has one; a palette or a few
 * animation frames swapped in place stay under it. */
#define NV2A_TEX_DUMP_VOLATILE 8u
/* The .xtd bytes one run may write (header and texels): far past a
 * title's whole texture set (a BLiNX 2 level and its hub are about 25 MB),
 * short of filling a disk when a game streams new texels for hours. At the
 * cap, or when its tables cannot grow, the dump stops with one line and
 * only counts from then on. */
#define NV2A_TEX_DUMP_MAX_BYTES (2048ull << 20)

/* .xtd header flags (+36). */
#define XTD_FLAG_SWIZZLED 0x1u
#define XTD_FLAG_LINEAR   0x2u
#define XTD_FLAG_DXT      0x4u
#define XTD_FLAG_PALETTE  0x8u

/* Whether the dump is on: the tex_dump debug entry, read once, names a
 * directory (created if missing). */
int nv2a_tex_dump_on(void);

/* Whether nv2a_tex_dump_upload would write this upload's texels: a new
 * hash at an address not given up on, not linear, not over the size cap.
 * A backend that did not decode the guest texels (a pack entry replaced
 * them) decodes them only when this says so. No side effects. */
int nv2a_tex_dump_wants(const struct nv2a_stage *t, uint64_t hash);

/* An upload of stage t's texture with texel hash `hash`: counts it and,
 * when nv2a_tex_dump_wants says so, writes <hash>.xtd from `levels`
 * (nv2a_tex_texels(t->width, t->height, t->levels) A8R8G8B8 texels) and
 * <hash>.json. `levels` may be NULL only when wants said 0. */
void nv2a_tex_dump_upload(const struct nv2a_stage *t, uint64_t hash,
                          const uint32_t *levels);

/* A stage bound to a render target instead of guest texels: counted per
 * bind and per distinct target (offset, size, format). */
void nv2a_tex_dump_rt(const struct nv2a_stage *t);

/* A present: the flip the next first sightings record, and the summary
 * line every 600. */
void nv2a_tex_dump_flip(uint64_t flip);

/* The "[TEXDUMP]" line to stderr, and counts.json and volatile.json
 * rewritten when anything changed since the last. At exit too. */
void nv2a_tex_dump_summary(void);

/* What the dump did, for the summary and the tests. */
struct nv2a_tex_dump_stats {
    uint64_t uploads;         /* every upload seen */
    uint64_t written;         /* .xtd files written (distinct hashes) */
    uint64_t texels;          /* texels in them, every level */
    uint64_t repeats;         /* uploads of a hash already dumped */
    uint64_t linear;          /* linear textures: sidecar only (distinct) */
    uint64_t volatile_addrs;  /* addresses given up on */
    uint64_t volatile_skips;  /* uploads skipped at them */
    uint64_t rt_binds;        /* stage binds of a render target */
    uint64_t rt_targets;      /* distinct targets bound (up to a table) */
    uint64_t oversize;        /* uploads over NV2A_TEX_PACK_MAX_DIM */
    uint64_t errors;          /* files that could not be written (the
                               * hash is retried, up to 3 times) */
    uint64_t bytes;           /* .xtd bytes written */
    uint64_t stopped;         /* uploads after the dump stopped */
};
void nv2a_tex_dump_get_stats(struct nv2a_tex_dump_stats *out);

/* Tests: dump into `dir` from now on (NULL turns it off), with every count,
 * table and the flip reset. The environment is then not read. */
int nv2a_tex_dump_set_dir(const char *dir);
/* Tests: the byte cap (0 restores NV2A_TEX_DUMP_MAX_BYTES). Kept across
 * set_dir. */
void nv2a_tex_dump_set_max_bytes(uint64_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEX_DUMP_H */
