/*
 * tex_pack: the texture pack reader (nv2a_tex_pack.h).
 *
 *   write     a pack of two entries (a 2x2 single level, a 4x2 with two
 *             levels) written by this test, loaded: count, scale, both
 *             found, a miss, an entry of another aspect than the guest
 *             texture's not taken, texels read back, hits counted
 *   refuse    bad magic, bad version, index past the end, an entry past
 *             the end, an unsorted index, a truncated file, more levels
 *             than the size allows, non-zero reserved header bytes or entry
 *             flags, a missing file: each refused with a reason, and nothing
 *             loaded; the most levels the size allows still loads
 *   hash      nv2a_tex_hash of a fixed byte pattern (1034 bytes: 0..255 four
 *             times, then "xboxrecomp") is 0x0B060D91FB09F186, the vector
 *             the CLI's Python port (xboxrecomp_cli.enhance.texhash) checks;
 *             the empty and the 64-zero-byte cases too
 *   v2        kind flags read and counted, the text scale, a kind or the
 *             whole pack switched off is a miss and bumps the generation
 *             (once per change); a miss records why (absent, kind off,
 *             pack off, held but refused); unknown flag bits, kind 3 and
 *             the v2 reserved word refused
 *   bc3       a BC3 entry's payload is its block count x 16, bounds-checked
 *             and read as it is; the block the CLI's test_bc3_vector encodes
 *             decodes through d3d8_dxt_decode_texel to its texels (alpha
 *             exact, colour within the one level the expansions differ); a
 *             size that is not whole blocks is refused
 *   misses    the ring keeps distinct (hash, size, format) with counts,
 *             sorted by count; candidates are swizzled or DXT one-level
 *             uploads; 64 at most, the oldest giving way; unload clears it
 *
 * The cases before v2 build version 1 packs, which the reader still takes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"
#include "d3d8_swizzle.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

#define HASH_A 0x1111111111111111ull
#define HASH_B 0x2222222222222222ull
#define TEXELS_A 4u                 /* 2x2, one level */
#define TEXELS_B (8u + 2u)          /* 4x2 then 2x1 */

/* The pack image: header, index, payload A, payload B. */
static size_t build_pack(uint8_t *buf, uint64_t hash_a, uint64_t hash_b, uint32_t version)
{
    size_t idx = NV2A_TEX_PACK_HDR, pay_a = idx + 2 * NV2A_TEX_PACK_ENTRY;
    size_t pay_b = pay_a + TEXELS_A * 4, end = pay_b + TEXELS_B * 4;
    uint32_t i;
    memset(buf, 0, end);
    memcpy(buf, NV2A_TEX_PACK_MAGIC, 4);
    wr32(buf + 4, version);
    wr32(buf + 8, 2);
    wr32(buf + 12, 4);
    wr64(buf + 16, idx);
    wr64(buf + idx, hash_a);
    wr32(buf + idx + 8, 2);
    wr32(buf + idx + 12, 2);
    wr32(buf + idx + 16, 1);
    wr64(buf + idx + 24, pay_a);
    wr64(buf + idx + 32, hash_b);
    wr32(buf + idx + 40, 4);
    wr32(buf + idx + 44, 2);
    wr32(buf + idx + 48, 2);
    wr64(buf + idx + 56, pay_b);
    for (i = 0; i < TEXELS_A; i++)
        wr32(buf + pay_a + i * 4, 0xFF000000u | (i * 0x010101u));
    for (i = 0; i < TEXELS_B; i++)
        wr32(buf + pay_b + i * 4, 0x80000000u | (0x100u * i));
    return end;
}

static void write_file(const char *path, const uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fails++; printf("FAIL cannot write %s\n", path); return; }
    fwrite(buf, 1, n, f);
    fclose(f);
}

static void test_write(void)
{
    uint8_t buf[512];
    char err[128] = "";
    size_t n = build_pack(buf, HASH_A, HASH_B, 1);
    struct nv2a_tex_pack_entry *e;
    const uint32_t *px;
    int rc;

    write_file("good.pack", buf, n);
    rc = nv2a_tex_pack_load("good.pack", err, sizeof err);
    CHECK(rc == 2, "load: %d (%s)", rc, err);
    CHECK(nv2a_tex_pack_count() == 2, "count %u", nv2a_tex_pack_count());
    CHECK(nv2a_tex_pack_scale() == 4, "scale %u", nv2a_tex_pack_scale());
    CHECK(nv2a_tex_pack_find(0x3333333333333333ull, 1, 1) == NULL, "miss found something");
    CHECK(nv2a_tex_pack_hits() == 0, "a miss counted as a hit");
    e = nv2a_tex_pack_find(HASH_A, 1, 1);         /* a 1x1 guest at 2x */
    CHECK(e && e->width == 2 && e->height == 2 && e->levels == 1 && e->texels == TEXELS_A,
          "entry A");
    px = nv2a_tex_pack_texels(e);
    CHECK(px && px[0] == 0xFF000000u && px[3] == 0xFF030303u, "texels A");
    CHECK(px == nv2a_tex_pack_texels(e), "texels A read twice");
    /* B is 4x2: a square guest texture of the same hash is something else. */
    CHECK(nv2a_tex_pack_find(HASH_B, 2, 2) == NULL, "entry B taken for a square guest");
    CHECK(nv2a_tex_pack_hits() == 1, "an aspect mismatch counted as a hit");
    e = nv2a_tex_pack_find(HASH_B, 2, 1);
    CHECK(e && e->width == 4 && e->height == 2 && e->levels == 2 && e->texels == TEXELS_B,
          "entry B");
    px = nv2a_tex_pack_texels(e);
    CHECK(px && px[0] == 0x80000000u && px[9] == 0x80000900u, "texels B");
    CHECK(nv2a_tex_pack_hits() == 2, "hits %llu", (unsigned long long)nv2a_tex_pack_hits());
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_count() == 0, "unload");
}

static void refuse(const char *path, const uint8_t *buf, size_t n, const char *want)
{
    char err[128] = "";
    int rc;
    write_file(path, buf, n);
    rc = nv2a_tex_pack_load(path, err, sizeof err);
    CHECK(rc == -1, "%s: loaded (%d)", path, rc);
    CHECK(strstr(err, want) != NULL, "%s: reason '%s', want '%s'", path, err, want);
    CHECK(nv2a_tex_pack_count() == 0, "%s: count %u after refusal", path, nv2a_tex_pack_count());
}

static void test_refuse(void)
{
    uint8_t buf[512];
    char err[128] = "";
    size_t n;

    n = build_pack(buf, HASH_A, HASH_B, 1);
    buf[0] = 'Y';
    refuse("magic.pack", buf, n, "magic");

    n = build_pack(buf, HASH_A, HASH_B, 7);
    refuse("version.pack", buf, n, "version");

    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr32(buf + 8, 100);
    refuse("index.pack", buf, n, "index outside");

    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr64(buf + NV2A_TEX_PACK_HDR + 56, n - 4);      /* B's texels past the end */
    refuse("entry.pack", buf, n, "outside the file");

    /* Offsets near 2^64: a sum would wrap to a small value inside the file. */
    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr64(buf + 16, ~0ull - 31);
    refuse("index_wrap.pack", buf, n, "index outside");

    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr64(buf + NV2A_TEX_PACK_HDR + 56, ~0ull - 3);
    refuse("entry_wrap.pack", buf, n, "outside the file");

    n = build_pack(buf, HASH_B, HASH_A, 1);
    refuse("unsorted.pack", buf, n, "sorted");

    n = build_pack(buf, HASH_A, HASH_B, 1);
    refuse("short.pack", buf, NV2A_TEX_PACK_HDR + 10, "index outside");
    refuse("stub.pack", buf, 8, "too short");

    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr32(buf + NV2A_TEX_PACK_HDR + 8, 0);            /* A's width 0 */
    refuse("size.pack", buf, n, "bad size");

    /* 2x2 allows two levels (2x2, 1x1); a third would be another 1x1,
     * which D3D11 refuses and Metal reports as invalid. */
    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr32(buf + NV2A_TEX_PACK_HDR + 16, 3);
    refuse("levels.pack", buf, n, "bad size");
    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr32(buf + NV2A_TEX_PACK_HDR + 16, 2);           /* A's 5 texels run into B's */
    write_file("levels_max.pack", buf, n);
    CHECK(nv2a_tex_pack_load("levels_max.pack", err, sizeof err) == 2,
          "levels at the limit refused: %s", err);
    nv2a_tex_pack_unload();

    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr64(buf + 24, 1);
    refuse("reserved.pack", buf, n, "reserved");

    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr32(buf + NV2A_TEX_PACK_HDR + 32 + 20, 1);      /* B's flags */
    refuse("flags.pack", buf, n, "flags");

    CHECK(nv2a_tex_pack_load("does-not-exist.pack", err, sizeof err) == -1
          && strstr(err, "open") != NULL, "missing file: '%s'", err);
    CHECK(nv2a_tex_pack_load("", err, sizeof err) == -1, "empty path loaded");
}

static void test_hash(void)
{
    uint8_t v[1034];
    int i;
    for (i = 0; i < 1024; i++)
        v[i] = (uint8_t)i;
    memcpy(v + 1024, "xboxrecomp", 10);
    CHECK(nv2a_tex_hash(v, sizeof v, NV2A_TEX_HASH_SEED) == 0x0B060D91FB09F186ull,
          "vector: %016llX", (unsigned long long)nv2a_tex_hash(v, sizeof v, NV2A_TEX_HASH_SEED));
    CHECK(nv2a_tex_hash(v, 0, NV2A_TEX_HASH_SEED) == 0x40500CA3D9E617F3ull, "empty");
    memset(v, 0, 64);
    CHECK(nv2a_tex_hash(v, 64, NV2A_TEX_HASH_SEED) == 0x69597731194007ABull, "zeros");
}

/* B is entry 1: its flags at index + 32 + 20. */
#define FLAGS_A (NV2A_TEX_PACK_HDR + 20)
#define FLAGS_B (NV2A_TEX_PACK_HDR + 32 + 20)

static void test_v2(void)
{
    uint8_t buf[512];
    char err[128] = "";
    size_t n = build_pack(buf, HASH_A, HASH_B, 2);
    struct nv2a_tex_pack_entry *e;
    uint32_t g;
    int rc;

    wr32(buf + FLAGS_A, XRTP_KIND_FONT);
    wr32(buf + FLAGS_B, XRTP_KIND_TEXT);
    wr32(buf + 24, 2);                               /* text at 2x */
    write_file("v2.pack", buf, n);
    rc = nv2a_tex_pack_load("v2.pack", err, sizeof err);
    CHECK(rc == 2, "v2 load: %d (%s)", rc, err);
    CHECK(nv2a_tex_pack_kind_count(XRTP_KIND_ART) == 0
          && nv2a_tex_pack_kind_count(XRTP_KIND_FONT) == 1
          && nv2a_tex_pack_kind_count(XRTP_KIND_TEXT) == 1, "kind counts");
    CHECK(nv2a_tex_pack_kind_count(7) == 0, "kind 7 counted");
    CHECK(nv2a_tex_pack_scale() == 4 && nv2a_tex_pack_text_scale() == 2,
          "scales %u, %u", nv2a_tex_pack_scale(), nv2a_tex_pack_text_scale());
    CHECK(nv2a_tex_pack_bc3_count() == 0, "bc3 count");
    e = nv2a_tex_pack_find(HASH_A, 1, 1);
    CHECK(e && e->kind == XRTP_KIND_FONT && !e->bc3 && e->bytes == TEXELS_A * 4, "entry A kind");
    e = nv2a_tex_pack_find(HASH_B, 2, 1);
    CHECK(e && e->kind == XRTP_KIND_TEXT, "entry B kind");

    /* Text off: B misses, A still hits; one bump per change, none for a
     * store of the same value. */
    g = nv2a_tex_pack_gen();
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_TEXT, 0);
    CHECK(nv2a_tex_pack_gen() == g + 1, "kind off: gen %u -> %u", g, nv2a_tex_pack_gen());
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_TEXT, 0);
    CHECK(nv2a_tex_pack_gen() == g + 1, "the same kind store bumped the gen");
    CHECK(nv2a_tex_pack_find(HASH_B, 2, 1) == NULL, "text off: B found");
    CHECK(nv2a_tex_pack_find(HASH_A, 1, 1) != NULL, "text off: A lost");
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_COUNT, 0);
    CHECK(nv2a_tex_pack_gen() == g + 1, "an unknown kind changed the mask");
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_TEXT, 1);
    CHECK(nv2a_tex_pack_find(HASH_B, 2, 1) != NULL, "text back on: B missing");
    /* The whole pack off: nothing hits, whatever the kinds. */
    nv2a_tex_pack_set_enabled(0);
    CHECK(nv2a_tex_pack_gen() == g + 3, "pack off: gen %u", nv2a_tex_pack_gen());
    CHECK(nv2a_tex_pack_find(HASH_A, 1, 1) == NULL, "pack off: A found");
    nv2a_tex_pack_set_enabled(1);
    CHECK(nv2a_tex_pack_find(HASH_A, 1, 1) != NULL, "pack back on: A missing");
    CHECK(nv2a_tex_pack_gen() == g + 4, "pack on: gen %u", nv2a_tex_pack_gen());

    /* Why each miss went up: absent, a kind off, the pack off, or held
     * and on (the backend refused it); the latest reason wins. */
    {
        struct nv2a_tex_pack_miss m[NV2A_TEX_PACK_MISSES];
        unsigned k, got = 0;
        nv2a_tex_pack_miss(0x77ull, 1, 1, 0x0F, 1);
        nv2a_tex_pack_set_kind_enabled(XRTP_KIND_TEXT, 0);
        nv2a_tex_pack_miss(HASH_B, 2, 1, 0x0E, 1);
        nv2a_tex_pack_set_kind_enabled(XRTP_KIND_TEXT, 1);
        nv2a_tex_pack_set_enabled(0);
        nv2a_tex_pack_miss(HASH_A, 1, 1, 0x0E, 1);
        nv2a_tex_pack_set_enabled(1);
        nv2a_tex_pack_miss(HASH_A, 2, 2, 0x0E, 1);
        nv2a_tex_pack_miss(HASH_A, 2, 2, 0x0E, 1);
        n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES);
        for (k = 0; k < n; k++) {
            if (m[k].hash == 0x77)
                got |= (m[k].why == NV2A_TEX_PACK_MISS_ABSENT) << 0;
            else if (m[k].hash == HASH_B)
                got |= (m[k].why == NV2A_TEX_PACK_MISS_KIND_OFF
                        && m[k].kind == XRTP_KIND_TEXT) << 1;
            else if (m[k].width == 1)
                got |= (m[k].why == NV2A_TEX_PACK_MISS_PACK_OFF) << 2;
            else
                got |= (m[k].why == NV2A_TEX_PACK_MISS_REFUSED && m[k].count == 2) << 3;
        }
        CHECK(n == 4 && got == 0xF, "miss reasons: %u misses, mask %X", (unsigned)n, got);
        nv2a_tex_pack_print_misses();
    }
    nv2a_tex_pack_unload();

    /* A v2 pack with no text scale: the header scale. */
    n = build_pack(buf, HASH_A, HASH_B, 2);
    write_file("v2_noscale.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v2_noscale.pack", err, sizeof err) == 2
          && nv2a_tex_pack_text_scale() == 4, "text scale default %u (%s)",
          nv2a_tex_pack_text_scale(), err);
    CHECK(nv2a_tex_pack_kind_count(XRTP_KIND_ART) == 2, "flags 0 is art");
    nv2a_tex_pack_unload();

    n = build_pack(buf, HASH_A, HASH_B, 2);
    wr32(buf + FLAGS_B, 0x4);                        /* bit 2: no meaning */
    refuse("v2_bit2.pack", buf, n, "unknown flags");
    n = build_pack(buf, HASH_A, HASH_B, 2);
    wr32(buf + FLAGS_B, 3);
    refuse("v2_kind3.pack", buf, n, "unknown kind");
    n = build_pack(buf, HASH_A, HASH_B, 2);
    wr32(buf + 28, 1);
    refuse("v2_reserved.pack", buf, n, "reserved");
    /* v1 has no kinds: the same flags are refused there. */
    n = build_pack(buf, HASH_A, HASH_B, 1);
    wr32(buf + FLAGS_B, XRTP_KIND_TEXT);
    refuse("v1_kind.pack", buf, n, "flags");
}

/* xboxrecomp-cli tests/test_enhance.py BC3_P / BC3_B / BC3_D: RGBA texels
 * in row order, the CLI encoder's block, what a DXT5 decoder draws. */
static const uint8_t BC3_B[16] = {
    0xff, 0x00, 0xf9, 0xed, 0x94, 0xe4, 0x26, 0x01,
    0xff, 0xe9, 0x80, 0x17, 0xd5, 0xef, 0xbb, 0xb2,
};
static const uint8_t BC3_D[64] = {
    0x10, 0xf3, 0x00, 0x00, 0x10, 0xf3, 0x00, 0x24, 0x10, 0xf3, 0x00, 0x24, 0x5a, 0xb6, 0x55, 0x48,
    0x5a, 0xb6, 0x55, 0x48, 0x5a, 0xb6, 0x55, 0x00, 0xa4, 0x79, 0xaa, 0x6d, 0x5a, 0xb6, 0x55, 0x91,
    0x5a, 0xb6, 0x55, 0x91, 0xa4, 0x79, 0xaa, 0x91, 0x5a, 0xb6, 0x55, 0xb6, 0xa4, 0x79, 0xaa, 0xb6,
    0xa4, 0x79, 0xaa, 0xda, 0xef, 0x3c, 0xff, 0xda, 0x5a, 0xb6, 0x55, 0xff, 0xa4, 0x79, 0xaa, 0xff,
};

#define HASH_C 0x3333333333333333ull

/* One BC3 entry of w x h (the vector block repeated), header, index, blocks. */
static size_t build_bc3(uint8_t *buf, uint32_t w, uint32_t h)
{
    size_t idx = NV2A_TEX_PACK_HDR, pay = idx + NV2A_TEX_PACK_ENTRY;
    size_t bytes = nv2a_tex_pack_bc3_bytes(w, h, 1), i;
    memset(buf, 0, pay + bytes);
    memcpy(buf, NV2A_TEX_PACK_MAGIC, 4);
    wr32(buf + 4, 2);
    wr32(buf + 8, 1);
    wr32(buf + 12, 4);
    wr64(buf + 16, idx);
    wr32(buf + 24, 2);
    wr64(buf + idx, HASH_C);
    wr32(buf + idx + 8, w);
    wr32(buf + idx + 12, h);
    wr32(buf + idx + 16, 1);
    wr32(buf + idx + 20, XRTP_KIND_TEXT | XRTP_FLAG_BC3);
    wr64(buf + idx + 24, pay);
    for (i = 0; i < bytes; i += 16)
        memcpy(buf + pay + i, BC3_B, 16);
    return pay + bytes;
}

static void test_bc3(void)
{
    uint8_t buf[512];
    char err[128] = "";
    struct nv2a_tex_pack_entry *e;
    const uint8_t *px;
    uint32_t k, argb, want;
    int rc;
    size_t n;

    CHECK(nv2a_tex_pack_bc3_bytes(4, 4, 1) == 16 && nv2a_tex_pack_bc3_bytes(8, 4, 1) == 32
          && nv2a_tex_pack_bc3_bytes(6, 2, 1) == 32
          && nv2a_tex_pack_bc3_bytes(8, 8, 2) == 64 + 16, "bc3 byte counts");

    n = build_bc3(buf, 8, 4);
    write_file("bc3.pack", buf, n);
    rc = nv2a_tex_pack_load("bc3.pack", err, sizeof err);
    CHECK(rc == 1, "bc3 load: %d (%s)", rc, err);
    CHECK(nv2a_tex_pack_bc3_count() == 1 && nv2a_tex_pack_kind_count(XRTP_KIND_TEXT) == 1,
          "bc3 counts");
    e = nv2a_tex_pack_find(HASH_C, 4, 2);
    CHECK(e && e->bc3 && e->kind == XRTP_KIND_TEXT && e->bytes == 32, "bc3 entry");
    px = e ? (const uint8_t *)nv2a_tex_pack_texels(e) : NULL;
    CHECK(px && memcmp(px, BC3_B, 16) == 0 && memcmp(px + 16, BC3_B, 16) == 0,
          "bc3 bytes not as stored");
    /* BC3_D is what a bit-replicating decoder draws (the CLI's, the GPUs');
     * this one expands 5:6:5 as v * 255 / max, at most one level lower. */
    for (k = 0; px && k < 16; k++) {
        const uint8_t *d = BC3_D + 4 * k;
        uint32_t c, off = 0;
        want = ((uint32_t)d[3] << 24) | ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
        argb = 0;
        CHECK(d3d8_dxt_decode_texel(px, 0x0F, k & 3u, k >> 2, 8, &argb), "decode %u", k);
        for (c = 0; c < 4; c++) {
            int x = (int)((argb >> (8 * c)) & 0xFFu), y = (int)((want >> (8 * c)) & 0xFFu);
            if (x - y > 1 || y - x > 1 || (c == 3 && x != y))
                off = 1;
        }
        CHECK(!off, "vector texel %u: %08X, want %08X", k, argb, want);
    }
    nv2a_tex_pack_unload();

    n = build_bc3(buf, 6, 4);
    refuse("bc3_odd.pack", buf, n, "multiple of 4");
    n = build_bc3(buf, 8, 4);
    refuse("bc3_short.pack", buf, n - 1, "outside the file");
}

static void test_misses(void)
{
    struct nv2a_tex_pack_miss m[NV2A_TEX_PACK_MISSES + 4];
    unsigned i, n;

    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES) == 0, "misses before any");
    nv2a_tex_pack_miss(0xAAull, 64, 32, 0x0F, 1);     /* DXT5: a candidate */
    nv2a_tex_pack_miss(0xBBull, 64, 64, 0x12, 1);     /* linear A8R8G8B8 */
    nv2a_tex_pack_miss(0xAAull, 64, 32, 0x0F, 1);
    nv2a_tex_pack_miss(0xAAull, 64, 32, 0x0F, 1);
    nv2a_tex_pack_miss(0xAAull, 32, 32, 0x0F, 1);     /* another size: distinct */
    nv2a_tex_pack_miss(0xCCull, 16, 16, 0x06, 1);     /* swizzled A8R8G8B8 */
    nv2a_tex_pack_miss(0xDDull, 16, 16, 0x0C, 5);     /* mipmapped DXT1 */
    nv2a_tex_pack_miss(0xCCull, 16, 16, 0x06, 1);
    n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES);
    CHECK(n == 5, "distinct misses %u", n);
    CHECK(m[0].hash == 0xAA && m[0].width == 64 && m[0].count == 3 && m[0].candidate,
          "top miss %llX x%u", (unsigned long long)m[0].hash, m[0].count);
    CHECK(m[1].hash == 0xCC && m[1].count == 2 && m[1].candidate, "second miss");
    for (i = 2; i < n; i++) {
        CHECK(m[i].count == 1, "miss %u count %u", i, m[i].count);
        if (m[i].hash == 0xBB || m[i].hash == 0xDD)
            CHECK(!m[i].candidate, "%llX taken for a candidate", (unsigned long long)m[i].hash);
        else
            CHECK(m[i].candidate && m[i].width == 32, "the 32x32 DXT5 miss");
    }
    CHECK(nv2a_tex_pack_misses(m, 2) == 2 && m[0].hash == 0xAA, "a short read");
    nv2a_tex_pack_print_misses();

    /* 70 more distinct: the ring holds 64, the oldest gone first. */
    for (i = 0; i < 70; i++)
        nv2a_tex_pack_miss(0x1000ull + i, 8, 8, 0x0C, 1);
    n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES + 4);
    CHECK(n == NV2A_TEX_PACK_MISSES, "full ring %u", n);
    for (i = 0; i < n; i++)
        CHECK(m[i].hash >= 0x1000ull + 70 - NV2A_TEX_PACK_MISSES, "kept %llX",
              (unsigned long long)m[i].hash);
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES) == 0, "unload kept the misses");
}

int main(void)
{
    test_write();
    test_refuse();
    test_hash();
    test_v2();
    test_bc3();
    test_misses();
    if (fails) {
        printf("tex_pack: %d failure(s)\n", fails);
        return 1;
    }
    printf("tex_pack: ok\n");
    return 0;
}
