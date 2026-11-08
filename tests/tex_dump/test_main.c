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
 *   rt match  the sidecar's `rt` by address (low 27 bits) and size, whichever
 *             came first, at every address a hash sat at; a target past the
 *             table counts its binds as dropped and marks nothing; the file
 *             parses, and its rows say what was bound
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
 *   recolour  the stage hash is plane ^ palette * 31; the plane hash is the
 *             index bytes' alone; a P8 sidecar names it and the palette,
 *             a non-P8 one neither; <plane>.xti holds the bytes it hashed,
 *             a non-P8 upload writes none; nv2a_stage_pack_find takes the exact
 *             entry for its palette and the plane's recolour entry for any
 *             other, never for a non-P8 stage of the same bytes, nor with
 *             the pack or the kind off; a 1x recolour entry (w = 0) gives
 *             the guest decode bit for bit under two palettes
 *   recolour mips  a swizzled 16x8 P8 plane with three levels: the 1x
 *             recolour entry, found through nv2a_stage_pack_find, gives the
 *             guest decode of every level; a palette changing every flip
 *             turns the plane volatile (the guest texels go up, the miss
 *             says why) and it recolours again once quiet
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
#include <sys/wait.h>
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

/* Nine draws show a texel over one pixel and one sliver over a thousand: the
 * maximum is the sliver's, the 90th percentile the texture's, as a real
 * reading (the largest of its quarter octave), so a 1:1 texture reads 1. */
static void test_mag_quantile(void)
{
    const uint64_t H = 0xF6F6ull, ONE = 0xF7F7ull;
    struct nv2a_stage t = stage(0x40000, 0x06, 4, 4, 1);
    uint32_t texels[16] = { 0 };
    char name[64], *b;
    long len;
    int i;

    for (i = 0; i < 9; i++)
        nv2a_tex_mag_note(H, 2.0, 2);
    nv2a_tex_mag_note(H, 2000.0, 2);
    /* 3.0 and 3.2 share a quarter octave (2.83 to 3.36): the larger. */
    nv2a_tex_mag_note(ONE, 3.0, 1);
    nv2a_tex_mag_note(ONE, 3.2, 1);
    nv2a_tex_dump_upload(&t, H, texels);
    nv2a_tex_dump_upload(&t, ONE, texels);
    nv2a_tex_dump_summary();
    name_of(name, sizeof name, H, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"mag_max\": 2000.000,\n") != NULL);
        CHECK(strstr(b, "\"mag_scale1\": 1000.000,\n") != NULL);
        CHECK(strstr(b, "\"mag_p90\": 2.000,\n") != NULL);
        CHECK(strstr(b, "\"mag_scale1_p90\": 1.000,\n") != NULL);
        CHECK(strstr(b, "\"mag_draws\": 10,\n") != NULL);
        free(b);
    }
    name_of(name, sizeof name, ONE, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"mag_p90\": 3.200,\n") != NULL);
        free(b);
    }
}

/* The shader's span slots for a coordinate range [lo, hi] on one axis. */
static void span_slots_for(uint32_t *sp, double lo, double hi)
{
    uint32_t qlo = (uint32_t)((int32_t)floor(lo * NV2A_TEX_SPAN_ONE) + (int32_t)NV2A_TEX_SPAN_BIAS);
    uint32_t qhi = (uint32_t)((int32_t)floor(hi * NV2A_TEX_SPAN_ONE) + (int32_t)NV2A_TEX_SPAN_BIAS);
    sp[0] = qhi + 1u;
    sp[1] = 2u * NV2A_TEX_SPAN_BIAS - qlo;
}

/* The repeat span: a draw's slots after its four LOD ones, per stage u then
 * v, decoded into repeats of the texture; a p90 per axis over the draws, a
 * maximum, and the sidecar keys. A cleared or impossible pair is no reading,
 * and a pre-transformed draw's span comes from its vertices. */
static void test_span(void)
{
    static uint32_t slots[NV2A_TEX_MAG_SLOTS * 16];
    static struct nv2a_tex_mag_draw draws[16];
    const uint64_t TILE = 0x7171ull, ONCE = 0x7272ull, STRIP = 0x7373ull, PRE = 0x7474ull;
    uint64_t hash[4] = { TILE, ONCE, STRIP, 0 }, n_draws = 0;
    uint32_t scale[4] = { 2, 1, 1, 1 };
    double mx[2], p90[2];
    struct nv2a_tex_span sp;
    struct nv2a_stage t = stage(0x48000, 0x06, 4, 4, 1);
    uint32_t texels[16] = { 0 };
    char name[64], *b;
    unsigned n = 0, i;
    long len;

    CHECK(NV2A_TEX_MAG_SLOTS == 20u && NV2A_TEX_MAG_BYTES == NV2A_TEX_MAG_DRAWS * 80u);
    nv2a_tex_mag_set_on(1);
    memset(slots, 0, sizeof slots);
    for (i = 0; i < 10; i++) {
        uint32_t *d = slots + (size_t)nv2a_tex_mag_draw_id(draws, &n, hash, scale)
                      * NV2A_TEX_MAG_SLOTS;
        d[0] = mag_slot_for(0);
        /* TILE: four repeats across, -1.5 to 2.5, three down; one draw in ten
         * eight across. Repeats are the texture's, whatever the entry scale. */
        span_slots_for(d + 4, -1.5, i == 9 ? 6.5 : 2.5);
        span_slots_for(d + 6, 0.0, 3.0);
        /* ONCE: inside one repeat, from 0.02 to 0.98. */
        span_slots_for(d + 8, 0.02, 0.98);
        span_slots_for(d + 10, 0.02, 0.98);
        /* STRIP: u wraps, v stays in one (a joint strip). */
        span_slots_for(d + 12, 3.0, 9.0);
        span_slots_for(d + 14, 0.0, 0.5);
        /* Stage 3 has no hash: its slots are not read. */
        span_slots_for(d + 16, 0.0, 100.0);
        span_slots_for(d + 18, 0.0, 100.0);
    }
    /* A cleared pair, and a high below a low: no span reading for TILE. */
    {
        uint32_t *d = slots + (size_t)nv2a_tex_mag_draw_id(draws, &n, hash, scale)
                      * NV2A_TEX_MAG_SLOTS;
        d[4] = 0;
        d[5] = 1;
        span_slots_for(d + 6, 1.0, 0.0);
    }
    nv2a_tex_mag_fold(draws, n, slots, 1);
    CHECK(nv2a_tex_span_get(TILE, mx, p90, &n_draws) && n_draws == 10);
    CHECK(FEQ(p90[0], 4.0) && FEQ(p90[1], 3.0) && FEQ(mx[0], 8.0) && FEQ(mx[1], 3.0));
    CHECK(nv2a_tex_span_get(ONCE, mx, p90, &n_draws) && n_draws == 10);
    CHECK(p90[0] < 1.0 && p90[1] < 1.0 && mx[0] < 1.0);
    CHECK(nv2a_tex_span_get(STRIP, mx, p90, NULL) && FEQ(p90[0], 6.0) && p90[1] <= 0.5);
    CHECK(!nv2a_tex_span_get(0, NULL, NULL, NULL));

    /* From vertices: texel coordinates on a 64 x 32 texture. */
    nv2a_tex_span_begin(&sp);
    {
        const float a[2] = { -32.0f, 0.0f }, c[2] = { 160.0f, 16.0f }, far[2] = { 1e30f, 8.0f };
        nv2a_tex_span_take(&sp, a, 64, 32);
        nv2a_tex_span_take(&sp, c, 64, 32);
        nv2a_tex_span_take(&sp, far, 64, 32);   /* u not taken, v is */
        nv2a_tex_span_take(&sp, a, 0, 32);      /* no size: nothing */
    }
    nv2a_tex_span_end(&sp, PRE);
    CHECK(nv2a_tex_span_get(PRE, mx, NULL, NULL) && FEQ(mx[0], 3.0) && FEQ(mx[1], 0.5));
    /* A span alone is no magnification: the sidecar must not read 0. */
    CHECK(!nv2a_tex_mag_get(PRE, NULL, NULL, NULL));
    nv2a_tex_span_begin(&sp);
    nv2a_tex_span_end(&sp, PRE + 1);
    CHECK(!nv2a_tex_span_get(PRE + 1, NULL, NULL, NULL));

    nv2a_tex_dump_upload(&t, TILE, texels);
    nv2a_tex_dump_summary();
    name_of(name, sizeof name, TILE, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "\"mag_draws\": 10,\n") != NULL);
        CHECK(strstr(b, "\"uv_span_u\": 4.000,\n") != NULL);
        CHECK(strstr(b, "\"uv_span_v\": 3.000,\n") != NULL);
        CHECK(strstr(b, "\"uv_span_u_max\": 8.000,\n") != NULL);
        CHECK(strstr(b, "\"uv_span_draws\": 10\n}\n") != NULL);
        free(b);
    }
    nv2a_tex_mag_set_on(0);
}

static void test_mag(void)
{
    static uint32_t slots[NV2A_TEX_MAG_DRAWS * NV2A_TEX_MAG_SLOTS];
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
        CHECK(strstr(b, "\"mag_p90\": 4.000,\n") != NULL);
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
    test_mag_quantile();
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
/* Just enough JSON to say a file the dump wrote is whole: the inventory reads
 * these with a real parser, and a stray comma would lose it the file. */
static const char *json_value(const char *p);

static const char *json_ws(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    return p;
}

static const char *json_string(const char *p)
{
    if (*p != '"')
        return NULL;
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && !*++p)
            return NULL;
    return *p ? p + 1 : NULL;
}

static const char *json_list(const char *p, char close, int keys)
{
    p = json_ws(p + 1);
    if (*p == close)
        return p + 1;
    for (;;) {
        if (keys) {
            if (!(p = json_string(json_ws(p))))
                return NULL;
            p = json_ws(p);
            if (*p++ != ':')
                return NULL;
        }
        if (!(p = json_value(p)))
            return NULL;
        p = json_ws(p);
        if (*p == close)
            return p + 1;
        if (*p++ != ',')
            return NULL;
    }
}

static const char *json_value(const char *p)
{
    char *end;
    p = json_ws(p);
    switch (*p) {
    case '{': return json_list(p, '}', 1);
    case '[': return json_list(p, ']', 0);
    case '"': return json_string(p);
    case 't': return strncmp(p, "true", 4) ? NULL : p + 4;
    case 'f': return strncmp(p, "false", 5) ? NULL : p + 5;
    case 'n': return strncmp(p, "null", 4) ? NULL : p + 4;
    default:
        (void)strtod(p, &end);
        return end == p ? NULL : end;
    }
}

static int json_valid(const char *b)
{
    const char *p = json_value(b);
    return p && !*json_ws(p);
}

/* A row of render_targets.json, found by its offset. */
struct rt_row { unsigned width, height, format; unsigned long long binds; };

static int rt_row_of(const char *b, uint32_t offset, struct rt_row *r)
{
    char key[40];
    const char *p;
    snprintf(key, sizeof key, "{\"offset\": \"0x%08X\"", offset);
    p = strstr(b, key);
    return p && sscanf(p + strlen(key),
                       ", \"width\": %u, \"height\": %u, \"format_id\": %u, \"binds\": %llu}",
                       &r->width, &r->height, &r->format, &r->binds) == 4;
}

static long long rt_dropped_of(const char *b)
{
    const char *p = strstr(b, "\"dropped_binds\": ");
    return p ? atoll(p + strlen("\"dropped_binds\": ")) : -1;
}

static int rt_of(uint64_t hash)
{
    char name[64], *b;
    int on;
    name_of(name, sizeof name, hash, "json");
    b = slurp(name, NULL);
    CHECK(b != NULL);
    if (!b)
        return -1;
    CHECK(json_valid(b));
    on = strstr(b, "\"rt\": true") != NULL;
    free(b);
    return on;
}

/* `rt` against what the backends call the same memory; a session of its own,
 * since it fills the target table. */
static void test_rt_match(const char *dir)
{
    static uint32_t big[4096];
    struct nv2a_tex_dump_stats st;
    struct nv2a_stage a = stage(0x100000, 0x06, 64, 64, 1);
    struct nv2a_stage b = stage(0x200000, 0x06, 64, 64, 1);
    struct nv2a_stage alias = stage(0x300000, 0x06, 64, 64, 1);
    struct nv2a_stage tall = stage(0x400000, 0x06, 64, 32, 1);
    struct nv2a_stage square = tall;
    struct nv2a_stage other_fmt = stage(0x500000, 0x06, 64, 64, 1);
    struct nv2a_stage past = stage(0x1000000 + 1050u * 0x1000u, 0x06, 16, 16, 1);
    uint64_t before;
    char *f;
    unsigned i;

    snprintf(s_dir, sizeof s_dir, "%s", dir);
    CHECK(nv2a_tex_dump_set_dir(s_dir) == 0);

    /* One hash at two addresses, the target at either, either way round. */
    nv2a_tex_dump_upload(&a, 0xA1, big);
    nv2a_tex_dump_upload(&b, 0xA1, big);
    nv2a_tex_dump_rt(&a);
    {
        struct nv2a_stage c = stage(0x210000, 0x06, 64, 64, 1);
        struct nv2a_stage d = stage(0x220000, 0x06, 64, 64, 1);
        nv2a_tex_dump_upload(&c, 0xA2, big);
        nv2a_tex_dump_upload(&d, 0xA2, big);
        nv2a_tex_dump_rt(&d);
        /* And a target first, then the upload that lands on it. */
        struct nv2a_stage e = stage(0x230000, 0x06, 64, 64, 1);
        struct nv2a_stage g = stage(0x240000, 0x06, 64, 64, 1);
        nv2a_tex_dump_rt(&e);
        nv2a_tex_dump_upload(&g, 0xA3, big);
        nv2a_tex_dump_upload(&e, 0xA3, big);
    }
    /* Through another window of the address space, both orders; one row. */
    nv2a_tex_dump_upload(&alias, 0xA4, big);
    alias.offset = 0x80300000u;
    nv2a_tex_dump_rt(&alias);
    {
        struct nv2a_stage up = stage(0x00310000, 0x06, 64, 64, 1);
        struct nv2a_stage rt = stage(0x80310000u, 0x06, 64, 64, 1);
        nv2a_tex_dump_rt(&rt);
        nv2a_tex_dump_upload(&up, 0xA5, big);
        up.offset = 0x00310000;
        nv2a_tex_dump_rt(&up);   /* the same target through the other window */
    }
    /* The size must agree; the format need not (a target has its own). */
    nv2a_tex_dump_upload(&tall, 0xA6, big);
    square.height = 64;
    nv2a_tex_dump_rt(&square);
    nv2a_tex_dump_upload(&other_fmt, 0xA7, big);
    other_fmt.color = 0x12;
    nv2a_tex_dump_rt(&other_fmt);

    /* Past the table: counted, no row, no `rt`. */
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.rt_dropped == 0);
    before = st.rt_targets;
    for (i = 0; i < 1100; i++) {
        struct nv2a_stage t = stage(0x1000000 + i * 0x1000u, 0x06, 16, 16, 1);
        nv2a_tex_dump_rt(&t);
    }
    nv2a_tex_dump_upload(&past, 0xA8, big);
    nv2a_tex_dump_get_stats(&st);
    CHECK(st.rt_targets == 1024);
    CHECK(st.rt_dropped == before + 1100u - 1024u);
    nv2a_tex_dump_summary();

    CHECK(rt_of(0xA1) == 1);
    CHECK(rt_of(0xA2) == 1);
    CHECK(rt_of(0xA3) == 1);
    CHECK(rt_of(0xA4) == 1);
    CHECK(rt_of(0xA5) == 1);
    CHECK(rt_of(0xA6) == 0);
    CHECK(rt_of(0xA7) == 1);
    CHECK(rt_of(0xA8) == 0);

    CHECK((f = slurp("render_targets.json", NULL)) != NULL);
    if (f) {
        struct rt_row r;
        unsigned rows = 0;
        const char *p;
        CHECK(json_valid(f));
        for (p = f; (p = strstr(p, "{\"offset\"")) != NULL; p++)
            rows++;
        CHECK(rows == 1024);
        CHECK(rt_dropped_of(f) == (long long)st.rt_dropped);
        CHECK(rt_row_of(f, 0x100000, &r) && r.binds == 1);
        CHECK(rt_row_of(f, 0x80310000u, &r) && r.binds == 2);
        CHECK(!rt_row_of(f, 0x00310000, &r));
        CHECK(!rt_row_of(f, past.offset, &r));
        free(f);
    }
    nv2a_tex_dump_set_dir(NULL);
}

/* A sidecar an older dump left in the directory has no facts: they are added,
 * once, and the file stays whole. */
static void test_older_sidecar(void)
{
    const uint64_t OLD = 0xF0F0000000000010ull;
    static const char old[] = "{\n  \"hash\": \"F0F0000000000010\",\n  \"width\": 4,\n"
                              "  \"texels\": true\n}\n";
    uint32_t texels[16] = { 0 };
    struct nv2a_stage t = stage(0x7A0000, 0x06, 4, 4, 1);
    uint64_t bound[NV2A_TEX_DUMP_STAGES] = { OLD };
    char name[64], path[640], *b;
    FILE *f;

    nv2a_tex_dump_upload(&t, OLD, texels);
    name_of(name, sizeof name, OLD, "json");
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs(old, f);
    fclose(f);
    nv2a_tex_dump_draw(bound, 1u, 1);
    nv2a_tex_dump_summary();
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(json_valid(b));
        CHECK(strstr(b, "\"texels\": true") != NULL);
        CHECK(strstr(b, "\"bump\": true") != NULL);
        CHECK(strstr(b, "\"drawn_2d\": true") != NULL);
        CHECK(strstr(strstr(b, "\"bump\"") + 1, "\"bump\"") == NULL);
        free(b);
    }
    /* And again over what that wrote: replaced, not added. */
    nv2a_tex_dump_draw(bound, 0, 0);
    nv2a_tex_dump_summary();
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(json_valid(b));
        CHECK(strstr(b, "\"drawn_2d\": false") != NULL);
        CHECK(strstr(strstr(b, "drawn_2d") + 1, "drawn_2d") == NULL);
        free(b);
    }
}

/* What a draw or a bind adds to a sidecar, after the upload wrote it. */
static void test_facts(void)
{
    const uint64_t HU = 0xF0F0000000000001ull, MODEL = 0xF0F0000000000002ull,
                   NONE = 0xF0F0000000000003ull, BUMP = 0xF0F0000000000004ull,
                   RT1 = 0xF0F0000000000005ull, RT2 = 0xF0F0000000000006ull;
    uint32_t texels[16] = { 0 };
    struct nv2a_stage hu = stage(0x700000, 0x06, 4, 4, 1);
    struct nv2a_stage model = stage(0x710000, 0x06, 4, 4, 1);
    struct nv2a_stage none = stage(0x720000, 0x06, 4, 4, 1);
    struct nv2a_stage bump = stage(0x730000, 0x06, 4, 4, 1);
    struct nv2a_stage target = stage(0x740000, 0x06, 64, 64, 1);
    struct nv2a_stage later = stage(0x750000, 0x06, 64, 64, 1);
    uint64_t bound[NV2A_TEX_DUMP_STAGES] = { 0 };
    char name[64], *b;
    static uint32_t big[4096];

    nv2a_tex_dump_upload(&hu, HU, texels);
    nv2a_tex_dump_upload(&model, MODEL, texels);
    nv2a_tex_dump_upload(&none, NONE, texels);
    nv2a_tex_dump_upload(&bump, BUMP, texels);
    nv2a_tex_dump_upload(&target, RT1, big);

    /* First write: nothing drawn, so no say on 2D. */
    name_of(name, sizeof name, HU, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"bump\": false") != NULL);
        CHECK(strstr(b, "\"rt\": false") != NULL);
        CHECK(strstr(b, "drawn_2d") == NULL);
        free(b);
    }

    /* An unknown hash, and an unknown answer, are no facts. */
    bound[0] = 0xF0F00000000000FFull;
    nv2a_tex_dump_draw(bound, 1, 0);
    CHECK(nv2a_tex_dump_draw_open(bound) == 0);
    bound[0] = NONE;
    CHECK(nv2a_tex_dump_draw_open(bound) == 1);
    nv2a_tex_dump_draw(bound, 0, -1);

    bound[0] = HU;
    nv2a_tex_dump_draw(bound, 0, 1);
    CHECK(nv2a_tex_dump_draw_open(bound) == 1);
    /* 3D wins whichever comes first, and settles it. */
    bound[0] = MODEL;
    nv2a_tex_dump_draw(bound, 0, 1);
    nv2a_tex_dump_draw(bound, 0, 0);
    CHECK(nv2a_tex_dump_draw_open(bound) == 0);
    nv2a_tex_dump_draw(bound, 0, 1);
    /* The mask is per stage: only stage 1 is read as slopes. */
    bound[0] = HU;
    bound[1] = BUMP;
    nv2a_tex_dump_draw(bound, 2u, -1);
    memset(bound, 0, sizeof bound);

    /* A target bound at the address of a texture, in both orders. */
    nv2a_tex_dump_rt(&target);
    nv2a_tex_dump_rt(&target);
    nv2a_tex_dump_rt(&later);
    nv2a_tex_dump_upload(&later, RT2, big);
    nv2a_tex_dump_summary();

    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"drawn_2d\": true") != NULL);
        CHECK(strstr(b, "\"bump\": false") != NULL);
        CHECK(strstr(b, "\"texels\": true") != NULL);
        CHECK(strstr(strstr(b, "\"bump\"") + 1, "\"bump\"") == NULL);  /* replaced, not added */
        CHECK(strcmp(b + strlen(b) - 2, "}\n") == 0);
        free(b);
    }
    name_of(name, sizeof name, MODEL, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"drawn_2d\": false") != NULL);
        free(b);
    }
    name_of(name, sizeof name, NONE, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "drawn_2d") == NULL);
        free(b);
    }
    name_of(name, sizeof name, BUMP, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"bump\": true") != NULL);
        free(b);
    }
    name_of(name, sizeof name, RT1, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"rt\": true") != NULL);
        free(b);
    }
    name_of(name, sizeof name, RT2, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"rt\": true") != NULL);
        free(b);
    }
    /* The targets themselves have no hash; they have this list. */
    CHECK((b = slurp("render_targets.json", NULL)) != NULL);
    if (b) {
        struct rt_row r;
        CHECK(json_valid(b));
        CHECK(rt_row_of(b, 0x740000, &r) && r.width == 64 && r.height == 64 && r.format == 6
              && r.binds == 2);
        CHECK(rt_row_of(b, 0x750000, &r) && r.binds == 1);
        CHECK(rt_dropped_of(b) == 0);
        free(b);
    }

    /* Facts and the trace's figures share the sidecar, each once. */
    nv2a_tex_mag_set_on(1);
    nv2a_tex_mag_note(HU, 4.0, 2);
    nv2a_tex_dump_summary();
    bound[0] = HU;
    nv2a_tex_dump_draw(bound, 1u, 0);
    nv2a_tex_mag_note(HU, 8.0, 2);
    nv2a_tex_dump_summary();
    nv2a_tex_mag_set_on(0);
    name_of(name, sizeof name, HU, "json");
    CHECK((b = slurp(name, NULL)) != NULL);
    if (b) {
        CHECK(strstr(b, "\"drawn_2d\": false") != NULL);
        CHECK(strstr(b, "\"bump\": true") != NULL);
        CHECK(strstr(b, "\"mag_max\": 8.000") != NULL);
        CHECK(strstr(strstr(b, "mag_max") + 1, "mag_max") == NULL);
        CHECK(strstr(strstr(b, "\"bump\"") + 1, "\"bump\"") == NULL);
        CHECK(strstr(b, "\"render_scale\": 2\n}\n") != NULL);
        free(b);
    }
}

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

/* v5 pack image: an exact entry for `exact` (2x2 opaque white over the
 * 4x4 texture's aspect: any texels do) and a 4x4 recolour entry for
 * `plane` whose payload is `idx`, sorted by hash. */
static size_t build_v5(uint8_t *buf, uint64_t exact, uint64_t plane, const uint32_t idx[16])
{
    size_t idx_off = NV2A_TEX_PACK_HDR, pay = idx_off + 2 * NV2A_TEX_PACK_ENTRY;
    size_t pay_e = pay, pay_r = pay + 4 * 4, end = pay_r + 16 * 4;
    int exact_first = exact < plane;
    uint8_t *ie = buf + idx_off + (exact_first ? 0 : NV2A_TEX_PACK_ENTRY);
    uint8_t *ir = buf + idx_off + (exact_first ? NV2A_TEX_PACK_ENTRY : 0);
    unsigned i;
    memset(buf, 0, end);
    memcpy(buf, NV2A_TEX_PACK_MAGIC, 4);
    buf[4] = 5;
    buf[8] = 2;
    buf[12] = 1;
    buf[16] = (uint8_t)idx_off;
    buf[28] = 1u << XRTP_KIND_WORLD;
    for (i = 0; i < 8; i++) {
        ie[i] = (uint8_t)(exact >> (i * 8));
        ir[i] = (uint8_t)(plane >> (i * 8));
    }
    ie[8] = 2; ie[12] = 2; ie[16] = 1; ie[20] = XRTP_KIND_WORLD; ie[24] = (uint8_t)pay_e;
    ir[8] = 4; ir[12] = 4; ir[16] = 1; ir[20] = XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR;
    ir[24] = (uint8_t)pay_r;
    for (i = 0; i < 4 * 4; i++)
        buf[pay_e + i] = 0xFF;
    for (i = 0; i < 16; i++) {
        buf[pay_r + i * 4] = (uint8_t)idx[i];
        buf[pay_r + i * 4 + 1] = (uint8_t)(idx[i] >> 8);
        buf[pay_r + i * 4 + 2] = (uint8_t)(idx[i] >> 16);
    }
    return end;
}

static void test_recolour(void)
{
    static uint32_t mem_words[0x4000];   /* palettes are read as words */
    uint8_t *mem = (uint8_t *)mem_words;
    static uint32_t ident[256];
    uint8_t pack[NV2A_TEX_PACK_HDR + 2 * NV2A_TEX_PACK_ENTRY + 4 * 4 + 16 * 4];
    struct nv2a_stage t = stage(0x1000, NV2A_TEX_P8, 4, 4, 1), a = stage(0x1000, 0x06, 2, 2, 1);
    uint32_t idx[16], want[16], texels[16] = { 0 };
    const uint32_t *got;
    struct nv2a_tex_pack_entry *e;
    char name[64], err[128] = "", *b;
    uint64_t plane, h, h2;
    unsigned i, p;
    long len;
    FILE *f;

    t.palette = 0x2000;
    t.pal_len = 256;
    for (i = 0; i < 16; i++)
        mem[0x1000 + i] = (uint8_t)(i * 13 + 5);
    for (i = 0; i < 256 * 4; i++)
        mem[0x2000 + i] = (uint8_t)(i * 7 + 1);
    plane = nv2a_stage_plane_hash(mem, &t);
    h = nv2a_stage_hash(mem, &t, 16);
    CHECK(plane == nv2a_tex_hash(mem + 0x1000, 16, NV2A_TEX_HASH_SEED));
    CHECK(h == (plane ^ nv2a_stage_palette_hash(mem, &t) * 31u));

    /* The sidecar: plane hash and the palette, P8 only. */
    nv2a_stage_dump_upload(mem, &t, h, texels);
    name_of(name, sizeof name, h, "json");
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        char want_plane[64], want_pal[32];
        snprintf(want_plane, sizeof want_plane, "\"plane_hash\": \"%016llX\",\n",
                 (unsigned long long)plane);
        snprintf(want_pal, sizeof want_pal, "\"palette_argb\": \"%08X%08X",
                 (unsigned)rd32(mem + 0x2000), (unsigned)rd32(mem + 0x2004));
        CHECK(strstr(b, want_plane) != NULL);
        CHECK(strstr(b, want_pal) != NULL);
        CHECK(strstr(b, "\"palette_len\": 256,\n  \"plane_hash\"") != NULL);
        CHECK(len < 4096);
        free(b);
    }
    /* The index plane: the guest's bytes as hashed, after a 64-byte header
     * keyed by the plane hash. */
    name_of(name, sizeof name, plane, "xti");
    b = slurp(name, &len);
    CHECK(b != NULL && len == NV2A_TEX_DUMP_HDR + 16);
    if (b && len == NV2A_TEX_DUMP_HDR + 16) {
        const uint8_t *u = (const uint8_t *)b;
        CHECK(memcmp(u, NV2A_TEX_DUMP_XTI_MAGIC, 4) == 0);
        CHECK(rd32(u + 8) == (uint32_t)plane && rd32(u + 12) == (uint32_t)(plane >> 32));
        CHECK(rd32(u + 16) == 4 && rd32(u + 20) == 4 && rd32(u + 28) == NV2A_TEX_P8);
        CHECK(rd32(u + 44) == NV2A_TEX_DUMP_HDR && rd32(u + 48) == 16);
        CHECK(memcmp(u + NV2A_TEX_DUMP_HDR, mem + 0x1000, 16) == 0);
        CHECK(nv2a_tex_hash(u + NV2A_TEX_DUMP_HDR, 16, NV2A_TEX_HASH_SEED) == plane);
    }
    free(b);
    nv2a_stage_dump_upload(mem, &a, 0x5151ull, texels);
    name_of(name, sizeof name, 0x5151ull, "xti");
    CHECK(slurp(name, NULL) == NULL);
    name_of(name, sizeof name, 0x5151ull, "json");
    b = slurp(name, NULL);
    CHECK(b && !strstr(b, "plane_hash") && !strstr(b, "palette_argb"));
    free(b);

    /* The guest's indices in decode order: the decode under palette[i] = i. */
    for (i = 0; i < 256; i++)
        ident[i] = i;
    nv2a_tex_decode(mem + 0x1000, NV2A_TEX_P8, 4, 4, 0, 1, ident, 256, idx);
    f = fopen("tex_dump_v5.pack", "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    fwrite(pack, 1, build_v5(pack, h, plane, idx), f);
    fclose(f);
    CHECK(nv2a_tex_pack_load("tex_dump_v5.pack", err, sizeof err) == 2);
    CHECK(nv2a_tex_pack_recolour_count() == 1);

    /* The exact entry for this palette; the recolour one for another. */
    e = nv2a_stage_pack_find(mem, &t, h);
    CHECK(e && !(e->flags & XRTP_FLAG_RECOLOUR) && e->width == 2);
    for (p = 0; p < 2; p++) {
        /* First a full hash the pack has no exact entry for, then a
         * palette the dump never saw. */
        if (p)
            mem[0x2000 + 4 * idx[3]] ^= 0x5A;
        h2 = p ? nv2a_stage_hash(mem, &t, 16) : h ^ 1u;
        CHECK(h2 != h);
        e = nv2a_stage_pack_find(mem, &t, h2);
        CHECK(e && (e->flags & XRTP_FLAG_RECOLOUR) && e->width == 4);
        got = nv2a_stage_pack_texels(mem, &t, e);
        nv2a_tex_decode(mem + 0x1000, NV2A_TEX_P8, 4, 4, 0, 1, (const uint32_t *)(mem + 0x2000),
                        256, want);
        CHECK(got && memcmp(got, want, sizeof want) == 0);
        nv2a_tex_pack_release(e);
        CHECK(e->data != NULL);   /* kept for the plane's next palette */
    }
    /* A non-P8 texture whose full hash is the plane's (the same bytes) is
     * never recoloured. */
    a.width = a.height = 4;
    CHECK(nv2a_stage_pack_find(mem, &a, plane) == NULL);
    /* Switches apply to recolour entries too. */
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 0);
    CHECK(nv2a_stage_pack_find(mem, &t, h ^ 1u) == NULL);
    nv2a_tex_pack_set_kind_enabled(XRTP_KIND_WORLD, 1);
    nv2a_tex_pack_set_enabled(0);
    CHECK(nv2a_stage_pack_find(mem, &t, h ^ 1u) == NULL);
    nv2a_tex_pack_set_enabled(1);
    CHECK(nv2a_stage_pack_find(mem, &t, h ^ 1u) != NULL);
    nv2a_tex_pack_unload();
    CHECK(nv2a_tex_pack_recolour_count() == 0);
    remove("tex_dump_v5.pack");
}

/* A v5 pack of one w x h recolour entry of `levels` for `plane`, payload
 * `idx` (texels of it); returns the file size. */
static size_t build_v5_one(uint8_t *buf, uint64_t plane, uint32_t w, uint32_t h,
                           uint32_t levels, const uint32_t *idx, size_t texels)
{
    size_t ix = NV2A_TEX_PACK_HDR, pay = ix + NV2A_TEX_PACK_ENTRY, i;
    memset(buf, 0, pay);
    memcpy(buf, NV2A_TEX_PACK_MAGIC, 4);
    buf[4] = 5;
    buf[8] = 1;
    buf[12] = 1;
    buf[16] = (uint8_t)ix;
    buf[28] = 1u << XRTP_KIND_WORLD;
    for (i = 0; i < 8; i++)
        buf[ix + i] = (uint8_t)(plane >> (i * 8));
    buf[ix + 8] = (uint8_t)w;
    buf[ix + 12] = (uint8_t)h;
    buf[ix + 16] = (uint8_t)levels;
    buf[ix + 20] = XRTP_KIND_WORLD | XRTP_FLAG_RECOLOUR;
    buf[ix + 24] = (uint8_t)pay;
    for (i = 0; i < texels; i++) {
        buf[pay + i * 4] = (uint8_t)idx[i];
        buf[pay + i * 4 + 1] = (uint8_t)(idx[i] >> 8);
        buf[pay + i * 4 + 2] = (uint8_t)(idx[i] >> 16);
        buf[pay + i * 4 + 3] = 0;
    }
    return pay + texels * 4;
}

static void test_recolour_mips(void)
{
    enum { W = 16, H = 8, LV = 3, N = 16 * 8 + 8 * 4 + 4 * 2 };
    static uint32_t mem_words[0x4000];
    static uint8_t pack[NV2A_TEX_PACK_HDR + NV2A_TEX_PACK_ENTRY + N * 4];
    static uint32_t ident[256], idx[N], want[N];
    uint8_t *mem = (uint8_t *)mem_words;
    struct nv2a_stage t = stage(0x1000, NV2A_TEX_P8, W, H, LV);
    struct nv2a_tex_pack_entry *e;
    const uint32_t *got;
    char err[128] = "";
    uint64_t plane, h;
    unsigned i, k, vol0;
    FILE *f;

    t.palette = 0x2000;
    t.pal_len = 256;
    CHECK(nv2a_tex_extent(NV2A_TEX_P8, W, H, 0, LV) == N);
    for (i = 0; i < N; i++)
        mem[0x1000 + i] = (uint8_t)(i * 29 + 3);
    for (i = 0; i < 256 * 4; i++)
        mem[0x2000 + i] = (uint8_t)(i * 11 + 7);
    for (i = 0; i < 256; i++)
        ident[i] = i;
    /* The indices in decode order, every level unswizzled on its own. */
    nv2a_tex_decode(mem + 0x1000, NV2A_TEX_P8, W, H, 0, LV, ident, 256, idx);
    plane = nv2a_stage_plane_hash(mem, &t);
    f = fopen("tex_dump_v5_mips.pack", "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    fwrite(pack, 1, build_v5_one(pack, plane, W, H, LV, idx, N), f);
    fclose(f);
    CHECK(nv2a_tex_pack_load("tex_dump_v5_mips.pack", err, sizeof err) == 1);
    for (i = 0; i < 4; i++)
        nv2a_tex_pack_flip();   /* past the load's bump */

    h = nv2a_stage_hash(mem, &t, N);
    e = nv2a_stage_pack_find(mem, &t, h);
    CHECK(e && (e->flags & XRTP_FLAG_RECOLOUR) && e->levels == LV && e->texels == N);
    got = nv2a_stage_pack_texels(mem, &t, e);
    nv2a_tex_decode(mem + 0x1000, NV2A_TEX_P8, W, H, 0, LV, (const uint32_t *)(mem + 0x2000),
                    256, want);
    CHECK(got && memcmp(got, want, sizeof want) == 0);
    nv2a_tex_pack_release(e);

    /* A fade: a new palette every flip. BURST of them in the window (the
     * first palette included) are recoloured; the next turns the plane
     * volatile. */
    vol0 = nv2a_tex_pack_volatile_count();
    for (k = 1; k < NV2A_TEX_PACK_RECOLOUR_BURST; k++) {
        mem[0x2000] = (uint8_t)(0x80u + k);
        nv2a_tex_pack_flip();
        CHECK(nv2a_stage_pack_find(mem, &t, nv2a_stage_hash(mem, &t, N)) != NULL);
    }
    mem[0x2000] = 0xEE;
    nv2a_tex_pack_flip();
    h = nv2a_stage_hash(mem, &t, N);
    {
        uint32_t gen = nv2a_tex_pack_gen();
        CHECK(nv2a_stage_pack_find(mem, &t, h) == NULL);
        CHECK(nv2a_tex_pack_volatile_count() == vol0 + 1 && nv2a_tex_pack_gen() != gen);
    }
    {
        struct nv2a_tex_pack_miss m[NV2A_TEX_PACK_MISSES];
        unsigned n, found = 0;
        nv2a_tex_pack_miss(h, W, H, NV2A_TEX_P8, LV);
        n = nv2a_tex_pack_misses(m, NV2A_TEX_PACK_MISSES);
        for (i = 0; i < n; i++)
            found |= m[i].hash == h && m[i].why == NV2A_TEX_PACK_MISS_VOLATILE
                     && m[i].kind == XRTP_KIND_WORLD;
        CHECK(found);
    }
    /* Quiet: QUIET flips with no new palette, then it recolours again. */
    for (i = 0; i < NV2A_TEX_PACK_RECOLOUR_QUIET; i++)
        nv2a_tex_pack_flip();
    mem[0x2000] = 0xEF;
    e = nv2a_stage_pack_find(mem, &t, nv2a_stage_hash(mem, &t, N));
    CHECK(e != NULL);
    got = e ? nv2a_stage_pack_texels(mem, &t, e) : NULL;
    nv2a_tex_decode(mem + 0x1000, NV2A_TEX_P8, W, H, 0, LV, (const uint32_t *)(mem + 0x2000),
                    256, want);
    CHECK(got && memcmp(got, want, sizeof want) == 0);
    nv2a_tex_pack_unload();
    remove("tex_dump_v5_mips.pack");
}

#ifndef _WIN32
/* The exit lines with the trace on and no dump, from a child that exits:
 * a span-only slot prints its spans and no magnification of 0, and is not
 * counted as a texture. */
static void test_mag_exit_lines(void)
{
    const char *name = "exit_lines.txt";
    char path[640], *b;
    long len;
    pid_t pid;
    int st = 0;
    snprintf(path, sizeof path, "%s/%s", s_dir, name);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        FILE *f = freopen(path, "w", stderr);
        if (!f)
            _exit(2);
        nv2a_tex_dump_set_dir(NULL);
        nv2a_tex_mag_set_on(1);
        nv2a_tex_mag_note(0xE1E1ull, 2.0, 1);
        nv2a_tex_span_note(0xE2E2ull, 4.0, 1.0);
        exit(0);
    }
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    b = slurp(name, &len);
    CHECK(b != NULL);
    if (b) {
        CHECK(strstr(b, "mag 000000000000E1E1 mag_max=2.000") != NULL);
        CHECK(strstr(b, "mag 000000000000E2E2 uv_span_u=4.000 uv_span_v=1.000 span_draws=1\n")
              != NULL);
        CHECK(strstr(b, "tex_mag: 1 textures, highest 2.000 (000000000000E1E1)") != NULL);
        free(b);
    }
    remove(path);
}
#endif

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
    test_span();
    test_mag_triangle();
    test_facts();
    test_older_sidecar();
    test_p8();
    test_recolour();
    test_recolour_mips();
#ifndef _WIN32
    test_retry();   /* rename onto a directory fails the POSIX way */
    test_mag_exit_lines();
#endif
    nv2a_tex_dump_summary();
    test_cap();
    nv2a_tex_dump_summary();
    test_off();
    {
        char other[sizeof s_dir];
        snprintf(other, sizeof other, "tex_dump_out_%d/rt", (int)getpid());
        test_rt_match(other);
    }
    if (s_fail)
        return 1;
    printf("tex_dump: ok\n");
    return 0;
}
