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
 */
#include "nv2a_tex_dump.h"
#include "nv2a_backend_common.h"

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

int main(void)
{
    snprintf(s_dir, sizeof s_dir, "tex_dump_out_%d/a/b", (int)getpid());
    CHECK(nv2a_tex_dump_set_dir(s_dir) == 0);
    CHECK(nv2a_tex_dump_on());
    test_write();
    test_linear();
    test_volatile();
    test_rt_oversize();
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
