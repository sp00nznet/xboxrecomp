/*
 * tex_dump: the runtime texture dump (nv2a_tex_dump.h).
 *
 *   write     a swizzled 4x4 with two levels: <hash>.xtd has the header
 *             (magic, version, hash, size, levels, format, flags, first
 *             flip, data offset, texel count) and the texels as given;
 *             <hash>.json names the format, layout, wrap and filters
 *   repeat    the same hash again writes nothing and counts a repeat;
 *             wants() says 0 for it
 *   linear    a linear texture writes its sidecar ("texels": false), no
 *             .xtd, and is counted once
 *   volatile  one address taking 8 distinct hashes writes them all; the
 *             9th marks it volatile and writes nothing, nor do later ones
 *             (even a hash seen before at it); volatile.json names it
 *   rt        render-target binds count per bind and per distinct target
 *   oversize  a texture over the pack's size cap is counted, not written
 *   retry     a sidecar that cannot be written (its name is a directory)
 *             removes the .xtd and leaves the hash to the next upload,
 *             which writes both
 *   cap       past the byte cap the dump stops: nothing more is written
 *             or wanted, the uploads after it are counted as stopped
 *   off       set_dir(NULL) turns it off and resets the counts
 *   mag       the trace's fold: a slot's biased -lod is 2^-lod pixels per
 *             entry texel, by the entry's scale per source texel; the max
 *             across frames; empty and overflowed slots ignored; a draw
 *             past the frame's slots counted, not folded; the sidecar takes
 *             the figures and has them rewritten, once
 *   triangle  the pre-transformed figure: pixels per texel from the vertices
 *   p8        a P8 stage's hash folds its palette; the file the dump writes
 *             under it and a v3 pack built with that hash match the upload,
 *             and the same indices under another palette miss
 */
#include "nv2a_tex_dump.h"
#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define getpid _getpid
#define mkdir(p, m) _mkdir(p)
#define rmdir _rmdir
#include <process.h>
#else
#include <unistd.h>
#endif

static int s_fail;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

static char s_dir[512];

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static long file_size(const char *name)
{
    char path[640];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static char *slurp(const char *name, long *len)
{
    char path[640];
    FILE *f;
    char *b;
    long n = file_size(name);
    if (n < 0)
        return NULL;
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    if (!(f = fopen(path, "rb")))
        return NULL;
    b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    b[n] = 0;
    fclose(f);
    if (len)
        *len = n;
    return b;
}

static struct nv2a_stage stage(uint32_t offset, uint32_t color, uint32_t w, uint32_t h,
                               uint32_t levels)
{
    struct nv2a_stage t;
    memset(&t, 0, sizeof t);
    t.offset = offset;
    t.color = color;
    t.width = w;
    t.height = h;
    t.levels = levels;
    t.addr_u = 1;           /* wrap */
    t.addr_v = 3;           /* clamp to edge */
    t.filter = 0x02020000u; /* min linear, mag linear */
    t.valid = 1;
    return t;
}

static void name_of(char *out, size_t len, uint64_t hash, const char *ext)
{
    snprintf(out, len, "%016llX.%s", (unsigned long long)hash, ext);
}

static void test_write(void)
{
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage t = stage(0x10000, 0x06, 4, 4, 2);   /* A8R8G8B8 swizzled */
    uint32_t texels[20];
    char name[64], *b;
    long n;
    unsigned i;

    for (i = 0; i < 20; i++)
        texels[i] = 0xFF000000u | i * 0x010203u;
    nv2a_tex_dump_flip(7);
    CHECK(nv2a_tex_dump_wants(&t, 0x1122334455667788ull));
    nv2a_tex_dump_upload(&t, 0x1122334455667788ull, texels);
    name_of(name, sizeof name, 0x1122334455667788ull, "xtd");
    b = slurp(name, &n);
    CHECK(b != NULL);
    if (b) {
        const uint8_t *p = (const uint8_t *)b;
        CHECK(n == 64 + 20 * 4);
        CHECK(memcmp(p, "XRTD", 4) == 0);
        CHECK(rd32(p + 4) == 1);
        CHECK(rd32(p + 8) == 0x55667788u && rd32(p + 12) == 0x11223344u);
        CHECK(rd32(p + 16) == 4 && rd32(p + 20) == 4 && rd32(p + 24) == 2);
        CHECK(rd32(p + 28) == 0x06);
        CHECK(rd32(p + 36) == XTD_FLAG_SWIZZLED);
        CHECK(rd32(p + 40) == 7);
        CHECK(rd32(p + 44) == 64);
        CHECK(rd32(p + 48) == 20 && rd32(p + 52) == 0);
        CHECK(n < 64 + 80 || memcmp(p + 64, texels, 80) == 0);
        free(b);
    }
    name_of(name, sizeof name, 0x1122334455667788ull, "json");
    b = slurp(name, NULL);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"hash\": \"1122334455667788\"") != NULL);
        CHECK(strstr(b, "\"format\": \"A8R8G8B8\"") != NULL);
        CHECK(strstr(b, "\"layout\": \"swizzled\"") != NULL);
        CHECK(strstr(b, "\"wrap_u\": \"wrap\"") != NULL);
        CHECK(strstr(b, "\"wrap_v\": \"clamp_edge\"") != NULL);
        CHECK(strstr(b, "\"min_filter\": \"linear\"") != NULL);
        CHECK(strstr(b, "\"mag_filter\": \"linear\"") != NULL);
        CHECK(strstr(b, "\"first_flip\": 7") != NULL);
        CHECK(strstr(b, "\"texels\": true") != NULL);
        free(b);
    }
    /* repeat */
    CHECK(!nv2a_tex_dump_wants(&t, 0x1122334455667788ull));
    nv2a_tex_dump_upload(&t, 0x1122334455667788ull, NULL);
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.uploads == 2 && st.written == 1 && st.repeats == 1 && st.texels == 20);
    CHECK(st.errors == 0 && st.bytes == 64 + 20 * 4);
}

static void test_linear(void)
{
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage t = stage(0x20000, 0x12, 640, 480, 1);   /* LIN_A8R8G8B8 */
    char name[64], *b;

    t.pitch = 640 * 4;
    CHECK(!nv2a_tex_dump_wants(&t, 0xAAull));
    nv2a_tex_dump_upload(&t, 0xAAull, NULL);
    nv2a_tex_dump_upload(&t, 0xAAull, NULL);
    name_of(name, sizeof name, 0xAAull, "xtd");
    CHECK(file_size(name) < 0);
    name_of(name, sizeof name, 0xAAull, "json");
    b = slurp(name, NULL);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"layout\": \"linear\"") != NULL);
        CHECK(strstr(b, "\"pitch\": 2560") != NULL);
        CHECK(strstr(b, "\"texels\": false") != NULL);
        free(b);
    }
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.linear == 1 && st.written == 1 && st.repeats == 2);   /* with test_write's */
}

static void test_volatile(void)
{
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage t = stage(0x30000, 0x06, 2, 2, 1);
    uint32_t texels[4] = {1, 2, 3, 4};
    char name[64], *b;
    uint64_t h;

    for (h = 1; h <= NV2A_TEX_DUMP_VOLATILE; h++) {
        CHECK(nv2a_tex_dump_wants(&t, 0x5000 + h));
        nv2a_tex_dump_upload(&t, 0x5000 + h, texels);
    }
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.written == 1 + NV2A_TEX_DUMP_VOLATILE && st.volatile_addrs == 0);
    /* A hash already among the eight is a repeat, not a ninth. */
    nv2a_tex_dump_upload(&t, 0x5001, NULL);
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.repeats == 3 && st.volatile_addrs == 0);
    CHECK(!nv2a_tex_dump_wants(&t, 0x5000 + NV2A_TEX_DUMP_VOLATILE + 1));
    nv2a_tex_dump_upload(&t, 0x5000 + NV2A_TEX_DUMP_VOLATILE + 1, NULL);
    name_of(name, sizeof name, 0x5000 + NV2A_TEX_DUMP_VOLATILE + 1, "xtd");
    CHECK(file_size(name) < 0);
    /* Given up on: nothing more of it, a known hash included. */
    nv2a_tex_dump_upload(&t, 0x5001, NULL);
    nv2a_tex_dump_upload(&t, 0x7777, NULL);
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.volatile_addrs == 1 && st.volatile_skips == 3);
    CHECK(st.written == 1 + NV2A_TEX_DUMP_VOLATILE);
    /* Another size at the same offset is another address. */
    t.width = 4;
    CHECK(nv2a_tex_dump_wants(&t, 0x9999));
    nv2a_tex_dump_summary();
    b = slurp("volatile.json", NULL);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"offset\": \"0x00030000\", \"width\": 2") != NULL);
        CHECK(strstr(b, "\"0000000000005008\"") != NULL);
        free(b);
    }
    b = slurp("counts.json", NULL);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"0000000000005001\": 2") != NULL);
        free(b);
    }
}

static void test_rt_oversize(void)
{
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage a = stage(0x40000, 0x12, 640, 480, 1);
    struct nv2a_stage b = stage(0x50000, 0x12, 320, 240, 1);
    struct nv2a_stage big = stage(0x60000, 0x06, 32768, 2, 1);

    nv2a_tex_dump_rt(&a);
    nv2a_tex_dump_rt(&a);
    nv2a_tex_dump_rt(&b);
    CHECK(!nv2a_tex_dump_wants(&big, 0xB16));
    nv2a_tex_dump_upload(&big, 0xB16, NULL);
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.rt_binds == 3 && st.rt_targets == 2);
    CHECK(st.oversize == 1);
}

static void test_retry(void)
{
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage t = stage(0x70000, 0x06, 4, 4, 1);
    uint32_t texels[16] = {0};
    const uint64_t hash = 0x0123456789ABCDEFull;
    char name[64], path[640];

    nv2a_tex_dump_get_stats(&st);
    name_of(name, sizeof name, hash, "json");
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    CHECK(mkdir(path, 0755) == 0);
    nv2a_tex_dump_upload(&t, hash, texels);
    {
        struct nv2a_tex_dump_stats now;
        nv2a_tex_dump_get_stats(&now);
        CHECK(now.errors == st.errors + 1 && now.written == st.written);
    }
    name_of(name, sizeof name, hash, "xtd");
    CHECK(file_size(name) < 0);
    CHECK(nv2a_tex_dump_wants(&t, hash));
    CHECK(rmdir(path) == 0);
    nv2a_tex_dump_upload(&t, hash, texels);
    CHECK(file_size(name) == 64 + 16 * 4);
    nv2a_tex_dump_get_stats(&st);
    CHECK(!nv2a_tex_dump_wants(&t, hash));
    nv2a_tex_dump_upload(&t, hash, NULL);
    {
        struct nv2a_tex_dump_stats now;
        nv2a_tex_dump_get_stats(&now);
        CHECK(now.written == st.written && now.repeats == st.repeats + 1);
    }
}

static void test_cap(void)
{
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage a = stage(0x80000, 0x06, 4, 4, 1), b = stage(0x90000, 0x06, 4, 4, 1);
    uint32_t texels[16] = {0};
    char name[64];

    snprintf(s_dir + strlen(s_dir), sizeof s_dir - strlen(s_dir), "/cap");
    nv2a_tex_dump_set_max_bytes(64 + 16 * 4 + 10);
    CHECK(nv2a_tex_dump_set_dir(s_dir) == 0);
    nv2a_tex_dump_upload(&a, 0xA1ull, texels);
    nv2a_tex_dump_upload(&b, 0xB2ull, texels);
    CHECK(!nv2a_tex_dump_wants(&b, 0xC3ull));
    nv2a_tex_dump_upload(&b, 0xC3ull, texels);
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.written == 1 && st.bytes == 64 + 16 * 4);
    CHECK(st.stopped == 2 && st.uploads == 3);
    name_of(name, sizeof name, 0xB2ull, "xtd");
    CHECK(file_size(name) < 0);
    nv2a_tex_dump_set_max_bytes(0);
}

static void test_off(void)
{
    struct nv2a_tex_dump_stats st;
    nv2a_tex_dump_set_dir(NULL);
    CHECK(!nv2a_tex_dump_on());
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.uploads == 0 && st.written == 0 && st.rt_binds == 0);
}

#define FEQ(a, b) (fabs((double)(a) - (double)(b)) < 1e-3)

static uint32_t mag_slot_for(int lod)
{
    return (uint32_t)((32 - lod) * 1024);
}

/* A sidecar of `size` bytes: valid JSON, padded in one string. */
static void write_sidecar(uint64_t hash, size_t size)
{
    static const char head[] = "{\n  \"pad\": \"", tail[] = "\"\n}\n";
    char name[64], path[640];
    FILE *f;
    size_t i;
    name_of(name, sizeof name, hash, "json");
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs(head, f);
    for (i = sizeof head + sizeof tail - 2u; i < size; i++)
        fputc('x', f);
    fputs(tail, f);
    fclose(f);
}

/* The rewrite reads into a fixed buffer: the largest file that fits gets the
 * figures, and one byte more leaves the file as it was, not cut short. */
static void test_mag_sidecar_size(void)
{
    const uint64_t FITS = 0xD4D4ull, OVER = 0xE5E5ull;
    struct nv2a_tex_dump_stats st, now;
    char name[64], *b;
    long len;

    write_sidecar(FITS, 4095);
    write_sidecar(OVER, 4096);
    nv2a_tex_dump_get_stats(&st);
    nv2a_tex_mag_note(FITS, 4.0, 1);
    nv2a_tex_mag_note(OVER, 4.0, 1);
    nv2a_tex_dump_summary();
    nv2a_tex_dump_get_stats(&now);
    CHECK(now.errors == st.errors + 1);

    name_of(name, sizeof name, FITS, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"pad\"") != NULL);
        CHECK(strstr(b, "\"mag_max\": 4.000,\n") != NULL);
        CHECK(strstr(b, "\"render_scale\": 1\n}\n") != NULL);
        free(b);
    }
    name_of(name, sizeof name, OVER, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(len == 4096);
        CHECK(strstr(b, "\"mag_max\"") == NULL);
        CHECK(strcmp(b + len - 4, "\"\n}\n") == 0);
        free(b);
    }
}

static void test_mag(void)
{
    static uint32_t slots[NV2A_TEX_MAG_DRAWS * NV2A_TEX_MAG_STAGES];
    static struct nv2a_tex_mag_draw draws[NV2A_TEX_MAG_DRAWS];
    const uint64_t HA = 0xA1A1ull, HB = 0xB2B2ull, HC = 0xC3C3ull;
    uint64_t hash[4] = { HA, HB, 0, HC }, n_draws = 0;
    uint32_t scale[4] = { 1, 2, 1, 1 };
    struct nv2a_stage t = stage(0x40000, 0x06, 4, 4, 1);
    uint32_t texels[16] = { 0 };
    char name[64], *b;
    double mag = 0;
    unsigned n = 0, rs = 0, id, i;
    long len;

    nv2a_tex_mag_set_on(1);
    CHECK(nv2a_tex_mag_on());
    id = nv2a_tex_mag_draw_id(draws, &n, hash, scale);
    CHECK(id == 0 && n == 1);
    slots[0] = mag_slot_for(-2);          /* stage 0, 4 pixels per texel */
    slots[1] = mag_slot_for(-2);          /* stage 1, the same at entry scale 2 */
    slots[2] = mag_slot_for(-5);          /* stage 2 has no hash: nothing */
    slots[3] = 0;                         /* stage 3 unread: nothing */
    nv2a_tex_mag_fold(draws, n, slots, 2);
    CHECK(nv2a_tex_mag_get(HA, &mag, &n_draws, &rs) && FEQ(mag, 4.0) && n_draws == 1 && rs == 2);
    CHECK(nv2a_tex_mag_get(HB, &mag, NULL, NULL) && FEQ(mag, 8.0));
    CHECK(!nv2a_tex_mag_get(HC, NULL, NULL, NULL));
    CHECK(!nv2a_tex_mag_get(0, NULL, NULL, NULL));

    /* The max stays over a later, smaller frame; out-of-range slots read as
     * nothing (the shader never writes them; a bad buffer must not count). */
    slots[0] = mag_slot_for(1);
    slots[1] = 64u * 1024u;
    slots[3] = 0xFFFFFFFFu;
    nv2a_tex_mag_fold(draws, n, slots, 2);
    CHECK(nv2a_tex_mag_get(HA, &mag, &n_draws, NULL) && FEQ(mag, 4.0) && n_draws == 2);
    CHECK(nv2a_tex_mag_get(HB, &mag, &n_draws, NULL) && FEQ(mag, 8.0) && n_draws == 1);
    CHECK(!nv2a_tex_mag_get(HC, NULL, NULL, NULL));

    /* A frame past its slots: the last id is shared and never folded. */
    n = NV2A_TEX_MAG_DRAWS - 1u;
    CHECK(nv2a_tex_mag_draw_id(draws, &n, hash, scale) == NV2A_TEX_MAG_DRAWS - 1u);
    CHECK(n == NV2A_TEX_MAG_DRAWS - 1u);

    /* The sidecar gets the figures after its own, valid JSON, and the next
     * summary replaces them instead of adding more. */
    nv2a_tex_dump_upload(&t, HA, texels);
    nv2a_tex_dump_summary();
    name_of(name, sizeof name, HA, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"mag_max\": 4.000,\n") != NULL);
        CHECK(strstr(b, "\"mag_scale1\": 2.000,\n") != NULL);
        CHECK(strstr(b, "\"mag_draws\": 2,\n") != NULL);
        CHECK(strstr(b, "\"render_scale\": 2\n}\n") != NULL);
        CHECK(strstr(b, "\"texels\"") != NULL);
        free(b);
    }
    for (i = 0; i < NV2A_TEX_MAG_STAGES; i++)
        slots[i] = 0;
    slots[0] = mag_slot_for(-3);
    n = 1;
    nv2a_tex_mag_fold(draws, n, slots, 2);
    nv2a_tex_dump_summary();
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        const char *first = strstr(b, "\"mag_max\"");
        CHECK(first && strstr(first, "\"mag_max\": 8.000,\n") == first);
        CHECK(first && strstr(first + 1, "\"mag_max\"") == NULL);
        CHECK(strstr(b, "\"mag_draws\": 3,\n") != NULL);
        free(b);
    }
    test_mag_sidecar_size();
    nv2a_tex_mag_set_on(0);
    CHECK(!nv2a_tex_mag_on());
}

static void test_mag_triangle(void)
{
    /* 40 pixels across 10 texels, 80 down 10: the larger gradient is the
     * x axis' 0.25 texels per pixel, so 4 pixels per texel, as the GPU's LOD. */
    const float xy[3][2] = { { 0, 0 }, { 40, 0 }, { 0, 80 } };
    const float uv[3][2] = { { 0, 0 }, { 10, 0 }, { 0, 10 } };
    const float line[3][2] = { { 0, 0 }, { 10, 10 }, { 20, 20 } };
    const float same[3][2] = { { 5, 5 }, { 5, 5 }, { 5, 5 } };
    CHECK(FEQ(nv2a_tex_mag_triangle(xy, uv), 4.0));
    CHECK(nv2a_tex_mag_triangle(line, uv) == 0.0);   /* no area */
    CHECK(nv2a_tex_mag_triangle(xy, same) == 0.0);   /* uv does not vary */
}

/* A dump-built pack matches a P8 upload: the hash the upload computes names
 * the dump's file and keys the pack; this is the whole route a game without
 * an archive reader has. */
static void test_p8(void)
{
    static uint8_t mem[0x10000];
    uint8_t pack[NV2A_TEX_PACK_HDR + NV2A_TEX_PACK_ENTRY + 16 * 16 * 4];
    struct nv2a_stage t = stage(0x1000, NV2A_TEX_P8, 4, 4, 1);
    uint32_t texels[16] = { 0 };
    char name[64], err[128] = "";
    uint64_t h, other;
    size_t idx = NV2A_TEX_PACK_HDR, pay = idx + NV2A_TEX_PACK_ENTRY;
    unsigned i;
    FILE *f;

    t.palette = 0x2000;
    t.pal_len = 256;
    for (i = 0; i < 16; i++)
        mem[0x1000 + i] = (uint8_t)i;
    for (i = 0; i < 256 * 4; i++)
        mem[0x2000 + i] = (uint8_t)(i * 7);
    h = nv2a_stage_hash(mem, &t, 16);
    CHECK(h != nv2a_tex_hash(mem + 0x1000, 16, NV2A_TEX_HASH_SEED));

    nv2a_tex_dump_upload(&t, h, texels);
    name_of(name, sizeof name, h, "xtd");
    CHECK(file_size(name) == 64 + 16 * 4);

    /* v3, one world entry at 4x the texels, with no palette of its own. */
    memset(pack, 0, sizeof pack);
    memcpy(pack, NV2A_TEX_PACK_MAGIC, 4);
    pack[4] = 3;
    pack[8] = 1;
    pack[12] = 4;
    pack[16] = (uint8_t)idx;
    pack[24] = 4;
    pack[28] = 1u << XRTP_KIND_WORLD;
    for (i = 0; i < 8; i++)
        pack[idx + i] = (uint8_t)(h >> (i * 8));
    pack[idx + 8] = 16;
    pack[idx + 12] = 16;
    pack[idx + 16] = 1;
    pack[idx + 20] = XRTP_KIND_WORLD;
    pack[idx + 24] = (uint8_t)pay;
    f = fopen("tex_dump_p8.pack", "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    fwrite(pack, 1, sizeof pack, f);
    fclose(f);
    CHECK(nv2a_tex_pack_load("tex_dump_p8.pack", err, sizeof err) == 1);
    CHECK(nv2a_tex_pack_find(h, 4, 4) != NULL);
    /* The same indices under another palette are other texels. */
    mem[0x2000] ^= 0xFF;
    other = nv2a_stage_hash(mem, &t, 16);
    CHECK(other != h);
    CHECK(nv2a_tex_pack_find(other, 4, 4) == NULL);
    nv2a_tex_pack_unload();
    remove("tex_dump_p8.pack");
}

int main(void)
{
    snprintf(s_dir, sizeof s_dir, "tex_dump_out_%d/a/b", (int)getpid());
    CHECK(nv2a_tex_dump_set_dir(s_dir) == 0);
    CHECK(nv2a_tex_dump_on());
    test_write();
    test_linear();
    test_volatile();
    test_rt_oversize();
    test_mag();
    test_mag_triangle();
    test_p8();
#ifndef _WIN32
    test_retry();   /* rename onto a directory fails the POSIX way */
#endif
    nv2a_tex_dump_summary();
    test_cap();
    nv2a_tex_dump_summary();
    test_off();
    if (s_fail)
        return 1;
    printf("tex_dump: ok\n");
    return 0;
}
