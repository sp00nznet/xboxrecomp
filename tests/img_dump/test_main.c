/*
 * img_dump: the debug image writer (src/platform/img_dump.h).
 *
 *   ext       img_dump_ext() is ".png", ".bmp" with dump_format=bmp
 *   png rgb   a BGR24 frame (gradients and noise, an odd width) written as
 *             PNG decodes, with stb_image as an independent decoder, to the
 *             same RGB pixels; no .tmp is left beside it
 *   png 565   the same through a 32-bit source without IMG_DUMP_ALPHA: RGB
 *   alpha     with IMG_DUMP_ALPHA, a fourth byte 0 everywhere gives an RGB
 *             PNG (the rule of `frames compact`), any other an RGBA one
 *             that keeps it
 *   bmp       a .bmp path writes the old file: 24-bit bottom-up, padded
 *             rows, a 54-byte header; 32-bit top-down with IMG_DUMP_ALPHA
 *   budget    dump_budget_mb=1: writes go through until the next would
 *             pass 1 MiB, then are refused (nothing written, nothing
 *             counted) and the texture dump's charge is refused too;
 *             dump_budget_mb=2G is not taken as 2 MiB
 *   enqueue   a PNG is charged when it is queued: the second of two that
 *             cannot both fit is refused by the call itself (0, no file),
 *             the first lands, and the charge settles to its size
 *   sync      (img_dump_sync_test, built with IMG_DUMP_WRITERS 0) no
 *             writer thread: every PNG is on disk when the call returns,
 *             and no drain is registered, however many are written (the
 *             SIGINT handler stays the test's)
 *
 *   img_dump_test FILE.bmp   also times a real dump (24 or 32 bpp) as PNG
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "img_dump.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#include <stdlib.h>
static void set_env(const char *k, const char *v)
{
    _putenv_s(k, v);
}
#else
static void set_env(const char *k, const char *v)
{
    setenv(k, v, 1);
}
#endif

static int fails;
#define CHECK(x)                                                     \
    do {                                                             \
        if (!(x)) {                                                  \
            printf("FAIL line %d: %s\n", __LINE__, #x);              \
            fails++;                                                 \
        }                                                            \
    } while (0)

static void debug(const char *list)
{
    set_env("RECOMP_DEBUG", list);
    recomp_env_reload();
    img_dump_budget_reset();
}

static int exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* w x h pixels, bpp bytes each (B, G, R[, A]), `pitch` a row. */
static uint8_t *frame(uint32_t w, uint32_t h, unsigned bpp, size_t pitch, int alpha)
{
    uint8_t *px = (uint8_t *)calloc(1, pitch * h);
    uint32_t x, y, seed = 12345;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t *p = px + (size_t)y * pitch + (size_t)x * bpp;
            seed = seed * 1103515245u + 12345u;
            p[0] = (uint8_t)(x * 3);
            p[1] = (uint8_t)(y * 5);
            p[2] = (uint8_t)((x + y) & 1 ? seed >> 16 : x ^ y);
            if (bpp == 4)
                p[3] = alpha == 1 ? (uint8_t)(seed >> 8) : alpha == 2 ? 0xFF : 0;
        }
    return px;
}

/* Decoded PNG == the source: n channels, RGB(A) of B, G, R(, A). */
static int same_png(const char *path, const uint8_t *px, uint32_t w, uint32_t h,
                    unsigned bpp, size_t pitch, int want_n)
{
    int dw, dh, n, ok = 1;
    uint32_t x, y;
    uint8_t *d = stbi_load(path, &dw, &dh, &n, 0);

    if (!d)
        return 0;
    if ((uint32_t)dw != w || (uint32_t)dh != h || n != want_n)
        ok = 0;
    for (y = 0; ok && y < h; y++)
        for (x = 0; ok && x < w; x++) {
            const uint8_t *s = px + (size_t)y * pitch + (size_t)x * bpp;
            const uint8_t *o = d + ((size_t)y * w + x) * (size_t)n;
            if (o[0] != s[2] || o[1] != s[1] || o[2] != s[0] || (n == 4 && o[3] != s[3]))
                ok = 0;
        }
    stbi_image_free(d);
    return ok;
}

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)*len);
    if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static void test_ext(void)
{
    debug("");
    CHECK(strcmp(img_dump_ext(), ".png") == 0);
    debug("dump_format=bmp");
    CHECK(strcmp(img_dump_ext(), ".bmp") == 0);
    debug("dump_format=png");
    CHECK(strcmp(img_dump_ext(), ".png") == 0);
}

static void test_png(void)
{
    const uint32_t w = 37, h = 23;
    size_t pitch3 = w * 3 + 5, pitch4 = w * 4;
    uint8_t *a = frame(w, h, 3, pitch3, 0);
    uint8_t *b = frame(w, h, 4, pitch4, 1);
    uint8_t *z = frame(w, h, 4, pitch4, 0);
    uint8_t *ff = frame(w, h, 4, pitch4, 2);

    debug("dump_budget_mb=0");
    CHECK(img_dump_write("rgb.png", a, pitch3, w, h, 3, 0));
    CHECK(img_dump_flush(5000));
    CHECK(same_png("rgb.png", a, w, h, 3, pitch3, 3));
    CHECK(!exists("rgb.png.tmp"));
    /* A second write replaces the file. */
    CHECK(img_dump_write("rgb.png", a, pitch3, w, h, 3, 0));
    CHECK(img_dump_flush(5000));
    CHECK(same_png("rgb.png", a, w, h, 3, pitch3, 3));

    CHECK(img_dump_write("drop.png", b, pitch4, w, h, 4, 0));
    CHECK(img_dump_flush(5000));
    CHECK(same_png("drop.png", b, w, h, 4, pitch4, 3));
    CHECK(img_dump_write("keep.png", b, pitch4, w, h, 4, IMG_DUMP_ALPHA));
    CHECK(img_dump_flush(5000));
    CHECK(same_png("keep.png", b, w, h, 4, pitch4, 4));
    CHECK(img_dump_write("zero.png", z, pitch4, w, h, 4, IMG_DUMP_ALPHA));
    CHECK(img_dump_flush(5000));
    CHECK(same_png("zero.png", z, w, h, 4, pitch4, 3));
    /* A constant 0xFF is kept, as compact keeps it. */
    CHECK(img_dump_write("ff.png", ff, pitch4, w, h, 4, IMG_DUMP_ALPHA));
    CHECK(img_dump_flush(5000));
    CHECK(same_png("ff.png", ff, w, h, 4, pitch4, 4));

    CHECK(!img_dump_write("bad.png", a, pitch3, 0, h, 3, 0));
    CHECK(!img_dump_write("bad.png", a, pitch3, w, h, 2, 0));
    CHECK(!exists("bad.png"));
    free(a);
    free(b);
    free(z);
    free(ff);
}

static void test_bmp(void)
{
    const uint32_t w = 5, h = 3;
    size_t pitch = w * 4;
    uint8_t *px = frame(w, h, 4, pitch, 1), *f;
    long len = 0;
    uint32_t x, y;
    int32_t ih;

    debug("");
    CHECK(img_dump_write("old.bmp", px, pitch, w, h, 4, 0));
    f = slurp("old.bmp", &len);
    CHECK(f && len == 54 + 16 * 3);   /* 15 bytes a row, padded to 16 */
    if (f && len == 54 + 16 * 3) {
        memcpy(&ih, f + 22, 4);
        CHECK(f[0] == 'B' && f[1] == 'M' && f[28] == 24 && ih == 3);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t *s = px + (size_t)(h - 1 - y) * pitch + x * 4;
                const uint8_t *o = f + 54 + y * 16 + x * 3;
                CHECK(o[0] == s[0] && o[1] == s[1] && o[2] == s[2]);
            }
    }
    free(f);
    CHECK(img_dump_write("old32.bmp", px, pitch, w, h, 4, IMG_DUMP_ALPHA));
    f = slurp("old32.bmp", &len);
    CHECK(f && len == 54 + 20 * 3);
    if (f && len == 54 + 20 * 3) {
        memcpy(&ih, f + 22, 4);
        CHECK(f[28] == 32 && ih == -3);
        CHECK(memcmp(f + 54, px, 20 * 3) == 0);
    }
    free(f);
    free(px);
}

static void test_budget(void)
{
    const uint32_t w = 256, h = 256;
    uint8_t *px = frame(w, h, 3, w * 3, 0);
    char name[64];
    int i, written = 0;

    debug("dump_budget_mb=1");
    CHECK(img_dump_budget_cap() == (1u << 20));
    /* 196662 bytes a BMP: five fit in 1 MiB, the sixth does not. */
    for (i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "budget_%d.bmp", i);
        written += img_dump_write(name, px, w * 3, w, h, 3, 0);
    }
    CHECK(written == 5);
    CHECK(!exists("budget_5.bmp"));
    CHECK(img_dump_budget_used() == 5u * 196662u);
    CHECK(!img_dump_budget_take(1u << 20));
    CHECK(img_dump_budget_take(1000));
    debug("dump_budget_mb=0");
    CHECK(img_dump_budget_cap() == 0);
    CHECK(img_dump_budget_take(1ull << 40));
    /* atol read "2G" as 2 MiB; a unit is not taken, the default stands. */
    debug("dump_budget_mb=2G");
#ifdef __APPLE__
    CHECK(img_dump_budget_cap() == (2048ull << 20));
#else
    CHECK(img_dump_budget_cap() == 0);
#endif
    free(px);
}

static long file_size(const char *p)
{
    long n = -1;
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fclose(f);
    }
    return n;
}

/* Noise in every byte: a PNG of it keeps about its raw size, so two of
 * 640x480 RGB (922 KB each) never fit in 1 MiB, however fast the first
 * one's deflate settles its charge. */
static void test_enqueue(void)
{
    const uint32_t w = 640, h = 480;
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 3);
    uint32_t i, seed = 99;
    long sz;

    for (i = 0; i < w * h * 3; i++) {
        seed = seed * 1103515245u + 12345u;
        px[i] = (uint8_t)(seed >> 16);
    }
    remove("enq_0.png");
    remove("enq_1.png");
    debug("dump_budget_mb=1");
    CHECK(img_dump_write("enq_0.png", px, w * 3, w, h, 3, 0));
    /* Refused here, not later on a writer: a 0, and never a file. */
    CHECK(!img_dump_write("enq_1.png", px, w * 3, w, h, 3, 0));
    CHECK(img_dump_flush(10000));
    CHECK(exists("enq_0.png") && !exists("enq_1.png"));
    CHECK(same_png("enq_0.png", px, w, h, 3, w * 3, 3));
    sz = file_size("enq_0.png");
    CHECK(sz > 0 && img_dump_budget_used() == (uint64_t)sz);
    CHECK(img_dump_budget_used() <= img_dump_budget_cap());
    free(px);
}

#ifdef IMG_DUMP_TEST_SYNC
#include <signal.h>
static void test_handler(int sig)
{
    (void)sig;
}

/* No writer thread: each PNG is written by the call, nothing is queued,
 * and the drains are not registered (a retry per write once registered
 * them again, and the second SIGINT handler saved over itself). */
static void test_sync(void)
{
    const uint32_t w = 31, h = 17;
    uint8_t *px = frame(w, h, 3, w * 3, 0);
    char name[64];
    int i;
#ifndef _WIN32
    struct sigaction sa, now;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = test_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
#endif
    debug("dump_budget_mb=0");
    for (i = 0; i < 4; i++) {
        snprintf(name, sizeof name, "sync_%d.png", i);
        remove(name);
        CHECK(img_dump_write(name, px, w * 3, w, h, 3, 0));
        /* No flush: on disk already. */
        CHECK(same_png(name, px, w, h, 3, w * 3, 3));
    }
    CHECK(img_dump_flush(0));
#ifndef _WIN32
    sigaction(SIGINT, NULL, &now);
    CHECK(now.sa_handler == test_handler);
#endif
    free(px);
}
#endif

/* Wall time: clock() would add up the writer threads' CPU. */
static double now_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Many PNGs at once: every one queued, every one on disk after the flush,
 * each with its own pixels, and no .tmp left. */
static void test_queue(void)
{
    const uint32_t w = 160, h = 120;
    char name[64];
    int i, ok = 0;
    uint8_t *px[24];

    debug("dump_budget_mb=0");
    for (i = 0; i < 24; i++) {
        px[i] = frame(w, h, 4, w * 4, i % 3);
        px[i][0] = (uint8_t)i; /* tell them apart */
        snprintf(name, sizeof name, "queue_%02d.png", i);
        ok += img_dump_write(name, px[i], w * 4, w, h, 4, IMG_DUMP_ALPHA);
    }
    CHECK(ok == 24);
    CHECK(img_dump_flush(10000));
    for (i = 0; i < 24; i++) {
        char tmp[80];
        snprintf(name, sizeof name, "queue_%02d.png", i);
        snprintf(tmp, sizeof tmp, "%s.tmp", name);
        CHECK(same_png(name, px[i], w, h, 4, w * 4, 3) || same_png(name, px[i], w, h, 4, w * 4, 4));
        CHECK(!exists(tmp));
        free(px[i]);
    }
    /* Nothing pending: a flush returns at once. */
    CHECK(img_dump_flush(0));
}

/* A real dump, timed: what a flip dump costs the thread that writes it. */
static int time_file(const char *path)
{
    long len = 0;
    uint8_t *f = slurp(path, &len);
    uint32_t w, h, bits, off, y;
    int32_t ih;
    uint8_t *px;
    size_t row;
    double t0, ms, enc;
    long png = 0;
    uint8_t *p;

    if (!f || len < 54)
        return 1;
    memcpy(&off, f + 10, 4);
    memcpy(&w, f + 18, 4);
    memcpy(&ih, f + 22, 4);
    bits = f[28];
    h = (uint32_t)(ih < 0 ? -ih : ih);
    row = ((size_t)w * (bits / 8) + 3) & ~(size_t)3;
    px = (uint8_t *)malloc(row * h);
    for (y = 0; y < h; y++)
        memcpy(px + row * y, f + off + row * (ih < 0 ? y : h - 1 - y), row);
    debug("dump_budget_mb=0");
    t0 = now_ms();
    img_dump_write("timed.png", px, row, w, h, bits / 8, IMG_DUMP_ALPHA);
    ms = now_ms() - t0;
    t0 = now_ms();
    img_dump_flush(10000);
    enc = now_ms() - t0;
    p = slurp("timed.png", &png);
    printf("img_dump: %s %ux%u %u bpp: BMP %ld bytes, PNG %ld bytes (%.1f%%), caller %.1f ms, "
           "writer %.1f ms; "
           "pixels %s\n",
           path, w, h, bits, len, png, 100.0 * (double)png / (double)len, ms, enc,
           same_png("timed.png", px, w, h, bits / 8, row, 3) ||
                   same_png("timed.png", px, w, h, bits / 8, row, 4)
               ? "same"
               : "DIFFER");
    free(p);
    free(px);
    free(f);
    return 0;
}

int main(int argc, char **argv)
{
    test_ext();
    test_png();
    test_bmp();
    test_queue();
    test_budget();
    test_enqueue();
#ifdef IMG_DUMP_TEST_SYNC
    test_sync();
#endif
    if (argc > 1)
        time_file(argv[1]);
    if (fails) {
        printf("img_dump: %d failed\n", fails);
        return 1;
    }
    printf("img_dump: ok\n");
    return 0;
}
