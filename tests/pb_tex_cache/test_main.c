/**
 * pb_tex_cache: the CPU rasteriser's decoded-texture cache keys a texture
 * without its level count (the 2736fda review: keyed on it, a texture
 * sampled with mips on one bind and level 0 only on another got two entries
 * that both decoded level 0).
 *
 * A 16x16 swizzled A8R8G8B8 texture with three levels, each one colour
 * (level 0 red, 1 green, 2 blue), drawn with MIN nearest-mip-nearest and MAG
 * nearest, so every pixel is one level's exact colour. A 16x16-pixel quad
 * mapping the whole texture samples level 0 (LOD 0), a 4x4 one level 2
 * (LOD 2, the clamp). The quads run a pass-through vertex program, since
 * only that path mipmaps and caches. Pixels are read back at quad centres;
 * the entry counts come from nv2a_pb_exec_tc_stats().
 *
 *   mips      the mipmapped bind builds one entry; small quad blue, big red
 *   shared    the same texture bound level 0 only (MIN nearest) is served by
 *             that entry: small quad red, no build
 *   rewrite   a flip with a level-0-only bind and then a mipmapped one;
 *             then level 2 rewritten to yellow and, in the next flip, the
 *             same two binds. The first bind's check covers every
 *             level the entry holds, so the second sees yellow
 *   fewer     a second texture bound level 0 only first, then mipmapped:
 *             rebuilt in its own slot (still two entries), blue, and a
 *             level-0 bind after it is a hit
 *   format    the first texture's offset under another format: an entry of
 *             its own
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d8_swizzle.h"
#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

#define W 64u
#define H 64u
#define BUF0  0x01000000u
#define BUF1  0x01010000u
#define TEX_A 0x02800000u
#define TEX_B 0x02810000u
#define LOG2  4u                /* 16x16 */
#define LEVELS 3u

#define RED    0xFFFF0000u
#define GREEN  0xFF00FF00u
#define BLUE   0xFF0000FFu
#define YELLOW 0xFFFFFF00u

/* SET_TEXTURE_FILTER: MAG [27:24] nearest; MIN [21:16] nearest (1) or
 * nearest mip, nearest texel (3). */
#define FILTER_L0  ((1u << 24) | (1u << 16))
#define FILTER_MIP ((1u << 24) | (3u << 16))

static int s_fail;

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void fill_level(uint32_t tex, uint32_t l, uint32_t argb)
{
    uint32_t n = nv2a_tex_level_dim(1u << LOG2, l), x, y;
    uint32_t *p = (uint32_t *)(guest(tex) + nv2a_tex_level_offset(0x06, 1u << LOG2,
                                                                  1u << LOG2, l));
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++)
            p[swizzle_offset(x, y, n, n)] = argb;
}

static void make_texture(uint32_t tex)
{
    fill_level(tex, 0, RED);
    fill_level(tex, 1, GREEN);
    fill_level(tex, 2, BLUE);
}

static void vertex(float x, float y, float u, float v)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(0.0f));
    M(0x1818, f2u(1.0f));
    M(0x1818, 0xFFFFFFFFu);
    M(0x1818, f2u(u));
    M(0x1818, f2u(v));
}

/* A size x size quad at (x, y) mapping the whole texture. */
static void quad(float x, float y, float size)
{
    M(0x17FC, 8);                          /* BEGIN_END: QUADS */
    vertex(x, y, 0, 0);
    vertex(x + size, y, 1, 0);
    vertex(x + size, y + size, 1, 1);
    vertex(x, y + size, 0, 1);
    M(0x17FC, 0);
}

/* mov oPos, v0; mov oD0, v3; mov oT0, v9 (as d3d11_backend_smoke encodes
 * it). Only a vertex-program batch takes the path that mipmaps and caches
 * (raster_batch_vsh); a pass-through batch samples level 0 live. */
static void vertex_program(void)
{
    static const uint32_t outs[3][2] = {{0, 0}, {3, 3}, {9, 9}}; /* {o, v} */
    int k;
    M(0x1E9C, 0);                          /* PROGRAM_LOAD: slot 0 */
    for (k = 0; k < 3; k++) {
        M(0x0B00 + 16 * k + 0, 0);
        M(0x0B00 + 16 * k + 4, (1u << 21) | (outs[k][1] << 9) | 0x1Bu);
        M(0x0B00 + 16 * k + 8, 2u << 26);  /* A = v */
        M(0x0B00 + 16 * k + 12, (0xFu << 12) | (1u << 11)
                                | (outs[k][0] << 3) | (k == 2 ? 1u : 0u));
    }
    M(0x1EA0, 0);                          /* PROGRAM_START */
    M(0x1E94, 2);                          /* EXECUTION_MODE: program */
}

static void begin_frame(uint32_t buf)
{
    int a;
    M(0x0200, W << 16);                    /* SURFACE_CLIP_HORIZONTAL */
    M(0x0204, H << 16);                    /* SURFACE_CLIP_VERTICAL   */
    M(0x0208, 0x00000128u);                /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    M(0x020C, W * 4u);                     /* SURFACE_PITCH           */
    M(0x0210, buf);                        /* SURFACE_COLOR_OFFSET    */
    M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
    M(0x1D90, 0xFF000000u);                /* COLOR_CLEAR_VALUE       */
    M(0x1D94, 0xF0);                       /* CLEAR_SURFACE: colour   */
    M(0x0304, 0);                          /* BLEND_ENABLE off        */
    for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
    vertex_program();
}

static void end_frame(uint32_t other)
{
    M(0x1B0C, 0);
    M(0x0210, other);
    /* FLIP_INCREMENT_WRITE is what advances the flip count, which is when
     * the cache re-checks an entry's guest bytes. */
    M(0x012C, 0);
    M(0x0130, 0);                          /* FLIP_STALL */
}

/* Stage 0: the texture at tex, format `color`, three levels, 2D, clamped. */
static void stage0(uint32_t tex, uint32_t color, uint32_t filter)
{
    M(0x1B00, tex);                        /* TEXTURE_OFFSET(0) */
    M(0x1B04, (LOG2 << 24) | (LOG2 << 20) | (LEVELS << 16) | (color << 8)
              | (2u << 4) | 2u);           /* 2D */
    M(0x1B08, 0x00000303u);                /* clamp u, clamp v */
    M(0x1B0C, (1u << 30) | (7u << 6));     /* CONTROL0: enable, LOD max 7 */
    M(0x1B14, filter);
}

static uint32_t px(uint32_t buf, uint32_t x, uint32_t y)
{
    return *(const uint32_t *)(guest(buf) + y * W * 4u + x * 4u) | 0xFF000000u;
}

static void expect_px(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    fprintf(stderr, "[pb_tex_cache] %-46s %08X want %08X %s\n", what, got, want,
            ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

static void expect_n(const char *what, uint64_t got, uint64_t want)
{
    int ok = got == want;
    fprintf(stderr, "[pb_tex_cache] %-46s %llu want %llu %s\n", what,
            (unsigned long long)got, (unsigned long long)want, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

int main(void)
{
    static uint8_t xbe[0x1000];
    struct nv2a_pb_tc_stats s0, s;

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_tex_cache] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);             /* the CPU rasteriser */
    make_texture(TEX_A);
    make_texture(TEX_B);
    nv2a_pb_exec_tc_stats(&s0);

    /* mips */
    begin_frame(BUF0);
    stage0(TEX_A, 0x06, FILTER_MIP);
    quad(4, 4, 4);
    quad(40, 4, 16);
    expect_px("mips: 4x4 quad samples level 2", px(BUF0, 6, 6), BLUE);
    expect_px("mips: 16x16 quad samples level 0", px(BUF0, 48, 12), RED);
    nv2a_pb_exec_tc_stats(&s);
    if (s.binds == s0.binds) {
        fprintf(stderr, "[pb_tex_cache] FAIL: no cache bind; the draws missed the fast path\n");
        return 1;
    }
    expect_n("mips: entries", s.entries, s0.entries + 1);
    expect_n("mips: builds", s.builds, s0.builds + 1);

    /* shared */
    stage0(TEX_A, 0x06, FILTER_L0);
    quad(12, 4, 4);
    expect_px("shared: level-0 bind samples level 0", px(BUF0, 14, 6), RED);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("shared: entries", s.entries, s0.entries + 1);
    expect_n("shared: builds (served by the mip entry)", s.builds, s0.builds + 1);
    end_frame(BUF1);

    /* rewrite: first a flip with nothing changed, where a level-0-only bind
     * comes before a mipmapped one that decodes level 2. An entry rechecked
     * over the levels a bind asks for, not those it holds, would now hash
     * level 0 alone, and keep that decoded level 2 past the change below. */
    begin_frame(BUF1);
    stage0(TEX_A, 0x06, FILTER_L0);
    quad(4, 4, 4);
    stage0(TEX_A, 0x06, FILTER_MIP);
    quad(12, 4, 4);
    expect_px("rewrite: before, level 2", px(BUF1, 14, 6), BLUE);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("rewrite: nothing changed, nothing dropped", s.dropped, s0.dropped);
    end_frame(BUF0);
    fill_level(TEX_A, 2, YELLOW);
    begin_frame(BUF1);
    stage0(TEX_A, 0x06, FILTER_L0);
    quad(4, 4, 4);
    stage0(TEX_A, 0x06, FILTER_MIP);
    quad(12, 4, 4);
    expect_px("rewrite: level-0 bind, level 0", px(BUF1, 6, 6), RED);
    expect_px("rewrite: mip bind sees the new level 2", px(BUF1, 14, 6), YELLOW);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("rewrite: builds", s.builds, s0.builds + 1);
    expect_n("rewrite: dropped", s.dropped, s0.dropped + 1);

    /* fewer */
    stage0(TEX_B, 0x06, FILTER_L0);
    quad(20, 4, 4);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("fewer: level-0 bind builds an entry", s.entries, s0.entries + 2);
    stage0(TEX_B, 0x06, FILTER_MIP);
    quad(28, 4, 4);
    expect_px("fewer: rebuilt with mips, level 2", px(BUF1, 30, 6), BLUE);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("fewer: rebuilt in its own slot (entries)", s.entries, s0.entries + 2);
    expect_n("fewer: builds", s.builds, s0.builds + 3);
    stage0(TEX_B, 0x06, FILTER_L0);
    quad(36, 4, 4);
    expect_px("fewer: level-0 bind after, level 0", px(BUF1, 38, 6), RED);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("fewer: level-0 bind after is a hit (builds)", s.builds, s0.builds + 3);

    /* format: X8R8G8B8 (0x07) at TEX_A's offset */
    stage0(TEX_A, 0x07, FILTER_L0);
    quad(44, 4, 4);
    nv2a_pb_exec_tc_stats(&s);
    expect_n("format: another format, its own entry", s.entries, s0.entries + 3);
    end_frame(BUF0);

    if (s_fail) {
        fprintf(stderr, "[pb_tex_cache] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[pb_tex_cache] done: all checks pass\n");
    return 0;
}
