/*
 * tex_pack: the texture pack reader (nv2a_tex_pack.h).
 *
 *   write     a pack of two entries (a 2x2 single level, a 4x2 with two
 *             levels) written by this test, loaded: count, scale, both
 *             found, a miss, an entry of another aspect than the guest
 *             texture's not taken, texels read back, hits counted;
 *             nv2a_tex_pack_is_file takes the file, not a folder named
 *             *.pack nor a missing name; a UTF-8 file name loads
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
 *             pack off, held but refused); unknown flag bits, kind 3, BC1
 *             and the v2 reserved word refused
 *   v3        kind 3 counted, found and switched alone; a BC1 entry's payload
 *             is its block count x 8, read as it is, the block test_bc1_vector
 *             in the CLI encodes decoding through d3d8_dxt_decode_texel; the
 *             word at +28 not refused; bit 6 and both block flags refused; v1
 *             and v2 still read
 *   v4        bit 6 (XRTP_FLAG_DETILE_HEX) read and counted on a world
 *             entry, refused on any other kind; v3 still reads; bit 7
 *             refused, alone or with bit 6, and version 7
 *   v5        bit 7 (XRTP_FLAG_RECOLOUR) read and counted; find never takes
 *             a recolour entry and find_recolour only one, each under the
 *             switches and the aspect rule; the recolour test vector of
 *             texture-pack.md, through the pack under two palettes; release
 *             keeps a recolour payload; a block format on one refused;
 *             with bit 6 on a world entry it loads and keeps both; the
 *             two-lane blend equals (c0 * (255 - w) + c1 * w + 127) / 255
 *             for every c0, c1, w on every channel
 *   v6        bit 8 (XRTP_FLAG_MIP_BLEND) read, kept on the entry and
 *             counted, cleared by unload; refused under v5; v5 still reads
 *   volatile  a plane taking more than BURST new palettes within WINDOW
 *             flips is volatile (find_recolour NULL, the generation bumped,
 *             a known palette refused too, the miss tagged), and recolours
 *             again from the flip that ends QUIET flips without a new one,
 *             which bumps the generation; one new palette every 3 flips never
 *             is, nor a burst of re-uploads right after a bump; a recolour
 *             entry switched off by its kind is a "kind off" miss
 *   volatile cycles  an 8-step and a 16-step palette cycle (palettes it has
 *             seen) turn volatile and stay so while they play; a recurring
 *             fade's return waits 2x longer each time, up to QUIET_MAX;
 *             editor picks every 10 flips stay recoloured
 *   recolour buffer  counts in the resident bytes; a buffer past
 *             NV2A_TEX_PACK_RECOLOUR_KEEP is freed at the release
 *   release   frees the payload, a later read brings back the same bytes;
 *             under set_keep it does nothing
 *   bc3       a BC3 entry's payload is its block count x 16, bounds-checked
 *             and read as it is; the block the CLI's test_bc3_vector encodes
 *             decodes through d3d8_dxt_decode_texel to its texels (alpha
 *             exact, colour within the one level the expansions differ); a
 *             size that is not whole blocks is refused
 *   misses    the ring keeps distinct (hash, size, format) with counts,
 *             sorted by count; candidates are swizzled or DXT uploads,
 *             palettes and mips included; 64 at most, the oldest giving way; unload clears it
 *   resident  with a budget that holds one entry, reading the other frees
 *             the least recently used; a freed entry reads back the same
 *             texels; a use moves an entry to the front; an entry over the
 *             budget is still read and held alone; the default budget
 *             frees nothing; unload clears the count; a pack that cannot
 *             be opened, or was rewritten since the load (another size, or
 *             the same size with another mtime), is not read and keeps the
 *             held entries
 *
 * The cases before v2 build version 1 packs, which the reader still takes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <sys/utime.h>
#define utime _utime
#define utimbuf _utimbuf
#define MKDIR(p) _mkdir(p)
#define rmdir _rmdir
#else
#include <unistd.h>
#include <utime.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

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

    /* The menu lists only a regular file: a folder named *.pack, or a
     * name with nothing there, is not one. */
    CHECK(nv2a_tex_pack_is_file("good.pack"), "good.pack not a file");
    MKDIR("dir.pack");
    CHECK(!nv2a_tex_pack_is_file("dir.pack"), "a folder taken for a pack");
    CHECK(!nv2a_tex_pack_is_file("absent.pack"), "a missing file taken for a pack");
    rmdir("dir.pack");

    /* A UTF-8 name (a Windows user folder with an accent): opened wide. */
    {
        static const char utf8[] = "caf\xC3\xA9.pack";
#ifdef _WIN32
        FILE *f = _wfopen(L"caf\u00E9.pack", L"wb");
        if (f) { fwrite(buf, 1, n, f); fclose(f); }
#else
        write_file(utf8, buf, n);
#endif
        CHECK(nv2a_tex_pack_is_file(utf8), "UTF-8 name not found");
        rc = nv2a_tex_pack_load(utf8, err, sizeof err);
        CHECK(rc == 2, "UTF-8 name: %d (%s)", rc, err);
        nv2a_tex_pack_unload();
        remove(utf8);
#ifdef _WIN32
        _wremove(L"caf\u00E9.pack");
#endif
    }
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
    CHECK(e && e->kind == XRTP_KIND_FONT && e->bc == XRTP_BC_NONE && e->bytes == TEXELS_A * 4, "entry A kind");
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
    CHECK(e && e->bc == XRTP_BC_3 && e->kind == XRTP_KIND_TEXT && e->bytes == 32, "bc3 entry");
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

/* A BC1 world entry and a texel one, v3: a 4x4 opaque block whose first row
 * is pure red and the other three pure blue, the vector the CLI's writer tests
 * are to share, so both ends read one layout. */
static const uint8_t BC1_B[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0x55, 0x55 };

static size_t build_v3(uint8_t *buf, uint32_t flags, uint32_t w, uint32_t h)
{
    size_t idx = NV2A_TEX_PACK_HDR, pay = idx + NV2A_TEX_PACK_ENTRY;
    size_t bytes = nv2a_tex_pack_bc1_bytes(w, h, 1), i;
    memset(buf, 0, pay + bytes);
    memcpy(buf, NV2A_TEX_PACK_MAGIC, 4);
    wr32(buf + 4, 3);
    wr32(buf + 8, 1);
    wr32(buf + 12, 2);
    wr64(buf + 16, idx);
    wr32(buf + 24, 2);
    wr32(buf + 28, 1u << XRTP_KIND_WORLD);
    wr64(buf + idx, HASH_C);
    wr32(buf + idx + 8, w);
    wr32(buf + idx + 12, h);
    wr32(buf + idx + 16, 1);
    wr32(buf + idx + 20, flags);
    wr64(buf + idx + 24, pay);
    for (i = 0; i < bytes; i += 8)
        memcpy(buf + pay + i, BC1_B, 8);
    return pay + bytes;
}

static void test_v3(void)
{
    uint8_t buf[512];
    char err[128] = "";
    struct nv2a_tex_pack_entry *e;
    const uint8_t *px;
    uint32_t argb = 0, g;
    int rc;
    size_t n;

    CHECK(nv2a_tex_pack_bc1_bytes(4, 4, 1) == 8 && nv2a_tex_pack_bc1_bytes(8, 4, 1) == 16
          && nv2a_tex_pack_bc1_bytes(6, 2, 1) == 16
          && nv2a_tex_pack_bc1_bytes(8, 8, 2) == 32 + 8, "bc1 byte counts");

    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1, 8, 4);
    write_file("v3.pack", buf, n);
    rc = nv2a_tex_pack_load("v3.pack", err, sizeof err);
    CHECK(rc == 1, "v3 load: %d (%s)", rc, err);
    CHECK(nv2a_tex_pack_version() == 3, "version %u", nv2a_tex_pack_version());
    CHECK(nv2a_tex_pack_bc1_count() == 1 && nv2a_tex_pack_bc3_count() == 0
          && nv2a_tex_pack_kind_count(XRTP_KIND_WORLD) == 1
          && nv2a_tex_pack_kind_count(XRTP_KIND_ART) == 0, "v3 counts");
    e = nv2a_tex_pack_find(HASH_C, 4, 2);
    CHECK(e && e->bc == XRTP_BC_1 && e->kind == XRTP_KIND_WORLD && e->bytes == 16, "bc1 entry");
    px = e ? (const uint8_t *)nv2a_tex_pack_texels(e) : NULL;
    CHECK(px && memcmp(px, BC1_B, 8) == 0 && memcmp(px + 8, BC1_B, 8) == 0,
          "bc1 bytes not as stored");
    /* Row 0 is index 0, the first endpoint; the rest index 1, the second. */
    CHECK(px && d3d8_dxt_decode_texel(px, 0x0C, 0, 0, 8, &argb) && argb == 0xFFFF0000u,
          "bc1 texel %08X", argb);
    CHECK(px && d3d8_dxt_decode_texel(px, 0x0C, 0, 1, 8, &argb) && argb == 0xFF0000FFu,
          "bc1 texel %08X", argb);

    /* The kind is one switch of its own, a generation each way. */
    g = nv2a_tex_pack_gen();
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 0);
    CHECK(nv2a_tex_pack_find(HASH_C, 4, 2) == NULL, "world off: found");
    CHECK(nv2a_tex_pack_gen() == g + 1, "world off: gen");
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 1);
    CHECK(nv2a_tex_pack_find(HASH_C, 4, 2) != NULL, "world on: missing");
    CHECK(nv2a_tex_pack_gen() == g + 2, "world on: gen");

    /* Release: the payload goes, and comes back the same. */
    e = nv2a_tex_pack_find(HASH_C, 4, 2);
    CHECK(e && nv2a_tex_pack_resident_bytes() == 16, "held %zu", nv2a_tex_pack_resident_bytes());
    nv2a_tex_pack_release(e);
    CHECK(e && !e->data && nv2a_tex_pack_resident_bytes() == 0, "released");
    px = e ? (const uint8_t *)nv2a_tex_pack_texels(e) : NULL;
    CHECK(px && memcmp(px, BC1_B, 8) == 0, "read again");
    nv2a_tex_pack_set_keep(1);
    nv2a_tex_pack_release(e);
    CHECK(e && e->data && nv2a_tex_pack_resident_bytes() == 16, "tex_pack_keep held it");
    nv2a_tex_pack_set_keep(0);
    nv2a_tex_pack_release(e);
    nv2a_tex_pack_release(e);   /* twice, and of what is not held: nothing */
    nv2a_tex_pack_release(NULL);
    CHECK(nv2a_tex_pack_resident_bytes() == 0, "double release");
    nv2a_tex_pack_unload();

    /* Texels at kind 3, and each version's refusals. */
    n = build_v3(buf, XRTP_KIND_WORLD, 2, 2) + 8;   /* BC1's 8 bytes; 2x2 texels are 16 */
    write_file("v3_texels.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v3_texels.pack", err, sizeof err) == 1, "v3 texels: '%s'", err);
    e = nv2a_tex_pack_find(HASH_C, 2, 2);
    CHECK(e && e->bc == XRTP_BC_NONE && e->kind == XRTP_KIND_WORLD && e->bytes == 16,
          "texel entry at kind 3");
    nv2a_tex_pack_unload();
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1, 8, 4);
    wr32(buf + NV2A_TEX_PACK_HDR + 20, XRTP_KIND_WORLD | XRTP_FLAG_BC1 | 0x40u);
    refuse("v3_bit6.pack", buf, n, "flags");
    wr32(buf + NV2A_TEX_PACK_HDR + 20, XRTP_KIND_WORLD | XRTP_FLAG_BC1 | XRTP_FLAG_BC3);
    refuse("v3_both.pack", buf, n, "two block formats");
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1, 6, 4);
    refuse("v3_odd.pack", buf, n, "multiple of 4");
    /* A v2 file refuses what only v3 defines, whatever else it holds. */
    n = build_v3(buf, XRTP_KIND_WORLD, 8, 4);
    wr32(buf + 4, 2);
    wr32(buf + 28, 0);
    refuse("v2_world.pack", buf, n, "kind");
    n = build_v3(buf, XRTP_KIND_ART | XRTP_FLAG_BC1, 8, 4);
    wr32(buf + 4, 2);
    wr32(buf + 28, 0);
    refuse("v2_bc1.pack", buf, n, "flags");
    /* v1 and v2 still load under the v3 reader (test_write, test_v2 too). */
    n = build_pack(buf, HASH_A, HASH_B, 2);
    write_file("v2_still.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v2_still.pack", err, sizeof err) == 2
          && nv2a_tex_pack_version() == 2, "v2 file: '%s'", err);
    nv2a_tex_pack_unload();
}

/* v4: v3's build with the detile mark. */
static void test_v4(void)
{
    uint8_t buf[512];
    char err[128] = "";
    struct nv2a_tex_pack_entry *e;
    size_t n;

    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1 | XRTP_FLAG_DETILE_HEX, 8, 4);
    wr32(buf + 4, 4);
    write_file("v4.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v4.pack", err, sizeof err) == 1, "v4 load: '%s'", err);
    CHECK(nv2a_tex_pack_version() == 4, "v4 version %u", nv2a_tex_pack_version());
    CHECK(nv2a_tex_pack_detile_count() == 1 && nv2a_tex_pack_bc1_count() == 1
          && nv2a_tex_pack_kind_count(XRTP_KIND_WORLD) == 1, "v4 counts");
    e = nv2a_tex_pack_find(HASH_C, 4, 2);
    CHECK(e && (e->flags & XRTP_FLAG_DETILE_HEX) && e->kind == XRTP_KIND_WORLD
          && e->bc == XRTP_BC_1, "v4 entry");
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_detile_count() == 0, "detile count after unload");
    /* Unmarked v4 and v3 files: no mark. */
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1, 8, 4);
    write_file("v3_plain.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v3_plain.pack", err, sizeof err) == 1
          && nv2a_tex_pack_detile_count() == 0, "v3 under v4: '%s'", err);
    nv2a_tex_pack_unload();
    /* The mark on art (a HUD piece, text) is refused, and on v3 it is an
     * unknown bit. */
    n = build_v3(buf, XRTP_KIND_ART | XRTP_FLAG_DETILE_HEX, 8, 4);
    wr32(buf + 4, 4);
    refuse("v4_art.pack", buf, n, "non-world");
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_DETILE_HEX | 0x80u, 8, 4);
    wr32(buf + 4, 4);
    refuse("v4_bit7.pack", buf, n, "unknown flags");
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR, 8, 4);
    wr32(buf + 4, 4);
    refuse("v4_recolour.pack", buf, n, "unknown flags");
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_DETILE_HEX, 8, 4);
    wr32(buf + 4, 7);
    refuse("v7.pack", buf, n, "version");
}

/* texture-pack.md's vector: bytes i0, i1, w, 0 under FF000000, FFFFFFFF,
 * 80FF0000. */
static const uint32_t RC_PAL[3] = { 0xFF000000u, 0xFFFFFFFFu, 0x80FF0000u };
static const uint32_t RC_IDX[6] = { 0x00000100u, 0x00FF0100u, 0x00800100u,
                                    0x00400202u, 0x00000300u, 0x00000003u };
static const uint32_t RC_OUT[6] = { 0xFF000000u, 0xFFFFFFFFu, 0xFF808080u,
                                    0x80FF0000u, 0xFF000000u, 0xFFFF00FFu };

static void test_v5(void)
{
    uint8_t buf[512];
    char err[128] = "";
    uint32_t out[TEXELS_B], pal2[3];
    const uint32_t *px;
    struct nv2a_tex_pack_entry *e;
    size_t n, pay_b, held;
    unsigned i;

    nv2a_tex_pack_recolour_texels(RC_IDX, 6, RC_PAL, 3, out);
    for (i = 0; i < 6; i++)
        CHECK(out[i] == RC_OUT[i], "vector %u: %08X, want %08X", i, out[i], RC_OUT[i]);
    nv2a_tex_pack_recolour_texels(RC_IDX, 1, NULL, 3, out);
    CHECK(out[0] == 0xFFFF00FFu, "no palette: %08X", out[0]);

    /* A: a 2x2 exact world entry; B: a 4x2 two-level recolour entry. */
    n = build_pack(buf, HASH_A, HASH_B, 5);
    pay_b = n - TEXELS_B * 4;
    wr32(buf + NV2A_TEX_PACK_HDR + 20, XRTP_KIND_WORLD);
    wr32(buf + NV2A_TEX_PACK_HDR + 52, XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR);
    for (i = 0; i < TEXELS_B; i++)
        wr32(buf + pay_b + i * 4, i < 6 ? RC_IDX[i] : 0x00000001u);
    write_file("v5.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v5.pack", err, sizeof err) == 2, "v5 load: '%s'", err);
    CHECK(nv2a_tex_pack_version() == 5 && nv2a_tex_pack_recolour_count() == 1, "v5 counts");
    CHECK(nv2a_tex_pack_find(HASH_A, 2, 2) != NULL, "exact entry");
    CHECK(nv2a_tex_pack_find(HASH_B, 4, 2) == NULL, "find took a recolour entry");
    CHECK(nv2a_tex_pack_find_recolour(HASH_A, 1, 2, 2) == NULL, "find_recolour took an exact entry");
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 1, 4, 4) == NULL, "recolour of another aspect");
    e = nv2a_tex_pack_find_recolour(HASH_B, 1, 2, 1);
    CHECK(e && e->width == 4 && e->levels == 2 && e->bytes == TEXELS_B * 4u, "recolour entry");
    px = nv2a_tex_pack_recolour(e, RC_PAL, 3);
    CHECK(px != NULL, "recolour read");
    if (px) {
        for (i = 0; i < 6; i++)
            CHECK(px[i] == RC_OUT[i], "pack vector %u: %08X", i, px[i]);
        CHECK(px[6] == RC_PAL[1] && px[9] == RC_PAL[1], "second level");
    }
    held = nv2a_tex_pack_resident_bytes();
    nv2a_tex_pack_release(e);
    CHECK(e->data != NULL && nv2a_tex_pack_resident_bytes() == held, "recolour payload released");
    /* Another palette, the same entry. */
    pal2[0] = 0xFF102030u;
    pal2[1] = 0xFF405060u;
    pal2[2] = 0x00000000u;
    px = nv2a_tex_pack_recolour(e, pal2, 3);
    CHECK(px && px[0] == pal2[0] && px[1] == pal2[1] && px[3] == pal2[2]
          && px[5] == 0xFFFF00FFu, "second palette");
    CHECK(nv2a_tex_pack_recolour(nv2a_tex_pack_find(HASH_A, 2, 2), pal2, 3) == NULL,
          "recolour of an exact entry");
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 0);
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 1, 4, 2) == NULL, "world off");
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 1);
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_recolour_count() == 0, "recolour count after unload");
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 1, 4, 2) == NULL, "after unload");

    /* A block format would blend indices. */
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1 | XRTP_FLAG_RECOLOUR, 8, 4);
    wr32(buf + 4, 5);
    refuse("v5_bc1.pack", buf, n, "block format");
    /* Hex tiling samples the recoloured texels: both marks stand. */
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR | XRTP_FLAG_DETILE_HEX, 8, 4);
    wr32(buf + 4, 5);
    /* Index texels, not blocks: 4 bytes each. */
    memset(buf + n, 0, 8u * 4u * 4u - (n - NV2A_TEX_PACK_HDR - NV2A_TEX_PACK_ENTRY));
    n = NV2A_TEX_PACK_HDR + NV2A_TEX_PACK_ENTRY + 8u * 4u * 4u;
    write_file("v5_detile.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v5_detile.pack", err, sizeof err) == 1
          && nv2a_tex_pack_recolour_count() == 1 && nv2a_tex_pack_detile_count() == 1,
          "v5 recolour + detile: '%s'", err);
    e = nv2a_tex_pack_find_recolour(HASH_C, 1, 4, 2);
    CHECK(e && (e->flags & XRTP_FLAG_DETILE_HEX) && e->bc == XRTP_BC_NONE
          && e->bytes == 8u * 4u * 4u, "v5 recolour + detile entry");
    nv2a_tex_pack_unload();
    /* v4 and older files still read as before, with no recolour entry. */
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1 | XRTP_FLAG_DETILE_HEX, 8, 4);
    wr32(buf + 4, 4);
    write_file("v4_under_v5.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v4_under_v5.pack", err, sizeof err) == 1
          && nv2a_tex_pack_recolour_count() == 0, "v4 under v5: '%s'", err);
    nv2a_tex_pack_unload();
}

static void test_v6(void)
{
    uint8_t buf[512];
    char err[256];
    struct nv2a_tex_pack_entry *e;
    size_t n;

    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1 | XRTP_FLAG_MIP_BLEND, 8, 4);
    wr32(buf + 4, 6);
    write_file("v6_blend.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v6_blend.pack", err, sizeof err) == 1
          && nv2a_tex_pack_blend_count() == 1, "v6 blend: '%s'", err);
    e = nv2a_tex_pack_find(HASH_C, 4, 2);
    CHECK(e && (e->flags & XRTP_FLAG_MIP_BLEND) && e->bc == XRTP_BC_1, "v6 blend entry");
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_blend_count() == 0, "blend count after unload");

    /* A v5 reader knows no bit 8: an older pack carrying it is corrupt. */
    wr32(buf + 4, 5);
    refuse("v5_blend.pack", buf, n, "unknown flags");
    n = build_v3(buf, XRTP_KIND_WORLD | XRTP_FLAG_BC1, 8, 4);
    wr32(buf + 4, 5);
    write_file("v5_under_v6.pack", buf, n);
    CHECK(nv2a_tex_pack_load("v5_under_v6.pack", err, sizeof err) == 1
          && nv2a_tex_pack_blend_count() == 0, "v5 under v6: '%s'", err);
    nv2a_tex_pack_unload();
}

/* Every (c0, c1, w) on all four channels at once: pal[k] puts k, 255 - k,
 * k ^ 0x5A and k * 7 in the four bytes, so each lane sees another pair. */
static void test_blend_exhaustive(void)
{
    static uint32_t pal[256], idx[65536], out[65536];
    unsigned w, i0, i1, s, bad = 0;
    for (i0 = 0; i0 < 256; i0++)
        pal[i0] = i0 | ((255u - i0) << 8) | ((i0 ^ 0x5Au) << 16) | (((i0 * 7u) & 255u) << 24);
    for (w = 0; w < 256; w++) {
        for (i0 = 0; i0 < 256; i0++)
            for (i1 = 0; i1 < 256; i1++)
                idx[i0 * 256 + i1] = i0 | (i1 << 8) | (w << 16);
        nv2a_tex_pack_recolour_texels(idx, 65536, pal, 256, out);
        for (i0 = 0; i0 < 256; i0++)
            for (i1 = 0; i1 < 256; i1++)
                for (s = 0; s < 32; s += 8) {
                    uint32_t a = (pal[i0] >> s) & 255u, b = (pal[i1] >> s) & 255u;
                    uint32_t want = (a * (255u - w) + b * w + 127u) / 255u;
                    if (((out[i0 * 256 + i1] >> s) & 255u) != want && bad++ < 4)
                        CHECK(0, "blend %02X %02X w %u lane %u: %08X", a, b, w, s,
                              out[i0 * 256 + i1]);
                }
    }
    CHECK(bad == 0, "blend: %u texels off", bad);
}

/* v5 pack of HASH_B as a 4x2 recolour entry (two levels), for the volatile
 * cases. */
static void load_recolour_b(const char *path)
{
    uint8_t buf[512];
    char err[128] = "";
    size_t n = build_pack(buf, HASH_A, HASH_B, 5);
    wr32(buf + NV2A_TEX_PACK_HDR + 20, XRTP_KIND_WORLD);
    wr32(buf + NV2A_TEX_PACK_HDR + 52, XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR);
    write_file(path, buf, n);
    CHECK(nv2a_tex_pack_load(path, err, sizeof err) == 2, "%s: '%s'", path, err);
}

static void flips(unsigned n)
{
    while (n--)
        nv2a_tex_pack_flip();
}

static unsigned miss_why_of(uint64_t hash, uint32_t *kind)
{
    struct nv2a_tex_pack_miss m[NV2A_TEX_PACK_MISSES];
    unsigned n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES), i;
    for (i = 0; i < n; i++)
        if (m[i].hash == hash) {
            *kind = m[i].kind;
            return m[i].why;
        }
    return 99;
}

static void test_volatile(void)
{
    uint64_t p;
    uint32_t gen, kind = 0;

    load_recolour_b("volatile.pack");
    flips(3);   /* past the load's bump */
    /* A burst: BURST new palettes go up recoloured, the next does not, and
     * the generation is bumped so their copies go up again. */
    for (p = 1; p <= NV2A_TEX_PACK_RECOLOUR_BURST; p++) {
        flips(1);
        CHECK(nv2a_tex_pack_find_recolour(HASH_B, p, 4, 2) != NULL, "burst palette %llu",
              (unsigned long long)p);
    }
    gen = nv2a_tex_pack_gen();
    flips(1);
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 100, 4, 2) == NULL, "volatile still recoloured");
    CHECK(nv2a_tex_pack_volatile_count() == 1 && nv2a_tex_pack_gen() != gen,
          "volatile count %u", nv2a_tex_pack_volatile_count());
    /* Re-uploads of a palette it went up with: from the guest now. */
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 3, 4, 2) == NULL, "known palette while volatile");
    nv2a_tex_pack_miss(HASH_B ^ 3u * 31u, 2, 1, 0x0B, 1);
    CHECK(miss_why_of(HASH_B ^ 3u * 31u, &kind) == NV2A_TEX_PACK_MISS_VOLATILE
          && kind == XRTP_KIND_WORLD, "volatile miss tag");
    /* Still changing: the quiet time restarts. */
    flips(NV2A_TEX_PACK_RECOLOUR_QUIET - 1);
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 101, 4, 2) == NULL, "quiet too early");
    flips(NV2A_TEX_PACK_RECOLOUR_QUIET - 1);
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 102, 4, 2) == NULL, "quiet restarted");
    gen = nv2a_tex_pack_gen();
    flips(NV2A_TEX_PACK_RECOLOUR_QUIET - 1);
    CHECK(nv2a_tex_pack_volatile_count() == 1 && nv2a_tex_pack_gen() == gen, "back too early");
    /* The flip ends it, with no upload, and bumps the generation so the
     * guest copies go up recoloured again. */
    flips(1);
    CHECK(nv2a_tex_pack_volatile_count() == 0 && nv2a_tex_pack_gen() != gen,
          "quiet: volatile %u", nv2a_tex_pack_volatile_count());
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 3, 4, 2) != NULL, "quiet: recolours again");
    /* Not volatile at once again: it counts afresh. */
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 103, 4, 2) != NULL, "quiet: new palette");
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_volatile_count() == 0, "volatile count after unload");

    /* A player picking colours: one new palette every 3 flips, 40 of them. */
    load_recolour_b("volatile.pack");
    flips(3);
    for (p = 1; p <= 40; p++) {
        flips(3);
        CHECK(nv2a_tex_pack_find_recolour(HASH_B, p, 4, 2) != NULL, "slow palette %llu",
              (unsigned long long)p);
    }
    /* A bump re-uploads every cached copy at once, a palette change at each
     * one: not an animation. */
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_ART, 0);
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_ART, 1);
    flips(1);
    for (p = 1; p <= 40; p++)
        CHECK(nv2a_tex_pack_find_recolour(HASH_B, p, 4, 2) != NULL, "re-upload %llu",
              (unsigned long long)p);
    CHECK(nv2a_tex_pack_volatile_count() == 0, "slow palettes turned volatile");
    /* Switched off by its kind: the miss says so, not "absent". */
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 0);
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 7, 4, 2) == NULL, "world off");
    nv2a_tex_pack_miss(HASH_B ^ 7u * 31u, 2, 1, 0x0B, 1);
    CHECK(miss_why_of(HASH_B ^ 7u * 31u, &kind) == NV2A_TEX_PACK_MISS_KIND_OFF
          && kind == XRTP_KIND_WORLD, "kind off miss tag");
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 1);
    nv2a_tex_pack_set_enabled(0);
    CHECK(nv2a_tex_pack_find_recolour(HASH_B, 8, 4, 2) == NULL, "pack off");
    nv2a_tex_pack_miss(HASH_B ^ 8u * 31u, 2, 1, 0x0B, 1);
    CHECK(miss_why_of(HASH_B ^ 8u * 31u, &kind) == NV2A_TEX_PACK_MISS_PACK_OFF, "pack off tag");
    nv2a_tex_pack_set_enabled(1);
    /* A full hash no recolour lookup stood for stays "absent". */
    nv2a_tex_pack_miss(0x4444ull, 2, 1, 0x0B, 1);
    CHECK(miss_why_of(0x4444ull, &kind) == NV2A_TEX_PACK_MISS_ABSENT, "absent tag");
    nv2a_tex_pack_unload();
    remove("volatile.pack");
}

/* One upload a flip under a cycle of n palettes, for `count` flips; the
 * number of them recoloured. */
static unsigned cycle(unsigned n, unsigned count, unsigned *step)
{
    unsigned k, got = 0;
    for (k = 0; k < count; k++) {
        flips(1);
        got += nv2a_tex_pack_find_recolour(HASH_B, 1000u + (*step)++ % n, 4, 2) != NULL;
    }
    return got;
}

static void test_volatile_cycles(void)
{
    unsigned step = 0, got;
    uint32_t gen;

    /* An 8-step cycle: every palette is one it has seen, but each step is
     * an upload (one cache slot per texture), so it turns volatile within
     * the first window and stays so while it plays. */
    load_recolour_b("cycles.pack");
    flips(3);
    got = cycle(8, NV2A_TEX_PACK_RECOLOUR_BURST + 1u, &step);
    CHECK(got == NV2A_TEX_PACK_RECOLOUR_BURST && nv2a_tex_pack_volatile_count() == 1,
          "8-step cycle: %u recoloured, volatile %u", got, nv2a_tex_pack_volatile_count());
    got = cycle(8, 4u * NV2A_TEX_PACK_RECOLOUR_QUIET, &step);
    CHECK(got == 0 && nv2a_tex_pack_volatile_count() == 1, "8-step cycle past quiet: %u", got);
    nv2a_tex_pack_unload();

    /* A 16-step cycle that keeps playing past the quiet window. */
    load_recolour_b("cycles.pack");
    flips(3);
    step = 0;
    cycle(16, NV2A_TEX_PACK_RECOLOUR_BURST + 1u, &step);
    got = cycle(16, 4u * NV2A_TEX_PACK_RECOLOUR_QUIET, &step);
    CHECK(got == 0 && nv2a_tex_pack_volatile_count() == 1, "16-step cycle past quiet: %u", got);

    /* It stops: back after QUIET, then a recurring fade waits twice as long
     * each time (each return bumps the generation). */
    gen = nv2a_tex_pack_gen();
    flips(NV2A_TEX_PACK_RECOLOUR_QUIET);
    CHECK(nv2a_tex_pack_volatile_count() == 0 && nv2a_tex_pack_gen() != gen, "first return");
    flips(NV2A_TEX_PACK_RECOLOUR_WINDOW);
    cycle(16, NV2A_TEX_PACK_RECOLOUR_BURST + 1u, &step);
    CHECK(nv2a_tex_pack_volatile_count() == 1, "fade again");
    flips(NV2A_TEX_PACK_RECOLOUR_QUIET);
    CHECK(nv2a_tex_pack_volatile_count() == 1, "second return after QUIET");
    flips(NV2A_TEX_PACK_RECOLOUR_QUIET);
    CHECK(nv2a_tex_pack_volatile_count() == 0, "second return after 2x QUIET");
    /* The wait stops growing at QUIET_MAX. */
    for (got = 0; got < 8; got++) {
        flips(NV2A_TEX_PACK_RECOLOUR_WINDOW);
        cycle(16, NV2A_TEX_PACK_RECOLOUR_BURST + 1u, &step);
        flips(NV2A_TEX_PACK_RECOLOUR_QUIET_MAX);
    }
    CHECK(nv2a_tex_pack_volatile_count() == 0, "back after QUIET_MAX");

    /* The same plane under one palette per character, changed only when the
     * player picks (every 10 flips): the change count stays low. */
    nv2a_tex_pack_unload();
    load_recolour_b("cycles.pack");
    flips(3);
    for (got = 0; got < 60; got++) {
        flips(10);
        CHECK(nv2a_tex_pack_find_recolour(HASH_B, 2000u + got % 4u, 4, 2) != NULL,
              "editor pick %u", got);
    }
    CHECK(nv2a_tex_pack_volatile_count() == 0, "editor picks turned volatile");
    nv2a_tex_pack_unload();
    remove("cycles.pack");
}

/* The recolour buffer: in the resident bytes, freed past KEEP. */
static void test_recolour_buffer(void)
{
    const uint32_t w = 2048, h = 1040;   /* 8.1 MiB of texels: past KEEP */
    size_t pay = NV2A_TEX_PACK_HDR + NV2A_TEX_PACK_ENTRY, n = pay + (size_t)w * h * 4u;
    uint8_t *buf = (uint8_t *)calloc(1, n);
    uint32_t pal[1] = { 0xFF123456u };
    char err[128] = "";
    struct nv2a_tex_pack_entry *e;
    const uint32_t *px;
    size_t held;

    CHECK((size_t)w * h * 4u > NV2A_TEX_PACK_RECOLOUR_KEEP, "case not past KEEP");
    if (!buf)
        return;
    memcpy(buf, NV2A_TEX_PACK_MAGIC, 4);
    wr32(buf + 4, 5);
    wr32(buf + 8, 1);
    wr32(buf + 12, 1);
    wr64(buf + 16, NV2A_TEX_PACK_HDR);
    wr32(buf + 28, 1u << XRTP_KIND_WORLD);
    wr64(buf + NV2A_TEX_PACK_HDR, HASH_C);
    wr32(buf + NV2A_TEX_PACK_HDR + 8, w);
    wr32(buf + NV2A_TEX_PACK_HDR + 12, h);
    wr32(buf + NV2A_TEX_PACK_HDR + 16, 1);
    wr32(buf + NV2A_TEX_PACK_HDR + 20, XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR);
    wr64(buf + NV2A_TEX_PACK_HDR + 24, pay);
    write_file("rc_big.pack", buf, n);
    free(buf);
    CHECK(nv2a_tex_pack_load("rc_big.pack", err, sizeof err) == 1, "rc_big: '%s'", err);
    e = nv2a_tex_pack_find_recolour(HASH_C, 1, w, h);
    px = e ? nv2a_tex_pack_recolour(e, pal, 1) : NULL;
    CHECK(px && px[0] == pal[0] && px[(size_t)w * h - 1] == pal[0], "big recolour");
    held = nv2a_tex_pack_resident_bytes();
    CHECK(held == 2u * (size_t)w * h * 4u, "payload + buffer held: %zu", held);
    nv2a_tex_pack_release(e);
    CHECK(nv2a_tex_pack_resident_bytes() == (size_t)w * h * 4u,
          "buffer kept past KEEP: %zu", nv2a_tex_pack_resident_bytes());
    px = nv2a_tex_pack_recolour(e, pal, 1);
    CHECK(px && px[17] == pal[0], "recolour after the buffer was freed");
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_resident_bytes() == 0, "resident after unload");
    remove("rc_big.pack");
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
        if (m[i].hash == 0xBB)
            CHECK(!m[i].candidate, "%llX taken for a candidate", (unsigned long long)m[i].hash);
        else if (m[i].hash == 0xDD)
            CHECK(m[i].candidate, "a mipped upload is a candidate");
        else
            CHECK(m[i].candidate && m[i].width == 32, "the 32x32 DXT5 miss");
    }
    CHECK(nv2a_tex_pack_misses(m, 2) == 2 && m[0].hash == 0xAA, "a short read");
    nv2a_tex_pack_print_misses();
    /* A P8 upload is a candidate now: its hash folds the palette, so a pack
     * built from its dump matches it. */
    nv2a_tex_pack_miss(0xEEull, 16, 16, 0x0B, 1);
    n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES);
    for (i = 0; i < n; i++)
        if (m[i].hash == 0xEE)
            CHECK(m[i].candidate, "a P8 upload is a candidate");

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

static void test_resident(void)
{
    uint8_t buf[512];
    char err[128] = "";
    size_t n = build_pack(buf, HASH_A, HASH_B, 1);
    struct nv2a_tex_pack_entry *a, *b;
    const uint32_t *px;

    write_file("resident.pack", buf, n);
    CHECK(nv2a_tex_pack_load("resident.pack", err, sizeof err) == 2, "load: %s", err);
    a = nv2a_tex_pack_find(HASH_A, 1, 1);
    b = nv2a_tex_pack_find(HASH_B, 2, 1);
    if (!a || !b) {
        fails++;
        printf("FAIL resident: entries not found\n");
        nv2a_tex_pack_unload();
        return;
    }

    /* The default budget holds both: nothing is freed. */
    CHECK(nv2a_tex_pack_texels(a) && nv2a_tex_pack_texels(b), "default: read");
    CHECK(a->data && b->data, "default budget freed an entry");
    CHECK(nv2a_tex_pack_resident_bytes() == (TEXELS_A + TEXELS_B) * 4,
          "default: %zu bytes held", nv2a_tex_pack_resident_bytes());
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_resident_bytes() == 0, "unload kept %zu bytes",
          nv2a_tex_pack_resident_bytes());

    /* Room for B (the larger) alone: each read frees the other. */
    nv2a_tex_pack_set_resident_max(TEXELS_B * 4);
    CHECK(nv2a_tex_pack_load("resident.pack", err, sizeof err) == 2, "reload: %s", err);
    a = nv2a_tex_pack_find(HASH_A, 1, 1);
    b = nv2a_tex_pack_find(HASH_B, 2, 1);
    CHECK(nv2a_tex_pack_texels(a) != NULL, "A read");
    px = nv2a_tex_pack_texels(b);
    CHECK(px && px[9] == 0x80000900u, "B read past A");
    CHECK(!a->data && b->data, "B's read did not free A");
    CHECK(nv2a_tex_pack_resident_bytes() == TEXELS_B * 4, "B alone: %zu bytes",
          nv2a_tex_pack_resident_bytes());
    px = nv2a_tex_pack_texels(a);
    CHECK(px && px[0] == 0xFF000000u && px[3] == 0xFF030303u, "A read again");
    CHECK(a->data && !b->data, "A's read did not free B");

    nv2a_tex_pack_unload();

    /* Three 2x2 entries, room for two: a use (not only a read) makes an
     * entry recent, and the least recently used is the one freed. */
    {
        uint8_t p3[NV2A_TEX_PACK_HDR + 3 * NV2A_TEX_PACK_ENTRY + 3 * 16];
        struct nv2a_tex_pack_entry *c, *d, *x;
        size_t pay = NV2A_TEX_PACK_HDR + 3 * NV2A_TEX_PACK_ENTRY;
        uint32_t i, t;
        memset(p3, 0, sizeof p3);
        memcpy(p3, NV2A_TEX_PACK_MAGIC, 4);
        wr32(p3 + 4, 1);
        wr32(p3 + 8, 3);
        wr32(p3 + 12, 2);
        wr64(p3 + 16, NV2A_TEX_PACK_HDR);
        for (i = 0; i < 3; i++) {
            uint8_t *q = p3 + NV2A_TEX_PACK_HDR + i * NV2A_TEX_PACK_ENTRY;
            wr64(q, 0x100ull + i);
            wr32(q + 8, 2);
            wr32(q + 12, 2);
            wr32(q + 16, 1);
            wr64(q + 24, pay + i * 16);
            for (t = 0; t < 4; t++)
                wr32(p3 + pay + i * 16 + t * 4, 0xFF000000u | (i << 8) | t);
        }
        write_file("resident3.pack", p3, sizeof p3);
        nv2a_tex_pack_set_resident_max(2 * 16);
        CHECK(nv2a_tex_pack_load("resident3.pack", err, sizeof err) == 3, "load 3: %s", err);
        c = nv2a_tex_pack_find(0x100, 1, 1);
        d = nv2a_tex_pack_find(0x101, 1, 1);
        x = nv2a_tex_pack_find(0x102, 1, 1);
        if (c && d && x) {
            CHECK(nv2a_tex_pack_texels(c) && nv2a_tex_pack_texels(d), "C and D read");
            CHECK(nv2a_tex_pack_texels(c) == c->data, "C used");   /* D is the oldest */
            px = nv2a_tex_pack_texels(x);
            CHECK(px && px[1] == 0xFF000201u, "E read");
            CHECK(c->data && !d->data && x->data, "E's read did not free D alone");
            px = nv2a_tex_pack_texels(d);                         /* C is the oldest */
            CHECK(px && px[3] == 0xFF000103u, "D read again");
            CHECK(!c->data && d->data && x->data, "D's read did not free C alone");
            CHECK(nv2a_tex_pack_resident_bytes() == 2 * 16, "three: %zu bytes",
                  nv2a_tex_pack_resident_bytes());
        } else {
            fails++;
            printf("FAIL resident: three entries not found\n");
        }
        nv2a_tex_pack_unload();
    }

    /* An entry over the budget is read and held alone. */
    nv2a_tex_pack_set_resident_max(TEXELS_A * 4);
    CHECK(nv2a_tex_pack_load("resident.pack", err, sizeof err) == 2, "reload: %s", err);
    a = nv2a_tex_pack_find(HASH_A, 1, 1);
    b = nv2a_tex_pack_find(HASH_B, 2, 1);
    CHECK(nv2a_tex_pack_texels(a) != NULL, "A at the budget");
    px = nv2a_tex_pack_texels(b);
    CHECK(px && px[0] == 0x80000000u && !a->data, "B over the budget: read, A freed");
    CHECK(nv2a_tex_pack_resident_bytes() == TEXELS_B * 4, "over: %zu bytes",
          nv2a_tex_pack_resident_bytes());
    nv2a_tex_pack_unload();

    /* Room for B alone, B held: reads of A that must fail keep B. */
    {
        struct stat st;
        struct utimbuf ut;
        nv2a_tex_pack_set_resident_max(TEXELS_B * 4);
        write_file("resident.pack", buf, n);
        CHECK(nv2a_tex_pack_load("resident.pack", err, sizeof err) == 2, "reload: %s", err);
        a = nv2a_tex_pack_find(HASH_A, 1, 1);
        b = nv2a_tex_pack_find(HASH_B, 2, 1);
        CHECK(nv2a_tex_pack_texels(b) != NULL, "B read");
        remove("resident.pack");
        CHECK(nv2a_tex_pack_texels(a) == NULL, "A read from a missing file");
        CHECK(b->data && nv2a_tex_pack_resident_bytes() == TEXELS_B * 4,
              "a failed open freed B");
        /* The same size, rebuilt in place: only the mtime tells. */
        write_file("resident.pack", buf, n);
        CHECK(stat("resident.pack", &st) == 0, "stat");
        ut.actime = st.st_atime;
        ut.modtime = st.st_mtime + 10;
        CHECK(utime("resident.pack", &ut) == 0, "utime");
        CHECK(nv2a_tex_pack_texels(a) == NULL, "A read from a rewritten file");
        CHECK(b->data && !a->data, "a rewritten file freed B");
        CHECK(nv2a_tex_pack_texels(b) == b->data, "B, held, no longer given");
        ut.modtime = st.st_mtime;
        utime("resident.pack", &ut);
        CHECK(nv2a_tex_pack_texels(a) == NULL, "a changed file read again");
        nv2a_tex_pack_unload();

        /* Another size: refused too. A fresh load takes the new file. */
        CHECK(nv2a_tex_pack_load("resident.pack", err, sizeof err) == 2, "reload: %s", err);
        a = nv2a_tex_pack_find(HASH_A, 1, 1);
        write_file("resident.pack", buf, n + 4);
        CHECK(nv2a_tex_pack_texels(a) == NULL, "A read from a longer file");
        nv2a_tex_pack_unload();
        CHECK(nv2a_tex_pack_load("resident.pack", err, sizeof err) == 2, "reload: %s", err);
        a = nv2a_tex_pack_find(HASH_A, 1, 1);
        CHECK(nv2a_tex_pack_texels(a) != NULL, "a new load did not reset the check");
        nv2a_tex_pack_unload();
    }
    nv2a_tex_pack_set_resident_max(NV2A_TEX_PACK_RESIDENT_MAX);
}

int main(void)
{
    test_write();
    test_refuse();
    test_hash();
    test_v2();
    test_bc3();
    test_v3();
    test_v4();
    test_v5();
    test_v6();
    test_blend_exhaustive();
    test_volatile();
    test_volatile_cycles();
    test_recolour_buffer();
    test_misses();
    test_resident();
    if (fails) {
        printf("tex_pack: %d failure(s)\n", fails);
        return 1;
    }
    printf("tex_pack: ok\n");
    return 0;
}
