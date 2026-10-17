/*
 * nv2a_tex_pack.c - see nv2a_tex_pack.h.
 */
#include "nv2a_tex_pack.h"
#include "../d3d/d3d8_swizzle.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define pack_seek(f, off) _fseeki64((f), (long long)(off), SEEK_SET)
#define pack_tell(f)      _ftelli64(f)
#else
#define pack_seek(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#define pack_tell(f)      ftello(f)
#endif

static struct nv2a_tex_pack_entry *s_entries;
static unsigned  s_count, s_scale, s_text_scale;
static unsigned  s_kind_count[XRTP_KIND_COUNT], s_bc3_count;
static char     *s_path;
static uint64_t  s_hits;

/* What find matches: bit k for kind k, PACK_ON for the whole pack.
 * Kept across unload and load, since the toggles belong to the player,
 * not to the file. Written at a flip and read by the upload on the same
 * walker thread; atomic so a menu reading it on another thread is defined. */
#define PACK_ON   0x80u
#define KINDS_ON  ((1u << XRTP_KIND_COUNT) - 1u)
static atomic_uint s_mask = PACK_ON | KINDS_ON;
static atomic_uint s_gen;

static struct nv2a_tex_pack_miss s_miss[NV2A_TEX_PACK_MISSES];
static unsigned s_miss_n, s_miss_next;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Texels over `levels` levels of w x h, each level halving (floor, min 1):
 * the layout nv2a_tex_texels gives, written out here so the reader has no
 * dependency on the backend-common code. */
static size_t level_texels(uint32_t w, uint32_t h, uint32_t levels)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < levels; l++) {
        n += (size_t)w * h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return n;
}

/* Levels a w x h chain can have: 1 + floor(log2(max(w, h))). More would
 * repeat 1x1 levels, which D3D11 refuses (the texture draws black) and
 * Metal reports as invalid. */
static uint32_t max_levels(uint32_t w, uint32_t h)
{
    uint32_t m = w > h ? w : h, n = 1;
    while (m >>= 1)
        n++;
    return n;
}

size_t nv2a_tex_pack_bc3_bytes(uint32_t w, uint32_t h, uint32_t levels)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < levels; l++) {
        n += (size_t)((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return n;
}

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

void nv2a_tex_pack_unload(void)
{
    unsigned i;
    for (i = 0; i < s_count; i++)
        free(s_entries[i].data);
    free(s_entries);
    free(s_path);
    s_entries = NULL;
    s_path = NULL;
    s_count = 0;
    s_scale = 0;
    s_text_scale = 0;
    memset(s_kind_count, 0, sizeof s_kind_count);
    s_bc3_count = 0;
    s_hits = 0;
    memset(s_miss, 0, sizeof s_miss);
    s_miss_n = s_miss_next = 0;
}

int nv2a_tex_pack_load(const char *path, char *err, size_t errlen)
{
    uint8_t hdr[NV2A_TEX_PACK_HDR];
    uint8_t *index = NULL;
    FILE *f;
    long long size;
    uint64_t index_off;
    uint32_t count, version, scale, text_scale = 0;
    unsigned i, kinds[XRTP_KIND_COUNT] = {0}, bc3 = 0;
    struct nv2a_tex_pack_entry *entries = NULL;
    const char *why = NULL;

    nv2a_tex_pack_unload();
    if (!path || !*path) {
        fail(err, errlen, "no path");
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        fail(err, errlen, "cannot open");
        return -1;
    }
    if (pack_seek(f, 0) != 0 || fseek(f, 0, SEEK_END) != 0 || (size = pack_tell(f)) < 0) {
        fclose(f);
        fail(err, errlen, "cannot read");
        return -1;
    }
    if ((uint64_t)size < NV2A_TEX_PACK_HDR || pack_seek(f, 0) != 0
            || fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        fail(err, errlen, "too short for a pack header");
        return -1;
    }
    if (memcmp(hdr, NV2A_TEX_PACK_MAGIC, 4) != 0) {
        fclose(f);
        fail(err, errlen, "not a texture pack (bad magic)");
        return -1;
    }
    version = rd32(hdr + 4);
    count = rd32(hdr + 8);
    scale = rd32(hdr + 12);
    index_off = rd64(hdr + 16);
    if (version == 2u)
        text_scale = rd32(hdr + 24);
    if (version != 1u && version != 2u)
        why = "unsupported pack version";
    /* Zero in v1 and v2's last u32, so a later reader can give them a
     * meaning an older reader refuses instead of misreading. v2 gave the
     * first u32 the text scale. */
    else if ((version == 1u ? rd64(hdr + 24) : rd32(hdr + 28)) != 0)
        why = "non-zero reserved header field";
    else if (count > NV2A_TEX_PACK_MAX_ENTRIES)
        why = "too many entries";
    /* Subtracted, not added: an offset near 2^64 must not wrap past the check. */
    else if (index_off < NV2A_TEX_PACK_HDR || index_off > (uint64_t)size
             || (uint64_t)count * NV2A_TEX_PACK_ENTRY > (uint64_t)size - index_off)
        why = "index outside the file";
    if (why) {
        fclose(f);
        fail(err, errlen, why);
        return -1;
    }
    if (count) {
        index = (uint8_t *)malloc((size_t)count * NV2A_TEX_PACK_ENTRY);
        entries = (struct nv2a_tex_pack_entry *)calloc(count, sizeof *entries);
        if (!index || !entries || pack_seek(f, index_off) != 0
                || fread(index, NV2A_TEX_PACK_ENTRY, count, f) != count) {
            free(index);
            free(entries);
            fclose(f);
            fail(err, errlen, "cannot read the index");
            return -1;
        }
    }
    fclose(f);
    for (i = 0; i < count && !why; i++) {
        const uint8_t *p = index + (size_t)i * NV2A_TEX_PACK_ENTRY;
        struct nv2a_tex_pack_entry *e = &entries[i];
        e->hash = rd64(p);
        e->width = rd32(p + 8);
        e->height = rd32(p + 12);
        e->levels = rd32(p + 16);
        e->flags = rd32(p + 20);
        e->offset = rd64(p + 24);
        if (!e->width || !e->height || e->width > NV2A_TEX_PACK_MAX_DIM
                || e->height > NV2A_TEX_PACK_MAX_DIM || !e->levels
                || e->levels > max_levels(e->width, e->height))
            why = "entry with a bad size";
        else if (version == 1u && e->flags)
            why = "entry with non-zero flags";
        else if (e->flags & ~XRTP_FLAGS_KNOWN)
            why = "entry with unknown flags";
        else if ((e->flags & XRTP_KIND_MASK) >= XRTP_KIND_COUNT)
            why = "entry with an unknown kind";
        /* D3D11 refuses a block-compressed texture whose top level is not
         * whole blocks; the CLI writes BC3 only for such sizes. */
        else if ((e->flags & XRTP_FLAG_BC3) && ((e->width | e->height) & 3u))
            why = "BC3 entry not a multiple of 4";
        else if (i && entries[i - 1].hash >= e->hash)
            why = "index not sorted by hash";
        else {
            e->kind = e->flags & XRTP_KIND_MASK;
            e->bc3 = (e->flags & XRTP_FLAG_BC3) != 0;
            e->texels = level_texels(e->width, e->height, e->levels);
            e->bytes = e->bc3 ? nv2a_tex_pack_bc3_bytes(e->width, e->height, e->levels)
                              : e->texels * 4u;
            if (e->offset < NV2A_TEX_PACK_HDR || e->offset > (uint64_t)size
                    || (uint64_t)e->bytes > (uint64_t)size - e->offset)
                why = "entry texels outside the file";
            kinds[e->kind]++;
            bc3 += e->bc3 ? 1u : 0u;
        }
    }
    free(index);
    if (why) {
        free(entries);
        fail(err, errlen, why);
        return -1;
    }
    s_path = (char *)malloc(strlen(path) + 1);
    if (!s_path) {
        free(entries);
        fail(err, errlen, "out of memory");
        return -1;
    }
    strcpy(s_path, path);
    s_entries = entries;
    s_count = count;
    s_scale = scale;
    s_text_scale = text_scale ? text_scale : scale;
    memcpy(s_kind_count, kinds, sizeof kinds);
    s_bc3_count = bc3;
    return (int)count;
}

unsigned nv2a_tex_pack_count(void)
{
    return s_count;
}

unsigned nv2a_tex_pack_scale(void)
{
    return s_scale;
}

unsigned nv2a_tex_pack_text_scale(void)
{
    return s_text_scale;
}

unsigned nv2a_tex_pack_kind_count(uint32_t kind)
{
    return kind < XRTP_KIND_COUNT ? s_kind_count[kind] : 0;
}

unsigned nv2a_tex_pack_bc3_count(void)
{
    return s_bc3_count;
}

uint64_t nv2a_tex_pack_hits(void)
{
    return s_hits;
}

static void set_mask(unsigned m)
{
    if (atomic_exchange_explicit(&s_mask, m, memory_order_relaxed) != m)
        atomic_fetch_add_explicit(&s_gen, 1u, memory_order_relaxed);
}

void nv2a_tex_pack_set_enabled(int on)
{
    unsigned m = atomic_load_explicit(&s_mask, memory_order_relaxed);
    set_mask(on ? m | PACK_ON : m & ~PACK_ON);
}

void nv2a_tex_pack_set_kind_enabled(uint32_t kind, int on)
{
    unsigned m = atomic_load_explicit(&s_mask, memory_order_relaxed);
    if (kind >= XRTP_KIND_COUNT)
        return;
    set_mask(on ? m | (1u << kind) : m & ~(1u << kind));
}

uint32_t nv2a_tex_pack_gen(void)
{
    return atomic_load_explicit(&s_gen, memory_order_relaxed);
}

/* The entry for (hash, aspect) whatever the switches say. */
static struct nv2a_tex_pack_entry *lookup(uint64_t hash, uint32_t guest_w, uint32_t guest_h)
{
    unsigned lo = 0, hi = s_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (s_entries[mid].hash < hash)
            lo = mid + 1;
        else if (s_entries[mid].hash > hash)
            hi = mid;
        else {
            struct nv2a_tex_pack_entry *e = &s_entries[mid];
            /* The hash covers the bytes, not the shape: a 64x16 and a 32x32
             * texture of the same bytes (a uniform fill) hash alike, so an
             * entry of another aspect is some other texture. */
            if ((uint64_t)e->width * guest_h != (uint64_t)e->height * guest_w)
                return NULL;
            return e;
        }
    }
    return NULL;
}

struct nv2a_tex_pack_entry *nv2a_tex_pack_find(uint64_t hash, uint32_t guest_w,
                                               uint32_t guest_h)
{
    unsigned m = atomic_load_explicit(&s_mask, memory_order_relaxed);
    struct nv2a_tex_pack_entry *e;
    if (!(m & PACK_ON))
        return NULL;
    e = lookup(hash, guest_w, guest_h);
    if (!e || !(m & (1u << e->kind)))
        return NULL;
    s_hits++;
    return e;
}

const uint32_t *nv2a_tex_pack_texels(struct nv2a_tex_pack_entry *e)
{
    FILE *f;
    uint32_t *data;
    if (!e)
        return NULL;
    if (e->data)
        return e->data;
    /* bytes is a multiple of 4 either way: texels * 4, or 16 per block. */
    data = (uint32_t *)malloc(e->bytes);
    if (!data)
        return NULL;
    f = s_path ? fopen(s_path, "rb") : NULL;
    if (!f || pack_seek(f, e->offset) != 0 || fread(data, 1u, e->bytes, f) != e->bytes) {
        /* Said for the first four failures only: the guest texels are
         * uploaded instead, and the next upload tries again. */
        static unsigned said;
        if (said++ < 4)
            fprintf(stderr, "[ENHANCE] textures.pack: cannot read entry %016llX from %s\n",
                    (unsigned long long)e->hash, s_path ? s_path : "?");
        if (f)
            fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (!e->bc3) {  /* BC3 blocks are bytes in GPU order: never swapped */
        size_t i;
        for (i = 0; i < e->texels; i++) {
            uint32_t v = data[i];
            data[i] = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
        }
    }
#endif
    e->data = data;
    return data;
}

/* ---- Misses ------------------------------------------------------------- */

void nv2a_tex_pack_miss(uint64_t hash, uint32_t w, uint32_t h, uint32_t color,
                        uint32_t levels)
{
    struct nv2a_tex_pack_miss *m;
    const struct nv2a_tex_pack_entry *e = lookup(hash, w, h);
    unsigned mask = atomic_load_explicit(&s_mask, memory_order_relaxed);
    uint32_t why = NV2A_TEX_PACK_MISS_ABSENT;
    unsigned i;

    /* The pack holds it but it did not go up: switched off, or the backend
     * could not use it (a device without BC3, a texture too large, a
     * failed read). The trace says which, so an owner does not rebuild a
     * pack that is fine. */
    if (e)
        why = !(mask & PACK_ON)              ? NV2A_TEX_PACK_MISS_PACK_OFF
            : !(mask & (1u << e->kind))      ? NV2A_TEX_PACK_MISS_KIND_OFF
            :                                  NV2A_TEX_PACK_MISS_REFUSED;
    for (i = 0; i < s_miss_n; i++) {
        m = &s_miss[i];
        if (m->hash == hash && m->width == w && m->height == h && m->color == color) {
            m->count++;
            m->why = why;
            return;
        }
    }
    /* Full: the oldest distinct miss gives way. A texture still in use
     * misses again on its next upload and comes back. */
    m = &s_miss[s_miss_next];
    s_miss_next = (s_miss_next + 1u) % NV2A_TEX_PACK_MISSES;
    if (s_miss_n < NV2A_TEX_PACK_MISSES)
        s_miss_n++;
    m->hash = hash;
    m->width = w;
    m->height = h;
    m->color = color;
    m->levels = levels;
    m->count = 1;
    m->why = why;
    m->kind = e ? e->kind : 0;
    /* What a pack can hold: the hash is over guest bytes the CLI also
     * reads from the dump's image files (swizzled or DXT, one level). A
     * linear or mipmapped upload is a render target, a movie frame or a
     * generated texture, which no file in the dump holds. */
    m->candidate = (d3d8_format_is_swizzled(color) || d3d8_format_dxt_block_bytes(color))
                && levels <= 1;
}

static int miss_cmp(const void *a, const void *b)
{
    const struct nv2a_tex_pack_miss *x = (const struct nv2a_tex_pack_miss *)a;
    const struct nv2a_tex_pack_miss *y = (const struct nv2a_tex_pack_miss *)b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return x->hash < y->hash ? -1 : x->hash > y->hash;
}

unsigned nv2a_tex_pack_misses(struct nv2a_tex_pack_miss *out, unsigned n)
{
    struct nv2a_tex_pack_miss all[NV2A_TEX_PACK_MISSES];
    unsigned k = s_miss_n;
    if (!out || !n)
        return 0;
    memcpy(all, s_miss, sizeof(all[0]) * k);
    qsort(all, k, sizeof all[0], miss_cmp);
    if (n > k)
        n = k;
    memcpy(out, all, sizeof(all[0]) * n);
    return n;
}

static const char *fmt_name(uint32_t color, char *buf, size_t len)
{
    switch (color) {
    case 0x0C: return "DXT1";
    case 0x0E: return "DXT3";
    case 0x0F: return "DXT5";
    default:
        snprintf(buf, len, "fmt%02X", color & 0xFFu);
        return buf;
    }
}

/* " (text off)" and the like, or "" for a texture the pack lacks. */
static const char *why_name(const struct nv2a_tex_pack_miss *m)
{
    static const char *const kind_off[XRTP_KIND_COUNT] = {
        " (art off)", " (font off)", " (text off)",
    };
    switch (m->why) {
    case NV2A_TEX_PACK_MISS_PACK_OFF: return " (pack off)";
    case NV2A_TEX_PACK_MISS_KIND_OFF:
        return m->kind < XRTP_KIND_COUNT ? kind_off[m->kind] : " (kind off)";
    case NV2A_TEX_PACK_MISS_REFUSED:  return " (entry refused)";
    default:                          return "";
    }
}

void nv2a_tex_pack_print_misses(void)
{
    struct nv2a_tex_pack_miss m[NV2A_TEX_PACK_MISSES];
    unsigned n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES), i;
    char buf[16];
    for (i = 0; i < n; i++)
        fprintf(stderr, "[TEXPACK] miss %016llX %ux%u %s x%u %s%s\n",
                (unsigned long long)m[i].hash, m[i].width, m[i].height,
                fmt_name(m[i].color, buf, sizeof buf), m[i].count,
                m[i].candidate ? "candidate" : "linear/mip", why_name(&m[i]));
}
