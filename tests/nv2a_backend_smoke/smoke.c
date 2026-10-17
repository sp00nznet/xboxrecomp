/**
 * nv2a_backend_smoke: a GPU render backend against the CPU rasteriser, on a
 * POSIX host, frame by frame.
 *
 * The POSIX, backend-neutral successor of tests/d3d11_backend_smoke for the
 * scenes a backend draws first. Each scene is a hand-written method stream
 * (surface, clear, quads, FLIP_STALL) sent to nv2a_pb_exec_method() twice:
 * once with the CPU rasteriser, once with the backend under test. Both leave
 * the presented frame in guest memory -- the CPU path draws there, a GPU
 * backend writes its present surface back when asked (sync_guest, as the
 * executor's dumps ask) -- so the comparison is the same bytes RECOMP_FB_DUMP
 * reads.
 *
 * Then the write-back itself (Metal): nothing is written at a flip under the
 * default metal_writeback=lazy, sync_guest writes the frame once and a second
 * call finds it clean, render.scale 2 writes the box-filtered host pixels,
 * and metal_writeback=always writes at the flip as before. With the argument
 * "guard" it runs metal_fb_guard alone (the guard is decided at the first
 * flip, so it needs its own process): a read of the present surface after a
 * lazy flip switches the run to writing back at every flip.
 *
 * Then the clip extents (metal-present-mirror, D3D11's clip_cases), each on
 * the CPU path and on Metal, the whole frame compared: a clear bounded by its
 * clip, one bounded by SET_CLEAR_RECT, a target grown from 320x240 to
 * 640x480 (contents kept, margin seeded from the guest bytes, at scale 1 and
 * 2), a draw scissored by its clip (at scale 1 and 3), and a flip of a
 * grown target showing only the 320x240 frame. The CPU halves of the clip
 * clear, the clear rect and the draw clip also run before the backend check,
 * for a Mac run without the Metal backend; Proton's check is
 * d3d11_backend_smoke's cpu_clip_cases (this test builds on macOS only).
 *
 * Then render-target ownership (rt-stale-alias, Metal): a target the title
 * rewrote from the CPU samples the new texels and is not written back; one
 * left alone still samples its rendered image; a new target overlapping a
 * dirty one writes that one back first and seeds from it. With the argument
 * "noalias" the same cases run under rt_alias_check=0 and the first must
 * fail, which shows the cases catch the stale target.
 *
 * Scenes:
 *   clear_quad   colour clear + a solid quad (pre-transformed, INLINE_ARRAY)
 *   checker      + a swizzled A8R8G8B8 checker, stage 0, wrap addressing
 *   yuy2         + a linear YUY2 ramp (the movie texture's format), clamp
 *   blend        + an alpha-blended quad over the checker (SRC_ALPHA,
 *                  ONE_MINUS_SRC_ALPHA)
 *   bigidx       a grid of 1200 quads from vertex arrays in guest memory,
 *                indexed by 4800 ARRAY_ELEMENT16 indices in one batch: the
 *                quads from index 4096 on have their own colour
 *   elem32       a triangle list whose last (odd) index is an
 *                ARRAY_ELEMENT32
 *
 * Then the walker's index limit (overflow): a batch past NV_MAX_INDICES
 * warns, counts one overflowed batch and keeps the first NV_MAX_INDICES; with
 * one slot left an ARRAY_ELEMENT16 word still lands its first half; an
 * ARRAY_ELEMENT32 index past 0xFFFF is counted. Read back through
 * nv2a_pb_exec_idx_stats, the summary line's counters.
 *
 * Last, the present pick with a display mode (present-scanout), CPU path: a
 * double-buffered title whose frames record each back buffer at three clips
 * among 19 offscreen targets presents 640x480 on every flip; a mode change
 * empties the scanout set; a single-buffered title keeps its size with 19
 * and with 40 targets (the second past the table, so the eviction
 * protection is what holds it); a table of protected entries still evicts.
 *
 * The CPU and GPU paths both draw the walker's index list, so a batch the
 * walker cut short gives two identical, equally wrong frames. bigidx and
 * elem32 therefore also check the colour of a pixel inside a quad past
 * index 4096 and inside the last triangle, on both paths.
 *
 * Pass: per scene, an exact match. Every scene is screen-space with nearest
 * sampling, so the two paths compute the same bytes; the golden-limit
 * statistics (MAE per channel, share off by more than 8)
 * are printed alongside. The presented surface must also be the one drawn.
 *
 * Backend under test: RECOMP_SMOKE_BACKEND, default "metal" (the only one
 * that writes back on POSIX). RECOMP_SMOKE_DUMP=<prefix> writes
 * <prefix><scene>_{cpu,gpu}.bmp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "d3d8_swizzle.h"
#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
#ifdef __APPLE__
const struct nv2a_pb_backend *nv2a_pb_metal_backend(void);
uint64_t nv2a_pb_metal_writebacks(void);
const uint8_t *nv2a_pb_metal_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh);
int nv2a_pb_metal_shown(uint32_t offset, uint32_t *tw, uint32_t *th,
                        uint32_t *sw, uint32_t *sh);
#endif

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

#define W 640u
#define H 480u
#define BUF0      0x01000000u
#define BUF1      0x0112C000u
#define BUF2      0x01258000u   /* the render.scale 2 case: a new target */
#define BUF3      0x01600000u   /* clip_cases' grow: clear of every other buffer */
#define BUF4      0x01740000u   /* and its render.scale 2 twin */
#define BUF5      0x01880000u   /* the draw clip at render.scale 3 */
#define BUF6      0x01A00000u   /* redraw_case: a 64x64 R5G6B5 target */
#define REPORT    0x00F00000u   /* occ_case's report: GET_REPORT takes 24 bits */
#define TEX_GUEST 0x02800000u   /* 64x64 swizzled A8R8G8B8, then YUY2 64x32 */
#define RTT0      0x01400000u   /* 64x64 render-to-texture targets, pitch 256 */
#define RTT1      0x01500000u
#define TEX_LOG2  6
#define VB_GUEST  0x02000000u   /* bigidx and elem32 vertex arrays */

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

/* Inline vertex: attr0 float4 position (pixels), attr3 D3DCOLOR, and with
 * `uv`, attr9 float2 texture coordinate. */
static void vertex_uv(float x, float y, uint32_t argb, const float *uv)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(0.0f));
    M(0x1818, f2u(1.0f));
    M(0x1818, argb);
    if (uv) {
        M(0x1818, f2u(uv[0]));
        M(0x1818, f2u(uv[1]));
    }
}

static void quad_uv(float x0, float y0, float x1, float y1, const uint32_t c[4],
                    const float uv[4][2])
{
    M(0x17FC, 8);                          /* BEGIN_END: QUADS */
    vertex_uv(x0, y0, c[0], uv ? uv[0] : NULL);
    vertex_uv(x1, y0, c[1], uv ? uv[1] : NULL);
    vertex_uv(x1, y1, c[2], uv ? uv[2] : NULL);
    vertex_uv(x0, y1, c[3], uv ? uv[3] : NULL);
    M(0x17FC, 0);
}

/* The swizzled checker (8x8-texel orange/blue squares, white top row, green
 * left column so orientation shows) and the YUY2 ramp after it: luma left
 * to right, Cr top to bottom. The same images as d3d11_backend_smoke. */
static void make_textures(void)
{
    uint32_t n = 1u << TEX_LOG2, x, y;
    uint32_t *p = (uint32_t *)guest(TEX_GUEST);
    uint8_t *y8 = guest(TEX_GUEST) + n * n * 4;

    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            uint32_t c = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFF2040FFu : 0xFFFF8000u;
            if (y == 0) c = 0xFFFFFFFFu;
            if (x == 0) c = 0xFF00FF00u;
            p[swizzle_offset(x, y, n, n)] = c;
        }
    for (y = 0; y < 32; y++)
        for (x = 0; x < 64; x += 2) {
            uint8_t *g = y8 + y * 128 + x * 2;
            g[0] = (uint8_t)(16 + x * 219 / 63);
            g[1] = 128;
            g[2] = (uint8_t)(16 + (x + 1) * 219 / 63);
            g[3] = (uint8_t)(128 + y * 3);
        }
}

enum { SC_CLEAR_QUAD, SC_CHECKER, SC_YUY2, SC_BLEND, SC_BIGIDX, SC_ELEM32, SC_COUNT };
static const char *const s_scene_name[SC_COUNT] = {"clear_quad", "checker", "yuy2", "blend",
                                                   "bigidx", "elem32"};

/* bigidx: 40 x 30 cells of 16 pixels, a 12-pixel quad in each, one batch. */
#define GRID_COLS  40u
#define GRID_QUADS 1200u
#define COL_LOW    0xFF2080E0u          /* quads before index 4096 */
#define COL_HIGH   0xFFE0C020u          /* quads from index 4096 on */
#define COL_TRI    0xFF40E040u          /* elem32's last triangle */

/* Pixels whose colour each path must show (x, y, expected X8R8G8B8). */
struct probe { uint32_t x, y, argb; };
static const struct probe s_probe_bigidx[] = {
    {20 * 16 + 8, 27 * 16 + 8, COL_HIGH},   /* quad 1100: index 4400 */
    {10 * 16 + 8,  0 * 16 + 8, COL_LOW},    /* quad 10 */
};
static const struct probe s_probe_elem32[] = {
    {470, 370, COL_TRI},                    /* the last triangle's corner */
    {150, 330, 0xFFE02020u},                /* the red rectangle */
};

/* Vertex arrays at VB_GUEST: attr0 float4 position (pixels, w 1) and attr3
 * D3DCOLOR, interleaved with a 20-byte stride. */
static void vb_vertex(uint32_t i, float x, float y, uint32_t argb)
{
    uint8_t *v = guest(VB_GUEST) + i * 20u;
    float p[4] = {x, y, 0.0f, 1.0f};
    memcpy(v, p, 16);
    memcpy(v + 16, &argb, 4);
}

static void vb_arrays(void)
{
    int a;
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? (20u << 8) | 0x42u : a == 3 ? (20u << 8) | 0x40u : 0x02u);
    M(0x1720 + 4 * 0, VB_GUEST);           /* VERTEX_DATA_ARRAY_OFFSET */
    M(0x1720 + 4 * 3, VB_GUEST + 16u);
}

/* One quad list of GRID_QUADS * 4 indices, sent as ARRAY_ELEMENT16 pairs. */
static void scene_bigidx(void)
{
    uint32_t q, i;

    for (q = 0; q < GRID_QUADS; q++) {
        float x = (float)((q % GRID_COLS) * 16u + 2u), y = (float)((q / GRID_COLS) * 16u + 2u);
        uint32_t c = q * 4u >= 4096u ? COL_HIGH : COL_LOW;
        vb_vertex(q * 4u + 0u, x, y, c);
        vb_vertex(q * 4u + 1u, x + 12.0f, y, c);
        vb_vertex(q * 4u + 2u, x + 12.0f, y + 12.0f, c);
        vb_vertex(q * 4u + 3u, x, y + 12.0f, c);
    }
    vb_arrays();
    M(0x17FC, 8);                          /* BEGIN_END: QUADS */
    for (i = 0; i < GRID_QUADS * 4u; i += 2)
        M(0x1800, i | ((i + 1u) << 16));   /* ARRAY_ELEMENT16 */
    M(0x17FC, 0);
}

/* Five triangles, fifteen indices: two rectangles of two triangles each
 * and a degenerate one between them, so the count is odd and every visible
 * edge is axis-aligned (the paths agree exactly only there). Seven
 * ARRAY_ELEMENT16 words, then the fifteenth index as ARRAY_ELEMENT32, as
 * the XDK sends an odd count. Without it the green rectangle loses its
 * bottom-left half. */
static void scene_elem32(void)
{
    static const uint16_t list[15] = {0, 1, 2, 0, 2, 3, 4, 4, 4, 5, 6, 7, 5, 7, 8};
    uint32_t k;

    vb_vertex(0, 100, 280, 0xFFE02020u);   /* red rectangle */
    vb_vertex(1, 220, 280, 0xFFE02020u);
    vb_vertex(2, 220, 380, 0xFFE02020u);
    vb_vertex(3, 100, 380, 0xFFE02020u);
    vb_vertex(4, 300, 300, 0xFFE02020u);   /* the degenerate triangle */
    vb_vertex(5, 450, 280, COL_TRI);       /* green rectangle */
    vb_vertex(6, 570, 280, COL_TRI);
    vb_vertex(7, 570, 380, COL_TRI);
    vb_vertex(8, 450, 380, COL_TRI);
    vb_arrays();
    M(0x17FC, 5);                          /* BEGIN_END: TRIANGLES */
    for (k = 0; k < 14; k += 2)
        M(0x1800, list[k] | ((uint32_t)list[k + 1] << 16));
    M(0x1808, list[14]);                   /* ARRAY_ELEMENT32 */
    M(0x17FC, 0);
}

static void stage0_checker(void)
{
    M(0x1B00, TEX_GUEST);                  /* TEXTURE_OFFSET(0) */
    M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
              | (1u << 4) | (2u << 0));    /* A8R8G8B8, 2D, 64x64 */
    M(0x1B08, 0x00000101u);                /* wrap u, wrap v */
    M(0x1B0C, 0x40000000u);                /* CONTROL0: enable */
}

/* One scene into the surface at `buf`, then the flip. */
static void scene(int sc, uint32_t buf, uint32_t other)
{
    /* One colour per quad: the CPU rasteriser's screen-space path shades
     * flat (the first vertex's colour), so a Gouraud quad would measure the
     * reference, not the backend. */
    static const uint32_t solid[4] = {0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u};
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    int a;

    M(0x0200, W << 16);                    /* SURFACE_CLIP_HORIZONTAL */
    M(0x0204, H << 16);                    /* SURFACE_CLIP_VERTICAL   */
    M(0x0208, 0x00000128u);                /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    M(0x020C, W * 4u);                     /* SURFACE_PITCH           */
    M(0x0210, buf);                        /* SURFACE_COLOR_OFFSET    */
    M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
    M(0x1D90, 0xFF603010u);                /* COLOR_CLEAR_VALUE       */
    M(0x1D94, 0xF0);                       /* CLEAR_SURFACE: colour   */
    M(0x0304, 0);                          /* BLEND_ENABLE off        */
    M(0x1B0C, 0);                          /* stage 0 off */
    for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);

    if (sc == SC_BIGIDX || sc == SC_ELEM32) {
        if (sc == SC_BIGIDX)
            scene_bigidx();
        else
            scene_elem32();
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        M(0x0210, other);
        M(0x0130, 0);
        return;
    }

    quad_uv(80.0f, 120.0f, 240.0f, 360.0f, solid, NULL);
    /* Fractional edges: a half-pixel offset in either rasteriser (pixel
     * centres at .5, the top-left rule) moves a whole row or column here. */
    {
        static const uint32_t green[4] = {0xFF00C040u, 0xFF00C040u, 0xFF00C040u, 0xFF00C040u};
        quad_uv(80.3f, 380.5f, 239.6f, 460.4f, green, NULL);
    }

    if (sc >= SC_CHECKER) {
        static const float uv[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
        stage0_checker();
        M(0x1760 + 4 * 9, 0x22u);          /* attr9: float2 */
        quad_uv(440.0f, 40.0f, 620.0f, 220.0f, white, uv);
    }
    if (sc >= SC_YUY2) {
        static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 32}, {0, 32}};
        M(0x1B00, TEX_GUEST + (4u << (2 * TEX_LOG2)));
        M(0x1B04, (0x24u << 8) | (1u << 4) | (2u << 0)); /* LC_CR8YB8CB8YA8 */
        M(0x1B08, 0x00000303u);            /* clamp */
        M(0x1B0C, 0x40000000u);
        M(0x1B10, 128u << 16);             /* CONTROL1: pitch */
        M(0x1B1C, (64u << 16) | 32u);      /* IMAGE_RECT */
        M(0x1760 + 4 * 9, 0x22u);
        quad_uv(440.0f, 260.0f, 620.0f, 350.0f, white, uv);
    }
    if (sc >= SC_BLEND) {
        /* A half-transparent quad (alpha 0x80) across the solid quad and
         * the background; untextured. */
        static const uint32_t c[4] = {0x8000FFFFu, 0x8000FFFFu, 0x8000FFFFu, 0x8000FFFFu};
        M(0x1B0C, 0);
        M(0x1760 + 4 * 9, 0x02u);
        M(0x0304, 1);                      /* BLEND_ENABLE */
        M(0x0344, 0x0302);                 /* SFACTOR: SRC_ALPHA */
        M(0x0348, 0x0303);                 /* DFACTOR: ONE_MINUS_SRC_ALPHA */
        M(0x0350, 0x8006);                 /* EQUATION: ADD */
        quad_uv(160.0f, 200.0f, 400.0f, 300.0f, c, NULL);
        M(0x0304, 0);
    }
    M(0x1760 + 4 * 9, 0x02u);
    M(0x1B0C, 0);

    M(0x0210, other);                      /* next buffer, as the XDK does */
    M(0x0130, 0);                          /* FLIP_STALL */
}

static void write_bmp(const char *path, const uint32_t *px)
{
    uint32_t row = W * 3, filesz = 54 + row * H, y, x;
    uint8_t hdr[54];
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    { uint32_t w = W, h = H; memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); }
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = H; y-- > 0; )
        for (x = 0; x < W; x++) {
            uint32_t v = px[y * W + x];
            uint8_t bgr[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

/* Run scene sc with backend b; its frame into out (W*H X8R8G8B8). The
 * surface is filled with a pattern first, so a backend that writes nothing
 * back cannot pass on the CPU path's leftovers. */
static int run(const struct nv2a_pb_backend *b, int sc, uint32_t *out, int k)
{
    uint32_t buf = (k & 1) ? BUF1 : BUF0, other = (k & 1) ? BUF0 : BUF1, i;
    uint32_t *g = (uint32_t *)guest(buf);
    const struct nv2a_pb_present *p;

    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    nv2a_pb_set_backend(b);
    scene(sc, buf, other);
    p = nv2a_pb_present_state();
    if (p->offset != buf) {
        fprintf(stderr, "[backend-smoke] %s: flip presented 0x%08X, drew 0x%08X\n",
                s_scene_name[sc], p->offset, buf);
        return 0;
    }
    if (b->sync_guest)
        b->sync_guest(p->offset, p->pitch, p->w, p->h);
    memcpy(out, g, W * H * 4);
    return 1;
}

static int compare(const char *name, const uint32_t *a, const uint32_t *b)
{
    const int tol = 8;
    uint64_t sum[3] = {0, 0, 0};
    uint32_t bad = 0, i, maxd[3] = {0, 0, 0};
    double mae[3], bad_frac;
    int c, ok;

    for (i = 0; i < W * H; i++) {
        int off = 0;
        for (c = 0; c < 3; c++) {
            int d = (int)((a[i] >> (8 * c)) & 0xFF) - (int)((b[i] >> (8 * c)) & 0xFF);
            if (d < 0) d = -d;
            sum[c] += (uint64_t)d;
            if ((uint32_t)d > maxd[c]) maxd[c] = (uint32_t)d;
            if (d > tol) off = 1;
        }
        bad += (uint32_t)off;
    }
    for (c = 0; c < 3; c++)
        mae[c] = (double)sum[c] / (double)(W * H);
    bad_frac = (double)bad / (double)(W * H);
    ok = maxd[0] == 0 && maxd[1] == 0 && maxd[2] == 0;   /* exact */
    fprintf(stderr, "[backend-smoke] %-10s %s: mae b/g/r %.3f/%.3f/%.3f max %u/%u/%u"
            " bad %.4f%% at tol %d (exact match required)\n", name,
            ok ? "PASS" : "FAIL", mae[0], mae[1], mae[2], maxd[0], maxd[1], maxd[2],
            100.0 * bad_frac, tol);
    return ok;
}

#ifdef __APPLE__
static int all_pattern(const uint32_t *g)
{
    uint32_t i;
    for (i = 0; i < W * H; i++)
        if (g[i] != 0xFFFF00FFu)
            return 0;
    return 1;
}

static int check(int ok, const char *what)
{
    fprintf(stderr, "[backend-smoke] write-back: %-58s %s\n", what, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* The write-back on demand (metal-zero-copy-present), with clear_quad. `ref`
 * is the CPU rasteriser's clear_quad frame. Returns the failures. */
static int writeback_cases(const struct nv2a_pb_backend *b, const uint32_t *ref,
                           uint32_t *tmp)
{
    const struct nv2a_pb_present *p;
    uint32_t *g;
    uint64_t n0;
    int f = 0;
    uint32_t i;

    nv2a_pb_set_backend(b);

    /* lazy: the flip leaves guest memory alone. */
    g = (uint32_t *)guest(BUF0);
    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    n0 = nv2a_pb_metal_writebacks();
    scene(SC_CLEAR_QUAD, BUF0, BUF1);
    p = nv2a_pb_present_state();
    f += check(all_pattern(g) && nv2a_pb_metal_writebacks() == n0,
               "lazy flip: guest untouched, no write-back");
    b->sync_guest(p->offset, p->pitch, p->w, p->h);
    f += check(!memcmp(g, ref, W * H * 4) && nv2a_pb_metal_writebacks() == n0 + 1,
               "sync_guest: the CPU frame, one write-back");
    b->sync_guest(p->offset, p->pitch, p->w, p->h);
    f += check(nv2a_pb_metal_writebacks() == n0 + 1, "sync_guest again: clean, no write-back");

    /* always: written at the flip, before any sync. */
    recomp_env_set(RENV_METAL_WRITEBACK, "always");
    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    scene(SC_CLEAR_QUAD, BUF0, BUF1);
    f += check(!memcmp(g, ref, W * H * 4), "metal_writeback=always: the CPU frame at the flip");
    recomp_env_set(RENV_METAL_WRITEBACK, NULL);

    /* render.scale 2 on a new target: the box filter of the host pixels. */
    {
        struct nv2a_host_opts o;
        const uint8_t *hp;
        uint32_t hw = 0, hh = 0;
        memset(&o, 0, sizeof o);
        o.render_scale = 2;
        nv2a_host_opts_set(&o);
        g = (uint32_t *)guest(BUF2);
        for (i = 0; i < W * H; i++)
            g[i] = 0xFFFF00FFu;
        scene(SC_CLEAR_QUAD, BUF2, BUF0);
        p = nv2a_pb_present_state();
        n0 = nv2a_pb_metal_writebacks();
        b->sync_guest(p->offset, p->pitch, p->w, p->h);
        hp = nv2a_pb_metal_host_pixels(BUF2, &hw, &hh);
        if (hp && hw == 2 * W && hh == 2 * H)
            nv2a_downscale_box32(hp, hw * 4u, W, H, 2, (uint8_t *)tmp, W * 4u);
        f += check(hp && hw == 2 * W && !memcmp(g, tmp, W * H * 4)
                   && nv2a_pb_metal_writebacks() == n0 + 1,
                   "render.scale 2: the box-filtered host pixels");
        memset(&o, 0, sizeof o);
        nv2a_host_opts_set(&o);
    }
    nv2a_pb_set_backend(NULL);
    return f;
}

/* A 32-bit surface at buf, w x h, pitch w * 4; with `clear`, cleared to argb. */
static void surf(uint32_t buf, uint32_t w, uint32_t h, int clear, uint32_t argb)
{
    M(0x0200, w << 16);
    M(0x0204, h << 16);
    M(0x0208, 0x00000128u);
    M(0x020C, w * 4u);
    M(0x0210, buf);
    M(0x0214, 0x02400000u);
    M(0x0304, 0);
    M(0x1B0C, 0);
    if (clear) {
        M(0x1D90, argb);
        M(0x1D94, 0xF0);
    }
}

/* The present surface drawn, then the flip: the flip count is what tells a
 * target drawn now from one drawn before. */
static void flip_buf0(void)
{
    surf(BUF0, W, H, 1, 0xFF000000u);
    M(0x0210, BUF1);
    M(0x0130, 0);
}

static uint32_t px_at(uint32_t buf, uint32_t pitch, uint32_t x, uint32_t y)
{
    return *(const uint32_t *)(guest(buf) + y * pitch + x * 4u) | 0xFF000000u;
}

/* BUF0 with a quad at (100,100)-(164,164) sampling the 64x64 linear
 * A8R8G8B8 texture at tex, then the flip; returns the quad's centre. */
static uint32_t sample_rtt(const struct nv2a_pb_backend *b, uint32_t tex)
{
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 64}, {0, 64}};
    const struct nv2a_pb_present *p;
    int a;

    surf(BUF0, W, H, 1, 0xFF0000FFu);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
    M(0x1B00, tex);
    M(0x1B04, (0x12u << 8) | (1u << 4) | (2u << 0));   /* linear A8R8G8B8 */
    M(0x1B08, 0x00000303u);
    M(0x1B0C, 0x40000000u);
    M(0x1B10, 256u << 16);
    M(0x1B1C, (64u << 16) | 64u);
    quad_uv(100.0f, 100.0f, 164.0f, 164.0f, white, uv);
    M(0x1B0C, 0);
    M(0x1760 + 4 * 9, 0x02u);
    M(0x0210, BUF1);
    M(0x0130, 0);
    p = nv2a_pb_present_state();
    b->sync_guest(p->offset, p->pitch, p->w, p->h);
    return px_at(BUF0, W * 4u, 132, 132);
}

/* The NV2A's 1/16-pixel truncation of screen positions: the XDK's 0.53125
 * offset lands an aligned edge on a pixel centre (nv2a_screen_snap). 1 on
 * failure. */
static int snap_case(void)
{
    static const float in[][2] = {
        {0.53125f, 0.5f}, {16.53125f, 16.5f}, {640.53125f, 640.5f},
        {320.0f, 320.0f}, {100.0625f, 100.0625f}, {100.09f, 100.0625f},
        /* trunc, not floor: toward zero, as xemu's roundScreenCoords */
        {-0.53125f, -0.5f}, {-0.03f, 0.0f},
    };
    unsigned i;
    int f = 0;
    for (i = 0; i < sizeof in / sizeof in[0]; i++)
        if (nv2a_screen_snap(in[i][0]) != in[i][1]) {
            fprintf(stderr, "[backend-smoke] screen snap: %g -> %g, want %g\n",
                    in[i][0], nv2a_screen_snap(in[i][0]), in[i][1]);
            f = 1;
        }
    return f;
}

/* The XDK's rectangle: every corner at a pixel edge plus 0.53125. Snapped
 * to x.5, its edges sit on pixel centres, and the top-left rule keeps the
 * first column and row and leaves the column and row past the far edges
 * (Burnout 3's pause overlay: the hardware draws columns 0..w-1). One quad at
 * the origin, one inside. On backend b (the CPU rasteriser included).
 * Returns the failures. */
static int edge_case(const struct nv2a_pb_backend *b, const char *name)
{
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    static const struct { uint32_t x, y, lit; } pr[] = {
        {0, 0, 1}, {0, 30, 1}, {30, 0, 1}, {63, 63, 1}, {64, 30, 0}, {30, 64, 0},
        {64, 64, 0},
        {100, 100, 1}, {99, 130, 0}, {130, 99, 0}, {163, 163, 1}, {164, 130, 0},
        {130, 164, 0},
    };
    const struct nv2a_pb_present *p;
    uint32_t i;
    int a, f = 0;

    nv2a_pb_set_backend(b);
    for (i = 0; i < W * H; i++)
        ((uint32_t *)guest(BUF0))[i] = 0xFFFF00FFu;
    surf(BUF0, W, H, 1, 0xFF000000u);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
    quad_uv(0.53125f, 0.53125f, 64.53125f, 64.53125f, white, NULL);
    quad_uv(100.53125f, 100.53125f, 164.53125f, 164.53125f, white, NULL);
    M(0x0210, BUF1);
    M(0x0130, 0);
    p = nv2a_pb_present_state();
    if (b->sync_guest)
        b->sync_guest(p->offset, p->pitch, p->w, p->h);
    for (i = 0; i < sizeof pr / sizeof pr[0]; i++) {
        uint32_t got = px_at(BUF0, W * 4u, pr[i].x, pr[i].y);
        uint32_t want = pr[i].lit ? 0xFFFFFFFFu : 0xFF000000u;
        if (got != want) {
            fprintf(stderr, "[backend-smoke] edges %s: pixel (%u,%u) %08X, want %08X\n",
                    name, pr[i].x, pr[i].y, got, want);
            f++;
        }
    }
    fprintf(stderr, "[backend-smoke] edges %s: 0.53125-offset quads keep column and"
            " row 0, not w and h %s\n", name, f ? "FAIL" : "PASS");
    nv2a_pb_set_backend(NULL);
    return f;
}

/* A 64x64 R5G6B5 target at BUF6 drawn, sampled (a decode: this backend binds
 * only 32-bit targets as they are), drawn again and sampled again, all in one
 * flip -- Burnout 3's speed blur. The second sample must see the second
 * colour: the texture cache hashes once a flip, so the write-back has to
 * mark it stale. Returns the failures. */
static int redraw_case(const struct nv2a_pb_backend *b)
{
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 64}, {0, 64}};
    static const uint32_t col[2] = {0xFFFF0000u, 0xFF00FF00u};
    const struct nv2a_pb_present *p;
    uint32_t got[2];
    int a, k, f = 0;

    nv2a_pb_set_backend(b);
    surf(BUF0, W, H, 1, 0xFF0000FFu);
    for (k = 0; k < 2; k++) {
        M(0x0200, 64u << 16);
        M(0x0204, 64u << 16);
        M(0x0208, 0x00000123u);            /* R5G6B5, Z24S8 */
        M(0x020C, 128u);
        M(0x0210, BUF6);
        M(0x1D90, col[k]);
        M(0x1D94, 0xF0);
        surf(BUF0, W, H, 0, 0);
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
        M(0x1B00, BUF6);
        M(0x1B04, (0x11u << 8) | (1u << 4) | (2u << 0));   /* linear R5G6B5 */
        M(0x1B08, 0x00000303u);
        M(0x1B0C, 0x40000000u);
        M(0x1B10, 128u << 16);
        M(0x1B1C, (64u << 16) | 64u);
        quad_uv(100.0f + 200.0f * (float)k, 100.0f, 164.0f + 200.0f * (float)k, 164.0f,
                white, uv);
        M(0x1B0C, 0);
        M(0x1760 + 4 * 9, 0x02u);
    }
    M(0x0210, BUF1);
    M(0x0130, 0);
    p = nv2a_pb_present_state();
    b->sync_guest(p->offset, p->pitch, p->w, p->h);
    got[0] = px_at(BUF0, W * 4u, 132, 132);
    got[1] = px_at(BUF0, W * 4u, 332, 132);
    for (k = 0; k < 2; k++)
        if (got[k] != col[k]) {
            fprintf(stderr, "[backend-smoke] redraw: sample %d is %08X, want %08X\n", k,
                    got[k], col[k]);
            f++;
        }
    fprintf(stderr, "[backend-smoke] redraw: a 16-bit target resampled in one flip %s\n",
            f ? "FAIL" : "PASS");
    nv2a_pb_set_backend(NULL);
    return f;
}

/* Render-target ownership. Returns the failures. */
static int alias_cases(const struct nv2a_pb_backend *b)
{
    uint64_t n0;
    uint32_t i, c;
    int f = 0;

    nv2a_pb_set_backend(b);

    /* Drawn red, then the title writes green over its memory: the texture
     * is the green, and the red is never written back over it. */
    surf(RTT0, 64, 64, 1, 0xFFFF0000u);
    flip_buf0();
    for (i = 0; i < 64 * 64; i++)
        ((uint32_t *)guest(RTT0))[i] = 0xFF00FF00u;
    n0 = nv2a_pb_metal_writebacks();
    c = sample_rtt(b, RTT0);
    f += check(c == 0xFF00FF00u && px_at(RTT0, 256, 10, 10) == 0xFF00FF00u
               && nv2a_pb_metal_writebacks() == n0 + 1,
               "rewritten target: the title's texels, no write-back");

    /* Drawn red and left alone: render-to-texture as before. */
    surf(RTT1, 64, 64, 1, 0xFFFF0000u);
    flip_buf0();
    c = sample_rtt(b, RTT1);
    f += check(c == 0xFFFF0000u, "untouched target: its rendered image");

    /* A 64x32 target over the lower half of the dirty RTT1: RTT1 is written
     * back first, so the new target's seed is the red RTT1 drew. */
    surf(RTT1, 64, 64, 1, 0xFF00FFFFu);   /* dirty again, cyan, memory still red */
    n0 = nv2a_pb_metal_writebacks();
    {
        static const uint32_t blue[4] = {0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu};
        int a;
        surf(RTT1 + 32u * 256u, 64, 32, 0, 0);
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        quad_uv(0.0f, 0.0f, 8.0f, 8.0f, blue, NULL);
    }
    flip_buf0();
    b->sync_guest(RTT1 + 32u * 256u, 256, 64, 32);
    f += check(px_at(RTT1, 256, 10, 10) == 0xFF00FFFFu
               && px_at(RTT1 + 32u * 256u, 256, 40, 20) == 0xFF00FFFFu
               && px_at(RTT1 + 32u * 256u, 256, 2, 2) == 0xFF0000FFu
               && nv2a_pb_metal_writebacks() == n0 + 2,
               "overlapping target: the old one written back, then seeded");
    nv2a_pb_set_backend(NULL);
    return f;
}

#endif

/* ---- clip extents (metal-present-mirror) ------------------------------------ */

#define PATTERN 0xFFFF00FFu

/* A 32-bit surface at buf with a w x h clip and the 640-pixel pitch every
 * clip case keeps (another pitch is another surface). */
static void surf_clip(uint32_t buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    M(0x0200, (w << 16) | x);
    M(0x0204, (h << 16) | y);
    M(0x0208, 0x00000128u);
    M(0x020C, W * 4u);
    M(0x0210, buf);
    M(0x0214, 0x02400000u);
    M(0x0304, 0);
    M(0x1B0C, 0);
}

static void clear_to(uint32_t argb)
{
    M(0x1D90, argb);
    M(0x1D94, 0xF0);
}

/* SET_CLEAR_RECT (x0, y0)-(x1, y1), both inclusive. */
static void clear_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    M(0x1D98, (x1 << 16) | x0);
    M(0x1D9C, (y1 << 16) | y0);
}

enum { CC_CLIP, CC_RECT, CC_GROW, CC_GROW2, CC_DRAW, CC_DRAW3, CC_RECT2, CC_COUNT };

static void clip_stream(int cc)
{
    switch (cc) {
    case CC_CLIP:   /* red over 640x480, then a 160x120 clip at (100,100) blue,
                     * with the clear rect the clip, as D3D's Clear sends it: a
                     * set rect alone bounds a clear (nv2a_clear_box) */
        surf_clip(BUF0, 0, 0, W, H);
        clear_to(0xFFFF0000u);
        surf_clip(BUF0, 100, 100, 160, 120);
        clear_rect(100, 100, 259, 219);
        clear_to(0xFF0000FFu);
        clear_rect(0, 0, W - 1, H - 1);
        break;
    case CC_RECT:   /* red, then the whole clip with a clear rect, green */
        surf_clip(BUF0, 0, 0, W, H);
        clear_to(0xFFFF0000u);
        clear_rect(300, 300, 339, 339);
        clear_to(0xFF00FF00u);
        clear_rect(0, 0, W - 1, H - 1);
        break;
    case CC_RECT2:  /* red, then a 160x120 clip at (100,100) and a larger clear
                     * rect, green: the rect alone, ended by the clip's far
                     * corner (the pitch surface's end), not cut to the clip */
        surf_clip(BUF0, 0, 0, W, H);
        clear_to(0xFFFF0000u);
        surf_clip(BUF0, 100, 100, 160, 120);
        clear_rect(50, 50, 399, 299);
        clear_to(0xFF00FF00u);
        clear_rect(0, 0, W - 1, H - 1);
        surf_clip(BUF0, 0, 0, W, H);
        break;
    case CC_DRAW:   /* red, then a 160x120 clip at (100,100) and a blue quad */
    case CC_DRAW3: {/* past it on every side: the clip scissors the quad */
        static const uint32_t blue[4] = {0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu};
        uint32_t buf = cc == CC_DRAW3 ? BUF5 : BUF0;
        int a;
        surf_clip(buf, 0, 0, W, H);
        clear_to(0xFFFF0000u);
        surf_clip(buf, 100, 100, 160, 120);
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        quad_uv(40.0f, 40.0f, 320.0f, 300.0f, blue, NULL);
        surf_clip(buf, 0, 0, W, H);
        break;
    }
    default: {      /* 320x240 red, then 640x480 with its lower right green */
        uint32_t buf = cc == CC_GROW ? BUF3 : BUF4;
        surf_clip(buf, 0, 0, 320, 240);
        clear_to(0xFFFF0000u);
        surf_clip(buf, 0, 0, W, H);
        clear_rect(320, 240, W - 1, H - 1);
        clear_to(0xFF00FF00u);
        clear_rect(0, 0, W - 1, H - 1);
        break;
    }
    }
}

static void fill(uint32_t buf, uint32_t argb)
{
    uint32_t *g = (uint32_t *)guest(buf), i;
    for (i = 0; i < W * H; i++)
        g[i] = argb;
}

/* The pixels each clip case must show, on both paths. */
/* The clear rect's (339,339) and (340,339) pin its inclusive max edge. */
static const struct probe s_clip_probe[CC_COUNT][6] = {
    {{50, 50, 0xFFFF0000u}, {150, 150, 0xFF0000FFu}, {300, 300, 0xFFFF0000u}, {0}},
    {{320, 320, 0xFF00FF00u}, {350, 350, 0xFFFF0000u}, {299, 320, 0xFFFF0000u},
     {339, 339, 0xFF00FF00u}, {340, 339, 0xFFFF0000u}, {0}},
    {{100, 100, 0xFFFF0000u}, {400, 300, 0xFF00FF00u}, {400, 100, PATTERN}, {100, 300, PATTERN}},
    {{100, 100, 0xFFFF0000u}, {400, 300, 0xFF00FF00u}, {400, 100, PATTERN}, {100, 300, PATTERN}},
    /* The clip's edges: (100,100) and (259,219) in, one pixel past out. */
    {{100, 100, 0xFF0000FFu}, {259, 219, 0xFF0000FFu}, {99, 150, 0xFFFF0000u},
     {150, 99, 0xFFFF0000u}, {260, 150, 0xFFFF0000u}, {150, 220, 0xFFFF0000u}},
    {{100, 100, 0xFF0000FFu}, {259, 219, 0xFF0000FFu}, {99, 150, 0xFFFF0000u},
     {150, 99, 0xFFFF0000u}, {260, 150, 0xFFFF0000u}, {150, 220, 0xFFFF0000u}},
    /* (50,50)-(259,219) green: above and left of the clip too. */
    {{50, 50, 0xFF00FF00u}, {60, 150, 0xFF00FF00u}, {259, 219, 0xFF00FF00u},
     {49, 150, 0xFFFF0000u}, {260, 150, 0xFFFF0000u}, {150, 220, 0xFFFF0000u}},
};
static const char *const s_clip_name[CC_COUNT] = {"clip clear", "clear rect", "grow",
                                                  "grow x2", "draw clip", "draw clip x3",
                                                  "rect > clip"};

static int clip_probes(int cc, const char *path, const uint32_t *px)
{
    int k, ok = 1;
    for (k = 0; k < 6 && s_clip_probe[cc][k].argb; k++) {
        const struct probe *p = &s_clip_probe[cc][k];
        uint32_t got = px[p->y * W + p->x] | 0xFF000000u;
        int hit = got == p->argb;
        fprintf(stderr, "[backend-smoke] %-10s %s pixel (%u,%u) 0x%08X want 0x%08X %s\n",
                s_clip_name[cc], path, p->x, p->y, got, p->argb, hit ? "PASS" : "FAIL");
        ok &= hit;
    }
    return ok;
}

/* The CPU halves of the clip clear, clear rect, draw clip and rect > clip cases, before the backend
 * check: the CPU path's clear box (nv2a_clear_box) needs no GPU, so a Mac run
 * without the Metal backend still checks it. On a Metal run clip_cases
 * repeats them. Proton never builds this test; its check is
 * d3d11_backend_smoke's cpu_clip_cases. Returns the failures. */
static int clip_cpu_cases(void)
{
    static uint32_t px[W * H];
    int cc, f = 0;

    nv2a_pb_set_backend(nv2a_pb_cpu_backend());
    for (cc = CC_CLIP; cc < CC_COUNT; cc++) {
        if (cc == CC_GROW || cc == CC_GROW2 || cc == CC_DRAW3)
            continue;   /* Metal's seeds; nothing the CPU path alone shows */
        fill(BUF0, PATTERN);
        clip_stream(cc);
        memcpy(px, guest(BUF0), W * H * 4);
        f += !clip_probes(cc, "cpu", px);
    }
    surf_clip(BUF0, 0, 0, W, H);
    nv2a_pb_set_backend(NULL);
    return f;
}

/* Texture address mode 4, border (Burnout 3 sets it on stages 0-3 with the
 * border colour as the source, colour 0x00000000). The checker on a quad
 * whose UVs run from -0.5 to 1.5: the taps past the texture take
 * SET_TEXTURE_BORDER_COLOR, the middle the checker. The quad sits 1/32
 * pixel off the grid so no pixel centre lands on a texel edge. With the
 * texture as the border source (FORMAT bit 3 clear) the edge texel stands
 * in, so the corner is the checker's green column. Metal has three sampler
 * border colours (transparent black, opaque black and white) and clamps to
 * the edge for any other, so the arbitrary colour is checked on the CPU
 * path alone; D3D11 takes any colour (d3d11_backend_smoke). */
enum { BC_ZERO, BC_WHITE, BC_ODD, BC_TEXSRC, BC_COUNT };
static const uint32_t s_bc_colour[BC_COUNT] = {0x00000000u, 0xFFFFFFFFu, 0xFF20C040u,
                                               0xFFFFFFFFu};
static const char *const s_bc_name[BC_COUNT] = {"border 0", "border white", "border odd",
                                                "border tex"};

static void border_stream(int bc)
{
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    static const float uv[4][2] = {{-0.5f, -0.5f}, {1.5f, -0.5f}, {1.5f, 1.5f}, {-0.5f, 1.5f}};
    const float o = 1.0f / 32.0f;
    int a;

    surf_clip(BUF0, 0, 0, W, H);
    clear_to(0xFF404040u);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
    stage0_checker();
    M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24) | (1u << 4) | (2u << 0)
              | (bc == BC_TEXSRC ? 0u : 1u << 3));   /* BORDER_SOURCE: colour */
    M(0x1B08, 0x00000404u);                /* border u, border v */
    M(0x1B24, s_bc_colour[bc]);            /* TEXTURE_BORDER_COLOR(0) */
    quad_uv(100.0f + o, 100.0f + o, 300.0f + o, 300.0f + o, white, uv);
    M(0x1B08, 0x00000101u);
    M(0x1B24, 0);
    M(0x1B0C, 0);
    M(0x1760 + 4 * 9, 0x02u);
}

static int border_probes(int bc, const char *path, const uint32_t *px)
{
    /* Left, right, above and below the texture, then its middle texel
     * (32,32), orange. A transparent border writes alpha 0 (blend off), so
     * BC_ZERO checks all 32 bits: compare() reads B/G/R only, and opaque
     * black would pass for it. */
    static const uint32_t at[5][2] = {{120, 200}, {290, 200}, {200, 120}, {200, 290},
                                      {200, 200}};
    uint32_t opaque = bc == BC_ZERO ? 0 : 0xFF000000u;
    uint32_t want = (bc == BC_TEXSRC ? 0xFF00FF00u : s_bc_colour[bc]) | opaque;
    int k, ok = 1;
    for (k = 0; k < 5; k++) {
        uint32_t x = at[k][0], y = at[k][1];
        uint32_t w = k == 4 ? 0xFFFF8000u : want;
        uint32_t got = px[y * W + x] | (k == 4 ? 0xFF000000u : opaque);
        if (bc == BC_TEXSRC && k != 0 && k != 2 && k != 4)
            continue;   /* the edge texels there are the checker's own */
        if (bc == BC_TEXSRC && k == 2)
            w = 0xFFFFFFFFu;   /* above: the white top row */
        fprintf(stderr, "[backend-smoke] %-12s %s pixel (%u,%u) 0x%08X want 0x%08X %s\n",
                s_bc_name[bc], path, x, y, got, w, got == w ? "PASS" : "FAIL");
        ok &= got == w;
    }
    return ok;
}

/* The CPU path for every case and, with Metal, the frames compared whole
 * for the colours Metal has. Returns the failures. */
static int border_cases(const struct nv2a_pb_backend *b)
{
    static uint32_t cpu[W * H];
#ifdef __APPLE__
    static uint32_t gpu[W * H];
#endif
    int bc, f = 0;

    make_textures();
    for (bc = 0; bc < BC_COUNT; bc++) {
        int ok;
        fill(BUF0, PATTERN);
        nv2a_pb_set_backend(nv2a_pb_cpu_backend());
        border_stream(bc);
        memcpy(cpu, guest(BUF0), W * H * 4);
        ok = border_probes(bc, "cpu", cpu);
#ifdef __APPLE__
        if (b && bc != BC_ODD) {
            const uint8_t *hp;
            uint32_t hw = 0, hh = 0;
            fill(BUF0, PATTERN);
            nv2a_pb_set_backend(b);
            border_stream(bc);
            hp = nv2a_pb_metal_host_pixels(BUF0, &hw, &hh);
            if (!hp || hw != W || hh != H) {
                fprintf(stderr, "[backend-smoke] %s: Metal target %ux%u FAIL\n",
                        s_bc_name[bc], hw, hh);
                ok = 0;
            } else {
                memcpy(gpu, hp, W * H * 4);
                ok &= compare(s_bc_name[bc], cpu, gpu);
                ok &= border_probes(bc, "gpu", gpu);
            }
        }
#else
        (void)b;
#endif
        f += !ok;
    }
    surf_clip(BUF0, 0, 0, W, H);
    nv2a_pb_set_backend(NULL);
    return f;
}

#ifdef __APPLE__
/* Each case's stream on the CPU path (guest memory) and on Metal (its host
 * pixels, box-filtered at scale 2; no flip, so nothing is written back),
 * compared whole. The grow cases' guest bytes are the pattern before each
 * run, so Metal's seed reads the pattern, not the CPU frame. Then the crop.
 * Returns the failures. */
static int clip_cases(const struct nv2a_pb_backend *b, uint32_t *cpu, uint32_t *gpu)
{
    uint32_t tw = 0, th = 0, sw = 0, sh = 0;
    int cc, f = 0, k;

    for (cc = CC_CLIP; cc < CC_COUNT; cc++) {
        uint32_t buf = cc == CC_GROW ? BUF3 : cc == CC_GROW2 ? BUF4 : cc == CC_DRAW3 ? BUF5 : BUF0;
        const uint8_t *hp;
        uint32_t hw = 0, hh = 0;
        /* render.scale 3 puts the draw clip's scissor on host pixels 300..780 */
        unsigned n = cc == CC_GROW2 ? 2u : cc == CC_DRAW3 ? 3u : 1u;
        int ok;

        fill(buf, PATTERN);
        nv2a_pb_set_backend(nv2a_pb_cpu_backend());
        clip_stream(cc);
        memcpy(cpu, guest(buf), W * H * 4);
        fill(buf, PATTERN);
        nv2a_pb_set_backend(b);
        if (n > 1) {
            struct nv2a_host_opts o;
            memset(&o, 0, sizeof o);
            o.render_scale = n;
            nv2a_host_opts_set(&o);
        }
        clip_stream(cc);
        if (n > 1) {
            struct nv2a_host_opts o;
            memset(&o, 0, sizeof o);
            nv2a_host_opts_set(&o);
        }
        hp = nv2a_pb_metal_host_pixels(buf, &hw, &hh);
        ok = hp && hw == W * n && hh == H * n;
        if (ok && n == 1)
            memcpy(gpu, hp, W * H * 4);
        else if (ok)
            nv2a_downscale_box32(hp, hw * 4u, W, H, n, (uint8_t *)gpu, W * 4u);
        if (!ok) {
            fprintf(stderr, "[backend-smoke] %s: Metal target %ux%u (want %ux%u) FAIL\n",
                    s_clip_name[cc], hw, hh, W * n, H * n);
            f++;
            continue;
        }
        ok = compare(s_clip_name[cc], cpu, gpu);
        ok &= clip_probes(cc, "cpu", cpu);
        ok &= clip_probes(cc, "gpu", gpu);
        if (!nv2a_pb_metal_shown(buf, &tw, &th, &sw, &sh) || tw != W || th != H) {
            fprintf(stderr, "[backend-smoke] %s: one target of %ux%u (want %ux%u) FAIL\n",
                    s_clip_name[cc], tw, th, W, H);
            ok = 0;
        }
        f += !ok;
    }

    /* The frame is 320x240 of BUF0's 640x480 target: the flip shows
     * 320x240. The walker presents the largest fresh surface, so the 640x480
     * entries must first go stale: more than PRESENT_STALE_FLIPS (120)
     * flips drawing only the 320x240 clip. The walker counts a flip at
     * FLIP_INCREMENT_WRITE, as the XDK's swap pushes it, not at FLIP_STALL
     * alone. Metal first (the CPU path's clears would rewrite BUF0 and make
     * Metal's target stale), then the CPU path, whose present state is the
     * cross-check. */
    for (k = 0; k < 2; k++) {
        const struct nv2a_pb_present *p;
        int i;
        nv2a_pb_set_backend(k ? nv2a_pb_cpu_backend() : b);
        surf_clip(BUF0, 0, 0, 320, 240);
        for (i = 0; i < 122; i++) {
            clear_to(0xFF000000u);
            M(0x012C, 0);                       /* FLIP_INCREMENT_WRITE */
            M(0x0130, 0);                       /* FLIP_STALL */
        }
        p = nv2a_pb_present_state();
        if (k) {
            int ok = p->offset == BUF0 && p->w == 320 && p->h == 240;
            fprintf(stderr, "[backend-smoke] crop: cpu present 0x%08X %ux%u (want 0x%08X"
                    " 320x240) %s\n", p->offset, p->w, p->h, BUF0, ok ? "PASS" : "FAIL");
            f += !ok;
        } else if (!nv2a_pb_metal_shown(BUF0, &tw, &th, &sw, &sh) || tw != W || th != H
                   || sw != 320 || sh != 240) {
            fprintf(stderr, "[backend-smoke] crop: target %ux%u, shown %ux%u (want 640x480,"
                    " 320x240) FAIL\n", tw, th, sw, sh);
            f++;
        } else {
            fprintf(stderr, "[backend-smoke] crop: target 640x480, shown 320x240 PASS\n");
        }
    }
    surf_clip(BUF0, 0, 0, W, H);
    nv2a_pb_set_backend(NULL);
    return f;
}

/* A visibility test spanning a partial clear: two 100x100 quads counted,
 * a 50x50 clip cleared between them. Metal sets a visibility offset once per
 * pass, so the clear ends the counting pass and the second quad counts in a
 * new one (metal_on_clear); the clear itself is not a sample. Metal must
 * count the 20000 pixels the quads cover. The CPU walker's count is printed,
 * not checked: it counts 101 rows a quad (20200), a fidelity gap of its own
 * (TASKS: occlusion count semantics). Returns the failures. */
static int occ_case(const struct nv2a_pb_backend *b)
{
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    volatile uint32_t *r = (volatile uint32_t *)guest(REPORT);
    uint32_t count[2] = {0, 0};
    int k, a, f = 0;

    for (k = 0; k < 2; k++) {
        const struct nv2a_pb_backend *cur = k ? b : nv2a_pb_cpu_backend();
        nv2a_pb_set_backend(cur);
        surf_clip(BUF0, 0, 0, W, H);
        clear_to(0xFF000000u);
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        r[3] = 0xFFFFFFFFu;
        M(0x17C8, 1);                          /* CLEAR_REPORT_VALUE */
        M(0x17CC, 1);                          /* ZPASS_PIXEL_COUNT_ENABLE */
        quad_uv(100.0f, 100.0f, 200.0f, 200.0f, white, NULL);
        surf_clip(BUF0, 300, 300, 50, 50);
        clear_to(0xFF0000FFu);
        surf_clip(BUF0, 0, 0, W, H);
        quad_uv(400.0f, 100.0f, 500.0f, 200.0f, white, NULL);
        M(0x17CC, 0);
        M(0x17D0, (1u << 24) | REPORT);        /* GET_REPORT */
        M(0x0210, BUF1);
        M(0x0130, 0);                          /* FLIP_STALL: the GPU done */
        for (a = 0; a < 2000 && r[3]; a++) {   /* at most 2 s */
            if (cur->on_poll)
                cur->on_poll();
            if (r[3])
                usleep(1000);
        }
        count[k] = r[3] ? 0xFFFFFFFFu : r[2];
    }
    nv2a_pb_set_backend(NULL);
    f = count[1] != 20000u;
    fprintf(stderr, "[backend-smoke] occlusion across a partial clear: gpu %u (want 20000)"
            " %s; cpu %u\n", count[1], f ? "FAIL" : "PASS", count[0]);
    return f;
}

/* metal_fb_guard: the surface is PROT_NONE after a lazy flip; a read faults,
 * the next flip reports it and switches to always, and the flip after that
 * writes back. Returns the failures. */
static int guard_case(const struct nv2a_pb_backend *b, const uint32_t *ref)
{
    volatile uint32_t *g = (volatile uint32_t *)guest(BUF0);
    uint32_t i, v;
    int f = 0;

    nv2a_pb_set_backend(b);
    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    scene(SC_CLEAR_QUAD, BUF0, BUF1);   /* armed after this flip */
    v = g[0];                           /* the title read: faults once */
    f += check(v == 0xFFFF00FFu, "fb_guard: the read proceeds (stale seed)");
    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    scene(SC_CLEAR_QUAD, BUF0, BUF1);   /* reports the read, switches */
    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    scene(SC_CLEAR_QUAD, BUF0, BUF1);   /* written back at the flip */
    f += check(!memcmp((const void *)g, ref, W * H * 4),
               "fb_guard: after a read, the CPU frame at the flip");
    nv2a_pb_set_backend(NULL);
    return f;
}
#endif

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

/* The index limit, on the CPU path: every index is vertex 0, so the
 * 0x80000-index batches are degenerate triangles and rasterise nothing.
 * Returns the failures. */
static int overflow_case(void)
{
    uint32_t max0, ov0, ff0, max, ov, ff, i, n;
    int f = 0;

    nv2a_pb_set_backend(nv2a_pb_cpu_backend());
    vb_vertex(0, 10.0f, 10.0f, 0xFFFFFFFFu);
    vb_arrays();
    nv2a_pb_exec_idx_stats(&max0, &ov0, &ff0);

    /* One word past the cap: NV_MAX_INDICES / 2 + 1 ARRAY_ELEMENT16 words. */
    M(0x17FC, 5);                          /* TRIANGLES */
    for (i = 0; i < NV_MAX_INDICES / 2u + 1u; i++)
        M(0x1800, 0);
    n = nv2a_pb_gpu_state()->idx_count;
    M(0x17FC, 0);
    nv2a_pb_exec_idx_stats(&max, &ov, &ff);
    fprintf(stderr, "[backend-smoke] overflow: batch past the cap keeps %u, max %u,"
            " %u overflowed (want %u, %u, %u) %s\n", n, max, ov - ov0,
            (unsigned)NV_MAX_INDICES, (unsigned)NV_MAX_INDICES, 1u,
            n == NV_MAX_INDICES && max == NV_MAX_INDICES && ov - ov0 == 1u ? "PASS" : "FAIL");
    f += !(n == NV_MAX_INDICES && max == NV_MAX_INDICES && ov - ov0 == 1u);

    /* One slot left: an odd count from ARRAY_ELEMENT32, then 16-bit pairs. */
    M(0x17FC, 5);
    M(0x1808, 0);
    for (i = 0; i < NV_MAX_INDICES / 2u; i++)
        M(0x1800, 0);
    n = nv2a_pb_gpu_state()->idx_count;
    M(0x17FC, 0);
    nv2a_pb_exec_idx_stats(NULL, &ov, NULL);
    fprintf(stderr, "[backend-smoke] overflow: one slot left keeps the first half: %u"
            " indices, %u overflowed (want %u, %u) %s\n", n, ov - ov0,
            (unsigned)NV_MAX_INDICES, 2u,
            n == NV_MAX_INDICES && ov - ov0 == 2u ? "PASS" : "FAIL");
    f += !(n == NV_MAX_INDICES && ov - ov0 == 2u);

    /* A 32-bit index past 0xFFFF: counted, the batch not overflowed. */
    M(0x17FC, 5);
    M(0x1800, 0);
    M(0x1808, 0x00010000u);
    M(0x17FC, 0);
    nv2a_pb_exec_idx_stats(NULL, &ov, &ff);
    fprintf(stderr, "[backend-smoke] overflow: ARRAY_ELEMENT32 0x10000 counts %u over"
            " 0xFFFF, %u overflowed (want 1, 2) %s\n", ff - ff0, ov - ov0,
            ff - ff0 == 1u && ov - ov0 == 2u ? "PASS" : "FAIL");
    f += !(ff - ff0 == 1u && ov - ov0 == 2u);

    {
        int a;
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
    }
    return f;
}

/* ---- scanout set (present-scanout) ------------------------------------------ */

/* The present pick's size scan and eviction with a display mode set, on the
 * CPU path: Burnout 3's race-start card, reduced to what the surface table
 * sees. The display stays at SC_A (AvSetDisplayMode's address) while the
 * title flips between SC_A and SC_B; each frame records its back buffer at
 * three clips (640x480 composite, 640x464 scene, 640x463 card text) and
 * clears 19 offscreen targets, one 640x480 at the display pitch. Run last:
 * the cases set a display mode, which every case before runs without, and
 * leave the table full of their entries. The first flip after a mode set
 * keeps the screen (no surface since the set has had the display pitch yet,
 * the field rule), so each case flips once before it checks. */
#define SC_A     0x03000000u
#define SC_B     0x0312C000u
#define SC_C     0x03258000u
#define SC_OFF   0x03384000u    /* the 640x480 offscreen target */
#define SC_SMALL 0x034B0000u    /* 128x128 targets at pitch 512, 64 KB apart */
#define SC_D     0x038C0000u    /* cases 3 and 4: a display SC_A's leftovers miss */
#define SC_PITCH (W * 4u)

void nv2a_pb_exec_present_stats(uint32_t *evictions, uint32_t *spared, uint32_t *protected_);

static void sc_surf(uint32_t buf, uint32_t w, uint32_t h, uint32_t pitch)
{
    M(0x0200, w << 16);
    M(0x0204, h << 16);
    M(0x0208, 0x00000128u);
    M(0x020C, pitch);
    M(0x0210, buf);
    M(0x0214, 0x02400000u);
}

/* A draw: only the clip extent matters to the table, so a 2x2 quad. */
static void sc_draw(uint32_t buf, uint32_t w, uint32_t h)
{
    static const uint32_t c[4] = {0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u};
    sc_surf(buf, w, h, SC_PITCH);
    quad_uv(0.0f, 0.0f, 2.0f, 2.0f, c, NULL);
}

/* The offscreen targets: SC_OFF at 640x480 and the display pitch, then
 * n - 1 128x128 ones, each cleared once. */
static void sc_offscreen(int n)
{
    int t;
    sc_surf(SC_OFF, W, H, SC_PITCH);
    clear_to(0xFF000000u);
    for (t = 0; t < n - 1; t++) {
        sc_surf(SC_SMALL + (uint32_t)t * 0x10000u, 128, 128, 512);
        clear_to(0xFF000000u);
    }
}

/* The XDK's swap: the colour offset moves to the next back buffer, then
 * FLIP_INCREMENT_WRITE (the walker's flip count) and FLIP_STALL (the pick). */
static void sc_flip(uint32_t next)
{
    M(0x0210, next);
    M(0x012C, 0);
    M(0x0130, 0);
}

static int sc_check(const char *what, int flip, uint32_t off, uint32_t w, uint32_t h)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int ok = p->offset == off && p->w == w && p->h == h && p->pitch == SC_PITCH;
    if (!ok)
        fprintf(stderr, "[backend-smoke] scanout: %s flip %d presents 0x%08X %ux%u pitch %u"
                " (want 0x%08X %ux%u pitch %u)\n", what, flip, p->offset, p->w, p->h,
                p->pitch, off, w, h, SC_PITCH);
    return ok;
}

static void sc_display(uint32_t buf)
{
    /* The address the AvSetDisplayMode bridge records: the resolved form. */
    xbox_SetDisplayFramebuffer(buf ? 0x80000000u | buf : 0, buf ? SC_PITCH : 0);
    xbox_DisplayModeBump();
}

/* Returns the failures. */
static int scanout_cases(void)
{
    static const uint32_t back[2] = {SC_A, SC_B};
    uint32_t ev0, sp0, pr0, ev, sp, pr;
    int k, a, ok, f = 0;

    nv2a_pb_set_backend(nv2a_pb_cpu_backend());
    M(0x0304, 0);
    M(0x1B0C, 0);
    clear_rect(0, 0, 4095, 4095);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);

    /* 1. Double-buffered card: 2 x 3 + 19 = 25 entries, over the old table's
     * 16. Before the scanout set this stream holds 640x464 on every flip;
     * Burnout 3's own frame (13 targets) alternated 480/464 instead, which
     * this case does not reproduce: either way the size is not 640x480. */
    sc_display(SC_A);
    ok = 1;
    for (k = 0; k < 10; k++) {
        uint32_t cur = back[k & 1];
        sc_draw(cur, W, H);
        sc_offscreen(19);
        sc_draw(cur, W, 464);
        sc_draw(cur, W, 463);
        sc_flip(back[(k + 1) & 1]);
        if (k >= 2)
            ok &= sc_check("double-buffered", k + 1, cur, W, H);
    }
    fprintf(stderr, "[backend-smoke] scanout: double-buffered card, 8 flips at 640x480 %s\n",
            ok ? "PASS" : "FAIL");
    f += !ok;

    /* 1b. The same card with 40 offscreen targets, over the new table's 32:
     * the other back buffer's 640x480 entry survives only as a scanout
     * offset, so this fails when present_scanout ignores the set. The first
     * two frames have 19, as case 1: a buffer never presented is in no set,
     * and with 40 targets a frame its entry is evicted before the flip. */
    sc_display(SC_A);
    ok = 1;
    for (k = 0; k < 10; k++) {
        uint32_t cur = back[k & 1];
        sc_draw(cur, W, H);
        sc_offscreen(k < 2 ? 19 : 40);
        sc_draw(cur, W, 464);
        sc_draw(cur, W, 463);
        sc_flip(back[(k + 1) & 1]);
        if (k >= 2)
            ok &= sc_check("double-buffered x40", k + 1, cur, W, H);
    }
    fprintf(stderr, "[backend-smoke] scanout: double-buffered card, 40 targets, 8 flips at"
            " 640x480 %s\n", ok ? "PASS" : "FAIL");
    f += !ok;

    /* 2. A mode change empties the set: SC_A's larger entry no longer sets
     * the size. Without the clear it would be the pick, at 1280x720. */
    sc_display(SC_C);
    for (k = 0; k < 2; k++) {
        sc_draw(SC_A, 1280, 720);
        sc_draw(SC_C, W, H);
        sc_flip(SC_A);
    }
    ok = sc_check("mode change", 2, SC_C, W, H);
    fprintf(stderr, "[backend-smoke] scanout: mode change clears the set %s\n",
            ok ? "PASS" : "FAIL");
    f += !ok;

    /* 3. Single-buffered with 19 offscreen targets: 21 entries a frame, over
     * the old 16, so the 640x480 entry was the oldest and evicted. */
    sc_display(SC_D);
    ok = 1;
    for (k = 0; k < 9; k++) {
        sc_draw(SC_D, W, H);
        sc_offscreen(19);
        sc_draw(SC_D, W, 464);
        sc_flip(SC_D);
        if (k)
            ok &= sc_check("single-buffered", k + 1, SC_D, W, H);
    }
    fprintf(stderr, "[backend-smoke] scanout: single-buffered, 19 targets, 8 flips at"
            " 640x480 %s\n", ok ? "PASS" : "FAIL");
    f += !ok;

    /* 3b. The same with 40 offscreen targets, over the new table's 32: the
     * protection, not the table size, keeps the 640x480 entry. */
    nv2a_pb_exec_present_stats(&ev0, &sp0, NULL);
    ok = 1;
    for (k = 0; k < 8; k++) {
        sc_draw(SC_D, W, H);
        sc_offscreen(40);
        sc_draw(SC_D, W, 464);
        sc_flip(SC_D);
        ok &= sc_check("single-buffered x40", k + 1, SC_D, W, H);
    }
    nv2a_pb_exec_present_stats(&ev, &sp, NULL);
    ok &= ev > ev0 && sp > sp0;
    fprintf(stderr, "[backend-smoke] scanout: single-buffered, 40 targets, 8 flips at"
            " 640x480 (%u evictions, %u spared a scanout entry) %s\n", ev - ev0, sp - sp0,
            ok ? "PASS" : "FAIL");
    f += !ok;

    /* 3c. 3b right after a mode set. The first pick after it wants the field
     * pitch (no surface has had the display's yet); the display buffer's
     * entries at the display pitch must stay protected through the next
     * frame, or 40 targets evict its 640x480 entry and flip 2 shows 464. */
    sc_display(SC_D);
    ok = 1;
    for (k = 0; k < 8; k++) {
        sc_draw(SC_D, W, H);
        sc_offscreen(40);
        sc_draw(SC_D, W, 464);
        sc_flip(SC_D);
        if (k)
            ok &= sc_check("mode set x40", k + 1, SC_D, W, H);
    }
    fprintf(stderr, "[backend-smoke] scanout: mode set, then 40 targets, flips 2-8 at"
            " 640x480 %s\n", ok ? "PASS" : "FAIL");
    f += !ok;

    /* 4. Every entry protected: 40 clips at SC_D, then 640x480. Plain LRU
     * takes over; the newest entries survive and nothing stalls. */
    nv2a_pb_exec_present_stats(NULL, NULL, &pr0);
    for (k = 0; k < 40; k++)
        sc_draw(SC_D, W, 100u + (uint32_t)k);
    sc_draw(SC_D, W, H);
    sc_flip(SC_D);
    nv2a_pb_exec_present_stats(NULL, NULL, &pr);
    ok = sc_check("all protected", 1, SC_D, W, H) && pr > pr0;
    fprintf(stderr, "[backend-smoke] scanout: table of protected entries still evicts"
            " (%u took a protected entry) %s\n", pr - pr0, ok ? "PASS" : "FAIL");
    f += !ok;

    /* 5. Ageing out of the set. SC_C is presented once (the newest draw of
     * the size SC_D sets), then only SC_D for n flips; then SC_C is drawn
     * larger. An offset presented present_stale (120) flips ago is still in
     * the set and its larger entry sets the size; one more flip and it is
     * not, so SC_D's frame shows. Pins scanout_live's <= from both sides. */
    for (a = 0; a < 2; a++) {
        uint32_t n = 119u + (uint32_t)a;     /* present_stale is the default 120 */
        sc_display(SC_D);
        sc_draw(SC_D, W, H);
        sc_flip(SC_D);                          /* the field latch holds */
        sc_draw(SC_D, W, H);
        sc_draw(SC_C, W, H);
        sc_flip(SC_D);
        ok = sc_check("ageing, SC_C presented", 2, SC_C, W, H);
        for (k = 0; k < (int)n; k++) {
            sc_draw(SC_D, W, H);
            sc_flip(SC_D);
        }
        sc_draw(SC_D, W, H);
        sc_draw(SC_C, 1280, 720);
        sc_flip(SC_D);
        ok &= a ? sc_check("ageing, aged out", (int)n + 3, SC_D, W, H)
                : sc_check("ageing, still live", (int)n + 3, SC_C, 1280, 720);
        fprintf(stderr, "[backend-smoke] scanout: SC_C presented %u flips ago %s the size"
                " %s\n", n + 1u, a ? "no longer sets" : "still sets", ok ? "PASS" : "FAIL");
        f += !ok;
    }

    sc_display(0);
    nv2a_pb_set_backend(NULL);
    return f;
}

/* The mode-height cases: with a known display mode the pick publishes the
 * mode's frame, not the clip (rules 1-3 at present_pick). After the scanout
 * cases, on the CPU path, with Metal where it builds for the two that show a
 * target; each ends with no display mode. As there, the first flip after a
 * mode set holds the screen (the field latch), so a case flips once first. */
#define MH_A    0x03A00000u     /* case 2: a buffer only ever drawn at 320x240 */
#define MH_F    0x03B40000u     /* case 3: the field title's buffer */
#define MH_U    0x03C80000u     /* case 5: the unknown word's buffer */
#define MH_P0   0x03D00000u     /* case 6: one buffer pair */
#define MH_P1   0x03E40000u
#define MH_C4   0x03F00000u     /* case 4: the 480-line mode's buffer */
#define MH_HD   0x03F80000u     /* and the 720p one (only 2x2 quads land there) */
#define MH_N    0x02C00000u     /* case 7: a display buffer nothing draws */
#define MH_O    0x02E00000u     /* and a 320x240 offscreen at the display pitch */
#define MH_480  0x0801010Du     /* NTSC-M 480i, 640x480 */
#define MH_720  0x880B0A02u     /* 720p, 1280x720 */

/* What the AvSetDisplayMode bridge records, A8R8G8B8. */
static void mh_mode(uint32_t buf, uint32_t pitch, uint32_t mode)
{
    xbox_SetDisplayMode(0x80000000u | buf, pitch, mode, 0x12);
    xbox_DisplayModeBump();
}

static void mh_draw(uint32_t buf, uint32_t w, uint32_t h, uint32_t pitch)
{
    static const uint32_t c[4] = {0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u};
    sc_surf(buf, w, h, pitch);
    quad_uv(0.0f, 0.0f, 2.0f, 2.0f, c, NULL);
}

static int mh_is(const char *what, int flip, uint32_t off, uint32_t w, uint32_t h,
                 uint32_t pitch)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int ok = p->offset == off && p->w == w && p->h == h && p->pitch == pitch;
    if (!ok)
        fprintf(stderr, "[backend-smoke] mode-height: %s flip %d presents 0x%08X %ux%u pitch"
                " %u (want 0x%08X %ux%u pitch %u)\n", what, flip, p->offset, p->w, p->h,
                p->pitch, off, w, h, pitch);
    return ok;
}

/* stderr into a temporary file, for counting a warning's lines: begin
 * returns the saved descriptor, end restores it and counts `what`. */
static int mh_capture_begin(FILE **tmp)
{
    int saved;

    *tmp = tmpfile();
    if (!*tmp)
        return -1;
    fflush(stderr);
    saved = dup(2);
    dup2(fileno(*tmp), 2);
    return saved;
}

static int mh_capture_end(FILE *tmp, int saved, const char *what)
{
    char line[512];
    int n = 0;

    if (saved < 0)
        return -1;
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(tmp);
    while (fgets(line, sizeof line, tmp)) {
        fputs(line, stderr);             /* the log keeps what the case printed */
        n += strstr(line, what) != NULL;
    }
    fclose(tmp);
    return n;
}

static uint32_t mh_px(int gpu, uint32_t buf, uint32_t x, uint32_t y)
{
#ifdef __APPLE__
    if (gpu) {
        uint32_t hw = 0, hh = 0;
        const uint8_t *hp = nv2a_pb_metal_host_pixels(buf, &hw, &hh);
        return hp && x < hw && y < hh ? ((const uint32_t *)hp)[y * hw + x] : 0;
    }
#endif
    (void)gpu;
    return ((const uint32_t *)guest(buf))[y * W + x];
}

/* Returns the failures. gpu is Metal, or NULL for the CPU path alone. */
static int mode_height_cases(const struct nv2a_pb_backend *gpu)
{
    static const uint32_t pair[2] = {MH_P0, MH_P1};
    uint32_t tw = 0, th = 0, sw = 0, sh = 0, lw = 0, lh = 0;
    int k, b, ok, changes, f = 0;

    (void)tw; (void)th; (void)sw; (void)sh;
    M(0x0304, 0);
    M(0x1B0C, 0);
    clear_rect(0, 0, 4095, 4095);

    /* 1. Rule 1 beside the crop: the crop case's stream (122 flips of a
     * 320x240 clip at BUF0) with a 640x480 mode set at BUF0 shows the mode's
     * 640x480 on both paths. Metal first, as there. */
    for (b = gpu ? 0 : 1; b < 2; b++) {
        int i;
        nv2a_pb_set_backend(b ? nv2a_pb_cpu_backend() : gpu);
        mh_mode(BUF0, W * 4u, MH_480);
        surf_clip(BUF0, 0, 0, W, H);
        clear_to(0xFF000000u);
        surf_clip(BUF0, 0, 0, 320, 240);
        for (i = 0; i < 122; i++) {
            clear_to(0xFF000000u);
            M(0x012C, 0);
            M(0x0130, 0);
        }
        if (b) {
            ok = mh_is("crop with a mode", 122, BUF0, W, H, W * 4u);
            fprintf(stderr, "[backend-smoke] mode-height: crop with a 640x480 mode, cpu"
                    " presents 640x480 %s\n", ok ? "PASS" : "FAIL");
        }
#ifdef __APPLE__
        else {
            ok = nv2a_pb_metal_shown(BUF0, &tw, &th, &sw, &sh) && tw == W && th == H
                && sw == W && sh == H;
            fprintf(stderr, "[backend-smoke] mode-height: crop with a 640x480 mode, metal"
                    " target %ux%u shown %ux%u (want 640x480, 640x480) %s\n", tw, th, sw, sh,
                    ok ? "PASS" : "FAIL");
        }
#endif
        f += !ok;
        surf_clip(BUF0, 0, 0, W, H);
        sc_display(0);
    }

    /* 2. Grow at present: a buffer only ever drawn with a 320x240 clip
     * shows the mode's 640x480. Metal grows its 320x240 target to the pick
     * (seeded from the guest bytes, the pattern) instead of clamping: the
     * cleared 320x240 at the top left, the pattern beyond, on both paths. */
    for (b = gpu ? 0 : 1; b < 2; b++) {
        uint32_t in, out;
        nv2a_pb_set_backend(b ? nv2a_pb_cpu_backend() : gpu);
        fill(MH_A, PATTERN);
        mh_mode(MH_A, W * 4u, MH_480);
        for (k = 0; k < 2; k++) {
            surf_clip(MH_A, 0, 0, 320, 240);
            clear_to(0xFFFF0000u);
            M(0x012C, 0);
            M(0x0130, 0);
        }
        ok = b ? mh_is("grow", 2, MH_A, W, H, W * 4u) : 1;
#ifdef __APPLE__
        if (!b)
            ok = nv2a_pb_metal_shown(MH_A, &tw, &th, &sw, &sh) && tw == W && th == H
                && sw == W && sh == H;
#endif
        in = mh_px(!b, MH_A, 10, 10) & 0x00FFFFFFu;
        out = mh_px(!b, MH_A, 400, 300) & 0x00FFFFFFu;
        ok &= in == 0xFF0000u && out == (PATTERN & 0x00FFFFFFu);
        if (b)
            fprintf(stderr, "[backend-smoke] mode-height: 320x240 clip under a 640x480 mode,"
                    " cpu presents 640x480, (10,10) %06X (400,300) %06X %s\n", in, out,
                    ok ? "PASS" : "FAIL");
        else
            fprintf(stderr, "[backend-smoke] mode-height: 320x240 clip under a 640x480 mode,"
                    " metal target %ux%u shown %ux%u, (10,10) %06X (400,300) %06X %s\n",
                    tw, th, sw, sh, in, out, ok ? "PASS" : "FAIL");
        f += !ok;
        sc_display(0);
    }

    nv2a_pb_set_backend(nv2a_pb_cpu_backend());

    /* 3. Field title: the mode at pitch 2560, every frame drawn with a
     * 640x232 clip at twice it. One field at the mode's half height, not the
     * clip (rule 2), with the field warning once; then a frame at the display
     * pitch is the mode's 640x480 (rule 1). The first case to set a field
     * pitch, so the warning has not printed yet. */
    {
        FILE *tmp = NULL;
        int saved = mh_capture_begin(&tmp), warned;
        mh_mode(MH_F, W * 4u, MH_480);
        ok = 1;
        for (k = 0; k < 4; k++) {
            mh_draw(MH_F, W, 232, W * 8u);
            sc_flip(MH_F);
            ok &= mh_is("field", k + 1, MH_F, W, 240, W * 8u);
        }
        mh_draw(MH_F, W, H, W * 4u);
        sc_flip(MH_F);
        ok &= mh_is("field, then frame", 5, MH_F, W, H, W * 4u);
        warned = mh_capture_end(tmp, saved, "(field rendering?)");
        ok &= warned == 1;
        fprintf(stderr, "[backend-smoke] mode-height: field title drawn at 640x232 presents"
                " 640x240 at pitch 5120, then 640x480; field warning %d time(s) (want 1)"
                " %s\n", warned, ok ? "PASS" : "FAIL");
    }
    f += !ok;
    sc_display(0);

    /* 4. Mode change: 480 lines at MH_C4, then 720p at a new address and
     * twice the pitch, then back. Each new mode's frame from its second flip
     * (the first holds the old one, not a new pick of an old buffer). */
    mh_mode(MH_C4, W * 4u, MH_480);
    for (k = 0; k < 2; k++) {
        mh_draw(MH_C4, W, H, W * 4u);
        sc_flip(MH_C4);
    }
    ok = mh_is("480 before 720p", 2, MH_C4, W, H, W * 4u);
    mh_mode(MH_HD, 5120u, MH_720);
    for (k = 0; k < 3; k++) {
        mh_draw(MH_HD, 1280, 720, 5120u);
        sc_flip(MH_HD);
        ok &= k ? mh_is("720p", k + 1, MH_HD, 1280, 720, 5120u)
                : mh_is("720p, held", 1, MH_C4, W, H, W * 4u);
    }
    mh_mode(MH_C4, W * 4u, MH_480);
    for (k = 0; k < 3; k++) {
        mh_draw(MH_C4, W, H, W * 4u);
        sc_flip(MH_C4);
        ok &= k ? mh_is("back to 480", k + 1, MH_C4, W, H, W * 4u)
                : mh_is("back to 480, held", 1, MH_HD, 1280, 720, 5120u);
    }
    fprintf(stderr, "[backend-smoke] mode-height: 640x480, 720p at 1280x720, 640x480 again %s\n",
            ok ? "PASS" : "FAIL");
    f += !ok;
    sc_display(0);

    /* 5. An unknown word (timing byte 0x7F) records no size: the clip
     * extent, as before this change (rule 3). */
    mh_mode(MH_U, W * 4u, (MH_480 & ~0xFF00u) | 0x7F00u);
    for (k = 0; k < 2; k++) {
        mh_draw(MH_U, 320, 240, W * 4u);
        sc_flip(MH_U);
    }
    ok = mh_is("unknown word", 2, MH_U, 320, 240, W * 4u);
    fprintf(stderr, "[backend-smoke] mode-height: unknown mode word keeps the 320x240 clip %s\n",
            ok ? "PASS" : "FAIL");
    f += !ok;
    sc_display(0);

    /* 6. Burnout 3's pattern: one buffer pair, 20 frames with the clip at
     * 640x464 (its menus), then 20 at 640x480 and 640x463 (the race and its
     * card). Every flip after the first presents 640x480: no size change at
     * the menu-to-race step. */
    mh_mode(MH_P0, W * 4u, 0x080F0D12u);   /* 480p, 640x480 */
    ok = 1;
    changes = 0;
    for (k = 0; k < 40; k++) {
        const struct nv2a_pb_present *p;
        uint32_t cur = pair[k & 1];
        if (k < 20) {
            mh_draw(cur, W, 464, W * 4u);
        } else {
            mh_draw(cur, W, H, W * 4u);
            mh_draw(cur, W, 463, W * 4u);
        }
        sc_flip(pair[(k + 1) & 1]);
        if (!k)
            continue;
        ok &= mh_is("menus to race", k + 1, cur, W, H, W * 4u);
        p = nv2a_pb_present_state();
        if (k > 1 && (p->w != lw || p->h != lh))
            changes++;
        lw = p->w;
        lh = p->h;
    }
    ok &= !changes;
    fprintf(stderr, "[backend-smoke] mode-height: menus at 640x464 then race, 39 flips at"
            " 640x480, %d size changes %s\n", changes, ok ? "PASS" : "FAIL");
    f += !ok;
    sc_display(0);

    /* 7. A pass-1 pick keeps its clip (rule 3): the mode set at a buffer
     * nothing draws, and only a 320x240 offscreen at the display pitch drawn,
     * for more than present_stale flips so every earlier surface is stale.
     * The offscreen is the pick, but not at the display address, so the
     * mode's 640x480 is not its frame: Metal must not grow its target (the
     * grown target would own, and write back over, memory past the buffer). */
    for (b = gpu ? 0 : 1; b < 2; b++) {
        int i;
        nv2a_pb_set_backend(b ? nv2a_pb_cpu_backend() : gpu);
        mh_mode(MH_N, W * 4u, MH_480);
        surf_clip(MH_O, 0, 0, 320, 240);
        for (i = 0; i < 122; i++) {
            clear_to(0xFF0000FFu);
            M(0x012C, 0);
            M(0x0130, 0);
        }
        ok = mh_is("pass-1 pick", 122, MH_O, 320, 240, W * 4u);
#ifdef __APPLE__
        if (!b)
            ok &= nv2a_pb_metal_shown(MH_O, &tw, &th, &sw, &sh) && tw == 320 && th == 240
                && sw == 320 && sh == 240;
#endif
        if (b)
            fprintf(stderr, "[backend-smoke] mode-height: offscreen 320x240 with the mode at an"
                    " undrawn buffer, cpu presents %ux%u (want 320x240) %s\n",
                    nv2a_pb_present_state()->w, nv2a_pb_present_state()->h,
                    ok ? "PASS" : "FAIL");
        else
            fprintf(stderr, "[backend-smoke] mode-height: offscreen 320x240 with the mode at an"
                    " undrawn buffer, metal target %ux%u shown %ux%u (want 320x240, 320x240)"
                    " %s\n", tw, th, sw, sh, ok ? "PASS" : "FAIL");
        f += !ok;
        surf_clip(MH_O, 0, 0, W, H);
        sc_display(0);
    }

    nv2a_pb_set_backend(NULL);
    return f;
}

/* The absolute checks of bigidx and elem32, on one path's frame. */
static int probes_ok(int sc, const char *path, const uint32_t *px)
{
    const struct probe *p = sc == SC_BIGIDX ? s_probe_bigidx : s_probe_elem32;
    uint32_t n = sc == SC_BIGIDX ? sizeof s_probe_bigidx / sizeof *p
                                 : sizeof s_probe_elem32 / sizeof *p, i;
    int ok = 1;

    if (sc != SC_BIGIDX && sc != SC_ELEM32)
        return 1;
    for (i = 0; i < n; i++) {
        uint32_t got = px[p[i].y * W + p[i].x] & 0x00FFFFFFu;
        int hit = got == (p[i].argb & 0x00FFFFFFu);
        fprintf(stderr, "[backend-smoke] %-10s %s pixel (%u,%u) %06X want %06X %s\n",
                s_scene_name[sc], path, p[i].x, p[i].y, got, p[i].argb & 0x00FFFFFFu,
                hit ? "PASS" : "FAIL");
        ok &= hit;
    }
    return ok;
}

int main(int argc, char **argv)
{
    static uint8_t xbe[0x1000];
    const char *which = getenv("RECOMP_SMOKE_BACKEND");
    const char *dump = getenv("RECOMP_SMOKE_DUMP");
    const struct nv2a_pb_backend *gpu = NULL;
    uint32_t *cpu_px, *gpu_px;
    int sc, k = 0, failures = 0;
    int guard = argc > 1 && !strcmp(argv[1], "guard");
    int noalias = argc > 1 && !strcmp(argv[1], "noalias");

    setvbuf(stderr, NULL, _IONBF, 0);
    if (!which)
        which = "metal";
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[backend-smoke] FAIL: guest memory init\n");
        return 1;
    }
    /* The index limit is the walker's alone (CPU path), so it runs before
     * the backend check: a host without a GPU backend still checks it. */
    if (!guard && !noalias && overflow_case()) {
        fprintf(stderr, "[backend-smoke] FAIL: index limit\n");
        failures++;
    }
    if (!guard && !noalias && snap_case()) {
        fprintf(stderr, "[backend-smoke] FAIL: screen snap\n");
        failures++;
    }
    if (!guard && !noalias)
        failures += edge_case(nv2a_pb_cpu_backend(), "cpu");
    /* So is the CPU clear box. */
    if (!guard && !noalias && clip_cpu_cases()) {
        fprintf(stderr, "[backend-smoke] FAIL: CPU clip clear or clear rect\n");
        failures++;
    }
#ifdef __APPLE__
    if (!strcasecmp(which, "metal"))
        gpu = nv2a_pb_metal_backend();
#endif
    if (!gpu) {
        if (!guard && !noalias && border_cases(NULL)) {
            fprintf(stderr, "[backend-smoke] FAIL: CPU border colour\n");
            failures++;
        }
        fprintf(stderr, "[backend-smoke] no backend '%s' on this host: GPU scenes"
                " skipped\n", which);
        failures += scanout_cases();
        failures += mode_height_cases(NULL);
        return failures ? 1 : 77;
    }
    cpu_px = (uint32_t *)malloc(W * H * 4);
    gpu_px = (uint32_t *)malloc(W * H * 4);
    if (!cpu_px || !gpu_px)
        return 1;
    make_textures();
#ifdef __APPLE__
    if (guard) {
        recomp_env_set(RENV_METAL_FB_GUARD, "1");
        if (!run(nv2a_pb_cpu_backend(), SC_CLEAR_QUAD, cpu_px, 0))
            return 1;
        failures = guard_case(gpu, cpu_px);
        fprintf(stderr, "[backend-smoke] %s: fb_guard\n", failures ? "FAIL" : "done");
        return failures ? 1 : 0;
    }
    if (noalias) {
        /* The cases without the check: the rewritten target must fail. */
        recomp_env_set(RENV_RT_ALIAS_CHECK, "0");
        failures = alias_cases(gpu);
        fprintf(stderr, "[backend-smoke] rt_alias_check=0: %d case(s) fail, as expected"
                " when at least 1\n", failures);
        return failures ? 0 : 1;
    }
#else
    (void)guard;
    (void)noalias;
#endif

    for (sc = 0; sc < SC_COUNT; sc++) {
        char path[512];
        if (!run(nv2a_pb_cpu_backend(), sc, cpu_px, k++)
                || !run(gpu, sc, gpu_px, k++)) {
            failures++;
            continue;
        }
        if (dump) {
            snprintf(path, sizeof path, "%s%s_cpu.bmp", dump, s_scene_name[sc]);
            write_bmp(path, cpu_px);
            snprintf(path, sizeof path, "%s%s_gpu.bmp", dump, s_scene_name[sc]);
            write_bmp(path, gpu_px);
        }
        {
            int same = compare(s_scene_name[sc], cpu_px, gpu_px);
            int pc = probes_ok(sc, "cpu", cpu_px), pg = probes_ok(sc, "gpu", gpu_px);
            if (!same || !pc || !pg)
                failures++;
        }
    }
    nv2a_pb_set_backend(NULL);
#ifdef __APPLE__
    if (gpu == nv2a_pb_metal_backend()) {
        int f = clip_cases(gpu, cpu_px, gpu_px);
        if (f)
            fprintf(stderr, "[backend-smoke] FAIL: %d clip-extent cases\n", f);
        failures += f;
        failures += occ_case(gpu);
        f = border_cases(gpu);
        if (f)
            fprintf(stderr, "[backend-smoke] FAIL: %d border colour cases\n", f);
        failures += f;
    }
    if (gpu == nv2a_pb_metal_backend() && run(nv2a_pb_cpu_backend(), SC_CLEAR_QUAD, cpu_px, 0)) {
        int f = writeback_cases(gpu, cpu_px, gpu_px);
        if (f)
            fprintf(stderr, "[backend-smoke] FAIL: %d write-back checks\n", f);
        failures += f;
        failures += redraw_case(gpu);
        failures += edge_case(gpu, "gpu");
        f = alias_cases(gpu);
        if (f)
            fprintf(stderr, "[backend-smoke] FAIL: %d render-target ownership checks\n", f);
        failures += f;
    }
#endif
    /* Last: they set a display mode and fill the table (scanout_cases). */
    failures += scanout_cases();
#ifdef __APPLE__
    failures += mode_height_cases(gpu == nv2a_pb_metal_backend() ? gpu : NULL);
#else
    failures += mode_height_cases(NULL);
#endif
    if (failures) {
        fprintf(stderr, "[backend-smoke] FAIL: %d of %d scenes\n", failures, SC_COUNT);
        return 1;
    }
    fprintf(stderr, "[backend-smoke] done: %d scenes identical on both paths\n", SC_COUNT);
    return 0;
}
