/**
 * d3d11_backend_smoke: the pushbuffer executor, drawing through D3D11.
 *
 * Sends nv2a_pb_exec_method() what a title's pushbuffer would: a 640x480
 * surface, a colour clear, one pre-transformed quad as INLINE_ARRAY (float4
 * position in pixels + D3DCOLOR), then the flip, double-buffered the way the
 * XDK does it (color_offset moves to the other buffer before FLIP_STALL).
 * Then textured quads (a swizzled checker, a linear YUY2 ramp) and a quad
 * through four register-combiner stages with the title's final combiner,
 * three overlapping quads with depth tests, and a stencil mask, and a
 * mipmapped texture drawn at three sizes.
 * Then render-target ownership (rt-stale-alias): a target whose guest
 * memory the title rewrote from the CPU is sampled as the new texels, and
 * one left alone still as its rendered image.
 * Then the CPU path's clip clear and clear rect (nv2a_clear_box) in guest
 * memory, the CPU halves of clip_cases: no other Proton test clears on the
 * CPU path.
 * Then index batches (pb-index-batch-cap), as nv2a_backend_smoke's bigidx
 * and elem32: a quad list of 4800 ARRAY_ELEMENT16 indices whose quads from
 * index 4096 on have their own colour, and a triangle list whose odd last
 * index is an ARRAY_ELEMENT32; a pixel inside each must be drawn.
 * Then the display mode's frame: with a 640x480 mode the crop case shows
 * 640x480, and a buffer drawn only at 320x240 is grown to it.
 * Last, present.surface=internal: a live render.scale change resizes the
 * swap chain to the frame's host size.
 * With RECOMP_PB_BACKEND=d3d11 that reaches ClearRenderTargetView, a draw
 * and Present in src/d3d/nv2a_pb_d3d11.c.
 *
 * Usage (under Proton: umu-run d3d11_backend_smoke.exe [frames]):
 *   RECOMP_STDIO_LOG=<file>      stderr goes here (Proton drops stdio)
 *   RECOMP_D3D11_DUMP=<prefix>   presented frames 1, 61, ... as BMPs
 */
#include <windows.h>
#include "recomp_env.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_swizzle.h"
#include "../kernel/nv2a_pb_state.h"
#include "../kernel/nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
uint32_t nv2a_pb_d3d11_pixel(uint32_t offset, int x, int y);
uint32_t nv2a_pb_d3d11_back_pixel(int x, int y);
void nv2a_pb_d3d11_back_size(uint32_t *w, uint32_t *h);
int nv2a_pb_d3d11_shown(uint32_t offset, uint32_t *tw, uint32_t *th,
                        uint32_t *sw, uint32_t *sh);
ptrdiff_t xbox_GetMemoryOffset(void);
void xbox_SetDisplayMode(uint32_t fb_va, uint32_t pitch, uint32_t mode, uint32_t format);
void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch);
void xbox_DisplayModeBump(void);

/* The kernel library expects these from the title's generated code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

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

static void vertex(float x, float y, uint32_t argb)
{
    vertex_uv(x, y, argb, NULL);
}

/* An untextured vertex at depth z, in depth units (0..2^24-1 for Z24S8). */
static void vertex_z(float x, float y, float z, uint32_t argb)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(z));
    M(0x1818, f2u(1.0f));
    M(0x1818, argb);
}

/* Guest memory for the texture: the runtime's memory offset is 0 here, so a
 * guest address is a host address, and it has to fit in 32 bits. */
#define TEX_GUEST 0x10000000u
/* The surfaces: two 640x480 buffers and a 320x240 render target
 * (OFFSCREEN) at SURF_BASE + 0x800000, all committed, since the backend
 * reads the guest bytes under a target. 16 MB, or a higher block when Wine
 * already uses that range. */
static uint32_t s_surf_base = 0x01000000u;
#define OFFSCREEN (s_surf_base + 0x00800000u)
/* clip_cases' render.scale 2 crop: a 640x480 buffer of its own, since a
 * target's host size is fixed when it is made. */
#define CROP2     (s_surf_base + 0x00400000u)
/* mode_cases' grow: a 640x480 buffer drawn only at 320x240, in the gap
 * after the two back buffers. */
#define GROW      (s_surf_base + 0x00258000u)
#define P8_GUEST  (TEX_GUEST + 0x8000u)
#define PAL_GUEST (TEX_GUEST + 0x9000u)
#define MIP_GUEST (TEX_GUEST + 0xA000u)  /* 32x32 A8R8G8B8, 6 levels */
#define RTT_A     0x10100000u   /* 64x64 render-to-texture targets, pitch 256 */
#define RTT_B     0x10110000u
#define VB_GUEST  0x10200000u   /* index cases: 96000 bytes of vertex arrays, 0x20000 committed */
#define TEX_LOG2  6                            /* 64x64 */
/* Four 16-byte visibility-test reports: GET_REPORT takes a 24-bit offset, so
 * the first free 64 KB below 16 MB. */
static uint32_t s_report_guest;
static int      s_vis_failures, s_vis_late;
static int      s_cross_exact, s_cross_stale, s_cross_first, s_cross_bad;
/* Report 3's push time by frame parity, and the push time of the frame
 * whose count it last read: the tracker forgets an address's count
 * NV2A_OCC_LATE_US (250 ms) after that report's push. */
static DWORD    s_cross_push[2], s_cross_hist_ms;
#define REPORT_GUEST s_report_guest
/* Report 3's open test covers 100 pixels on even frames and 200 on odd ones,
 * so a count read a frame later tells the frame's own result (exact) from
 * the address's last one (stale, settled at the flip) apart. */
#define CROSS_COUNT(f) ((f) & 1 ? 200u : 100u)
#define OCC_VISIBLE    0x10000u    /* NV2A_OCC_VISIBLE: no history yet */

/* Visibility test: CLEAR_REPORT_VALUE, ZPASS count on, a quad, count off,
 * GET_REPORT into report k. EndVisibilityTest sets the status to 0xFFFFFFFF
 * just before it sends GET_REPORT, as D3D does. */
static void vis_test(int k, float x0, float y0, float x1, float y1, float z)
{
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(REPORT_GUEST + 16u * k);
    M(0x17C8, 1);                          /* CLEAR_REPORT_VALUE */
    M(0x17CC, 1);                          /* ZPASS_PIXEL_COUNT_ENABLE */
    if (x1 > x0) {                         /* else an empty test: count 0 */
        M(0x17FC, 8);
        vertex_z(x0, y0, z, 0xFFFFFFFFu);
        vertex_z(x1, y0, z, 0xFFFFFFFFu);
        vertex_z(x1, y1, z, 0xFFFFFFFFu);
        vertex_z(x0, y1, z, 0xFFFFFFFFu);
        M(0x17FC, 0);
    }
    M(0x17CC, 0);
    r[3] = 0xFFFFFFFFu;
    M(0x17D0, (1u << 24) | (REPORT_GUEST + 16u * k));   /* GET_REPORT */
}

/* A swizzled A8R8G8B8 texture: 8x8-texel checker, orange and blue, with a
 * white top row and a green left column so orientation shows. */
static int make_texture(void)
{
    uint32_t n = 1u << TEX_LOG2, x, y;
    uint32_t *p = (uint32_t *)VirtualAlloc((void *)(uintptr_t)TEX_GUEST,
                                           0x10000, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE);
    if (!p)
        return 0;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            uint32_t c = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFF2040FFu : 0xFFFF8000u;
            if (y == 0) c = 0xFFFFFFFFu;
            if (x == 0) c = 0xFF00FF00u;
            p[swizzle_offset(x, y, n, n)] = c;
        }
    /* A linear YUY2 image (the movie texture's format), 64x32 at pitch 128,
     * right after it: luma ramps left to right, Cr top to bottom, so it
     * goes from black to white with red growing downwards. */
    {
        uint8_t *y8 = (uint8_t *)p + n * n * 4;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 64; x += 2) {
                uint8_t *g = y8 + y * 128 + x * 2;
                g[0] = (uint8_t)(16 + x * 219 / 63);           /* Y0 */
                g[1] = 128;                                     /* Cb */
                g[2] = (uint8_t)(16 + (x + 1) * 219 / 63);     /* Y1 */
                g[3] = (uint8_t)(128 + y * 3);                  /* Cr */
            }
    }
    /* A swizzled P8 texture, 16x16, at P8_GUEST, and its palette at
     * PAL_GUEST: 4x4-texel diagonal bands through entries 0..3 (red,
     * yellow, cyan, white); the other 252 entries dark grey. */
    {
        uint8_t *i8 = (uint8_t *)(uintptr_t)P8_GUEST;
        uint32_t *pal = (uint32_t *)(uintptr_t)PAL_GUEST;
        static const uint32_t band[4] = {0xFFFF0000u, 0xFFFFFF00u, 0xFF00FFFFu, 0xFFFFFFFFu};
        for (x = 0; x < 256; x++)
            pal[x] = x < 4 ? band[x] : 0xFF404040u;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                i8[swizzle_offset(x, y, 16, 16)] = (uint8_t)(((x >> 2) + (y >> 2)) & 3);
    }
    /* A swizzled A8R8G8B8 32x32 with its full chain (6 levels, packed one
     * after another), each level one colour: red, green, blue, yellow,
     * cyan, magenta. Drawn minified, a level's colour says which one the
     * sampler picked. */
    {
        static const uint32_t lc[6] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                                       0xFFFFFF00u, 0xFF00FFFFu, 0xFFFF00FFu};
        uint32_t *m = (uint32_t *)(uintptr_t)MIP_GUEST, l, d;
        for (l = 0, d = 32; l < 6; l++, d /= 2)
            for (x = 0; x < d * d; x++)
                *m++ = lc[l];
    }
    return 1;
}

/* A 32-bit surface at buf, w x h, pitch w * 4, cleared to argb. */
static void surf_clear(uint32_t buf, uint32_t w, uint32_t h, uint32_t argb)
{
    M(0x0200, w << 16);
    M(0x0204, h << 16);
    M(0x0208, 0x00000128u);
    M(0x020C, w * 4u);
    M(0x0210, buf);
    M(0x0214, 0x02400000u);
    M(0x1B0C, 0);
    M(0x1D90, argb);
    M(0x1D94, 0xF0);
}

/* buf (640x480) with a quad at (100,100)-(164,164) sampling the 64x64
 * linear A8R8G8B8 texture at tex; returns the quad's centre, then flips. */
static uint32_t sample_rtt(uint32_t buf, uint32_t other, uint32_t tex)
{
    static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 64}, {0, 64}};
    uint32_t c;
    int a, k;

    surf_clear(buf, 640, 480, 0xFF0000FFu);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
    M(0x1B00, tex);
    M(0x1B04, (0x12u << 8) | (1u << 4) | (2u << 0));
    M(0x1B08, 0x00000303u);
    M(0x1B0C, 0x40000000u);
    M(0x1B10, 256u << 16);
    M(0x1B1C, (64u << 16) | 64u);
    M(0x17FC, 8);
    for (k = 0; k < 4; k++)
        vertex_uv(k == 1 || k == 2 ? 164.0f : 100.0f, k >= 2 ? 164.0f : 100.0f,
                  0xFFFFFFFFu, uv[k]);
    M(0x17FC, 0);
    M(0x1B0C, 0);
    M(0x1760 + 4 * 9, 0x02u);
    c = nv2a_pb_d3d11_pixel(buf, 132, 132);
    M(0x0210, other);
    M(0x0130, 0);
    return c;
}

/* Render-target ownership; returns the failures. */
static int alias_cases(const uint32_t *buf)
{
    uint32_t c, i;
    int f = 0;

    if (!VirtualAlloc((void *)(uintptr_t)RTT_A, 0x20000, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE)) {
        fprintf(stderr, "[backend-smoke] rt alias: no memory at 0x%08X\n", RTT_A);
        return 1;
    }
    /* Drawn red, then the title writes green over its memory. */
    surf_clear(RTT_A, 64, 64, 0xFFFF0000u);
    surf_clear(buf[0], 640, 480, 0xFF000000u);
    M(0x0210, buf[1]);
    M(0x0130, 0);
    for (i = 0; i < 64 * 64; i++)
        ((uint32_t *)(uintptr_t)RTT_A)[i] = 0xFF00FF00u;
    c = sample_rtt(buf[0], buf[1], RTT_A);
    fprintf(stderr, "[backend-smoke] rt alias: rewritten target samples 0x%08X (want"
            " 0xFF00FF00) %s\n", c, c == 0xFF00FF00u ? "PASS" : "FAIL");
    f += c != 0xFF00FF00u;
    /* Drawn red and left alone: render-to-texture as before. */
    surf_clear(RTT_B, 64, 64, 0xFFFF0000u);
    surf_clear(buf[0], 640, 480, 0xFF000000u);
    M(0x0210, buf[1]);
    M(0x0130, 0);
    c = sample_rtt(buf[0], buf[1], RTT_B);
    fprintf(stderr, "[backend-smoke] rt alias: untouched target samples 0x%08X (want"
            " 0xFFFF0000) %s\n", c, c == 0xFFFF0000u ? "PASS" : "FAIL");
    f += c != 0xFFFF0000u;
    return f;
}

/* Index batches: the same grid and triangle list as nv2a_backend_smoke's
 * bigidx and elem32. Vertex arrays at VB_GUEST: attr0 float4 position
 * (pixels, w 1) and attr3 D3DCOLOR, 20-byte stride. */
#define GRID_COLS  40u
#define GRID_QUADS 1200u
#define COL_LOW    0xFF2080E0u
#define COL_HIGH   0xFFE0C020u
#define COL_TRI    0xFF40E040u

static void vb_vertex(uint32_t i, float x, float y, uint32_t argb)
{
    uint8_t *v = (uint8_t *)(uintptr_t)VB_GUEST + i * 20u;
    float p[4] = {x, y, 0.0f, 1.0f};
    memcpy(v, p, 16);
    memcpy(v + 16, &argb, 4);
}

static void vb_arrays(void)
{
    int a;
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? (20u << 8) | 0x42u : a == 3 ? (20u << 8) | 0x40u : 0x02u);
    M(0x1720 + 4 * 0, VB_GUEST);
    M(0x1720 + 4 * 3, VB_GUEST + 16u);
}

static int index_probe(uint32_t buf, const char *what, int x, int y, uint32_t want)
{
    uint32_t c = nv2a_pb_d3d11_pixel(buf, x, y);
    int ok = (c & 0x00FFFFFFu) == (want & 0x00FFFFFFu);
    fprintf(stderr, "[backend-smoke] index batch: %-34s 0x%08X (want 0x%08X) %s\n",
            what, c, want, ok ? "PASS" : "FAIL");
    return !ok;
}

/* The XDK's rectangle: every corner at a pixel edge plus 0.53125. Snapped to
 * the NV2A's 1/16 grid its edges sit on pixel centres, and the top-left rule
 * keeps the first column and row and leaves the column and row past the far
 * edges (nv2a_backend_smoke's edge_case, on D3D11). Returns the failures. */
static int edge_cases(const uint32_t *buf)
{
    static const struct { int x, y, lit; } pr[] = {
        {0, 0, 1}, {0, 30, 1}, {30, 0, 1}, {63, 63, 1}, {64, 30, 0}, {30, 64, 0},
        {64, 64, 0},
        {100, 100, 1}, {99, 130, 0}, {130, 99, 0}, {163, 163, 1}, {164, 130, 0},
        {130, 164, 0},
    };
    static const float q[2][2] = {{0.53125f, 64.53125f}, {100.53125f, 164.53125f}};
    unsigned i;
    int a, k, f = 0;

    surf_clear(buf[0], 640, 480, 0xFF000000u);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
    for (k = 0; k < 2; k++) {
        M(0x17FC, 8);
        vertex(q[k][0], q[k][0], 0xFFFFFFFFu);
        vertex(q[k][1], q[k][0], 0xFFFFFFFFu);
        vertex(q[k][1], q[k][1], 0xFFFFFFFFu);
        vertex(q[k][0], q[k][1], 0xFFFFFFFFu);
        M(0x17FC, 0);
    }
    for (i = 0; i < sizeof pr / sizeof pr[0]; i++) {
        uint32_t c = nv2a_pb_d3d11_pixel(buf[0], pr[i].x, pr[i].y) & 0x00FFFFFFu;
        uint32_t want = pr[i].lit ? 0x00FFFFFFu : 0;
        if (c != want) {
            fprintf(stderr, "[backend-smoke] edges: pixel (%d,%d) %06X, want %06X\n",
                    pr[i].x, pr[i].y, c, want);
            f++;
        }
    }
    M(0x0210, buf[1]);
    M(0x0130, 0);
    fprintf(stderr, "[backend-smoke] edges: 0.53125-offset quads keep column and row 0,"
            " not w and h %s\n", f ? "FAIL" : "PASS");
    return f;
}

/* Returns the failures. */
static int index_cases(const uint32_t *buf)
{
    static const uint16_t list[15] = {0, 1, 2, 0, 2, 3, 4, 4, 4, 5, 6, 7, 5, 7, 8};
    uint32_t q, i;
    int a, f = 0;

    if (!VirtualAlloc((void *)(uintptr_t)VB_GUEST, 0x20000, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE)) {
        fprintf(stderr, "[backend-smoke] index batch: no memory at 0x%08X\n", VB_GUEST);
        return 1;
    }
    M(0x0304, 0);                              /* no blend, depth or stencil */
    M(0x030C, 0);
    M(0x035C, 0);
    M(0x032C, 0);

    /* bigidx: 1200 quads, one batch of 4800 indices. */
    for (q = 0; q < GRID_QUADS; q++) {
        float x = (float)((q % GRID_COLS) * 16u + 2u), y = (float)((q / GRID_COLS) * 16u + 2u);
        uint32_t c = q * 4u >= 4096u ? COL_HIGH : COL_LOW;
        vb_vertex(q * 4u + 0u, x, y, c);
        vb_vertex(q * 4u + 1u, x + 12.0f, y, c);
        vb_vertex(q * 4u + 2u, x + 12.0f, y + 12.0f, c);
        vb_vertex(q * 4u + 3u, x, y + 12.0f, c);
    }
    surf_clear(buf[0], 640, 480, 0xFF603010u);
    vb_arrays();
    M(0x17FC, 8);                              /* QUADS */
    for (i = 0; i < GRID_QUADS * 4u; i += 2)
        M(0x1800, i | ((i + 1u) << 16));       /* ARRAY_ELEMENT16 */
    M(0x17FC, 0);
    f += index_probe(buf[0], "quad 1100 (index 4400)", 20 * 16 + 8, 27 * 16 + 8, COL_HIGH);
    f += index_probe(buf[0], "quad 10", 10 * 16 + 8, 8, COL_LOW);
    M(0x0210, buf[1]);
    M(0x0130, 0);

    /* elem32: two rectangles and a degenerate triangle, the fifteenth index
     * as ARRAY_ELEMENT32. */
    vb_vertex(0, 100, 280, 0xFFE02020u);
    vb_vertex(1, 220, 280, 0xFFE02020u);
    vb_vertex(2, 220, 380, 0xFFE02020u);
    vb_vertex(3, 100, 380, 0xFFE02020u);
    vb_vertex(4, 300, 300, 0xFFE02020u);
    vb_vertex(5, 450, 280, COL_TRI);
    vb_vertex(6, 570, 280, COL_TRI);
    vb_vertex(7, 570, 380, COL_TRI);
    vb_vertex(8, 450, 380, COL_TRI);
    surf_clear(buf[0], 640, 480, 0xFF603010u);
    vb_arrays();
    M(0x17FC, 5);                              /* TRIANGLES */
    for (i = 0; i < 14; i += 2)
        M(0x1800, list[i] | ((uint32_t)list[i + 1] << 16));
    M(0x1808, list[14]);                       /* ARRAY_ELEMENT32 */
    M(0x17FC, 0);
    f += index_probe(buf[0], "last triangle (ARRAY_ELEMENT32)", 470, 370, COL_TRI);
    f += index_probe(buf[0], "first rectangle", 150, 330, 0xFFE02020u);
    for (a = 0; a < 16; a++)
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
    M(0x0210, buf[1]);
    M(0x0130, 0);
    return f;
}

/* A CLIPPLANE stage (texture-shader mode 5) kills by its coordinate's
 * sign: SHADER_CLIP_PLANE_MODE bit 4n+j set kills tc[j] >= 0, clear kills
 * tc[j] < 0. u runs -1..1 across each quad, so the stage's nibble 0 keeps
 * the right half and nibble 1 the left (nv2a_backend_smoke's clipplane
 * scenes, on D3D11). Drawn through a vertex program (oT<stage> = v9), with
 * one combiner stage, R0 = V0, as the game does, or with none (rc 0), which
 * takes the basic shader and its stage-0 kill. The other stage's nibble is
 * always the opposite one, so a kill that reads the wrong nibble flips both
 * quads. Returns the failures. */
static int clipplane_case(uint32_t buf, const char *what, int stage, int rc)
{
    static const float uv[4][2] = {{-1, 0}, {1, 0}, {1, 1}, {-1, 1}};
    static const struct { const char *what; int x, y; uint32_t want; } pr[] = {
        {"nibble 0, u < 0 killed", 150, 150, 0xFF603010u},
        {"nibble 0, u >= 0 kept", 250, 150, 0xFF30C0E0u},
        {"nibble 1, u < 0 kept", 150, 330, 0xFF30C0E0u},
        {"nibble 1, u >= 0 killed", 250, 330, 0xFF603010u},
    };
    static const float y0[2] = {100.0f, 280.0f};
    const uint32_t outs[3][2] = {{0, 0}, {3, 3}, {9u + (uint32_t)stage, 9}};   /* {o, v} */
    unsigned i;
    int k, f = 0;

    surf_clear(buf, 640, 480, 0xFF603010u);
    M(0x0304, 0);                              /* no blend, depth or stencil */
    M(0x030C, 0);
    M(0x035C, 0);
    M(0x032C, 0);
    M(0x1B0C, 0);                              /* stage 0 unbound */
    M(0x1E9C, 0);                              /* PROGRAM_LOAD: slot 0 */
    for (k = 0; k < 3; k++) {
        M(0x0B00 + 16 * k + 0, 0);
        M(0x0B00 + 16 * k + 4, (1u << 21) | (outs[k][1] << 9) | 0x1Bu);
        M(0x0B00 + 16 * k + 8, 2u << 26);      /* A = v */
        M(0x0B00 + 16 * k + 12, (0xFu << 12) | (1u << 11)
                                | (outs[k][0] << 3) | (k == 2 ? 1u : 0u));
    }
    M(0x1EA0, 0);                              /* PROGRAM_START */
    M(0x1E94, 2);                              /* EXECUTION_MODE: program */
    M(0x1E70, 5u << (5 * stage));              /* SHADER_STAGE_PROGRAM: CLIPPLANE */
    if (rc) {
        M(0x0AC0, 0x04200000u);                /* COLOR_ICW 0: A V0, B 1 */
        M(0x1E40, 0x000000C0u);                /* COLOR_OCW 0: AB -> R0 */
        M(0x0260, 0x14200000u);                /* ALPHA_ICW 0: A V0.a, B 1 */
        M(0x0AA0, 0x000000C0u);                /* ALPHA_OCW 0: AB -> R0 */
        M(0x0288, 0x0000000Cu);                /* SPECULAR_FOG_CW0: D = R0 */
        M(0x028C, 0x00001C80u);                /* SPECULAR_FOG_CW1: G = R0.a */
    }
    M(0x1E60, rc ? 0x00000001u : 0u);          /* COMBINER_CONTROL: 1 stage or none */
    M(0x1760 + 4 * 9, 0x22u);                  /* attr9: float2 */
    for (k = 0; k < 2; k++) {
        /* SHADER_CLIP_PLANE_MODE: nibble k for the stage, 1 - k for the other */
        M(0x17F8, ((uint32_t)k << (4 * stage)) | ((uint32_t)(1 - k) << (4 * (1 - stage))));
        M(0x17FC, 8);
        vertex_uv(100.0f, y0[k],          0xFF30C0E0u, uv[0]);
        vertex_uv(300.0f, y0[k],          0xFF30C0E0u, uv[1]);
        vertex_uv(300.0f, y0[k] + 100.0f, 0xFF30C0E0u, uv[2]);
        vertex_uv(100.0f, y0[k] + 100.0f, 0xFF30C0E0u, uv[3]);
        M(0x17FC, 0);
    }
    M(0x17F8, 0);
    M(0x1E94, 0);                              /* fixed function again */
    M(0x1E60, 0);                              /* combiners off */
    M(0x1E70, 1u);                             /* stage 0 2D: the power-on mapping */
    M(0x1760 + 4 * 9, 0x02u);
    M(0x1B0C, 0);                              /* stage 0 off, as border_cases leaves it */
    for (i = 0; i < sizeof pr / sizeof pr[0]; i++) {
        uint32_t c = nv2a_pb_d3d11_pixel(buf, pr[i].x, pr[i].y);
        int ok = (c & 0x00FFFFFFu) == (pr[i].want & 0x00FFFFFFu);
        fprintf(stderr, "[backend-smoke] clipplane %s: %-24s 0x%08X (want 0x%08X) %s\n",
                what, pr[i].what, c, pr[i].want, ok ? "PASS" : "FAIL");
        f += !ok;
    }
    return f;
}

/* Stage 0 under a combiner, stage 1 under a combiner (the 4n nibble shift),
 * and stage 0 with no combiners (the basic shader's kill, which only the
 * GPU paths have: the CPU path's no-combiner rasteriser kills only a
 * textured stage 0). Returns the failures. */
static int clipplane_cases(uint32_t buf)
{
    return clipplane_case(buf, "t0", 0, 1) + clipplane_case(buf, "t1", 1, 1)
           + clipplane_case(buf, "t0 basic", 0, 0);
}

/* Present a 320x240 frame of a 640x480 target (buf, bound): red whole
 * frames first, into both swap-chain buffers, then more than
 * PRESENT_STALE_FLIPS flips of a blue 320x240 clip, so the walker's present
 * extent is the frame. Back-buffer (100,100) must be the frame's blue. At
 * stock (400,300) is the band outside the frame and must be black, which a
 * flip-discard buffer (DXVK) would still hold red from the whole frames; at
 * a render.scale the blit's crop puts guest (200,150) there, blue (see the
 * caller). A copy that is lost or mis-sized fails one probe or the other.
 * Returns the failures. */
static int crop_flips(uint32_t buf)
{
    static const int px[2] = {100, 400}, py[2] = {100, 300};
    uint32_t got[2];
    unsigned scale = nv2a_host_render_scale();
    int f = 0, k, i;

    M(0x0210, buf);
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    for (k = 0; k < 2; k++) {
        M(0x1D90, 0xFFFF0000u);
        M(0x1D94, 0xF0);
        M(0x012C, 0);                       /* FLIP_INCREMENT_WRITE */
        M(0x0130, 0);
    }
    M(0x0200, 320u << 16);
    M(0x0204, 240u << 16);
    /* The probe reads one pixel per flip: the first point through the
     * next-to-last flip, the second at the last. */
    nv2a_pb_d3d11_back_pixel(px[0], py[0]);
    for (k = 0; k < 122; k++) {
        if (k == 121)
            got[0] = nv2a_pb_d3d11_back_pixel(px[1], py[1]);
        M(0x1D90, 0xFF0000FFu);
        M(0x1D94, 0xF0);
        M(0x012C, 0);
        M(0x0130, 0);
    }
    got[1] = nv2a_pb_d3d11_back_pixel(px[1], py[1]);
    nv2a_pb_d3d11_back_pixel(-1, -1);       /* off: no read per flip after */
    for (i = 0; i < 2; i++) {
        uint32_t want = i == 0 || scale > 1 ? 0xFF0000FFu : 0xFF000000u;
        fprintf(stderr, "[backend-smoke] crop x%u: back buffer (%d,%d) 0x%08X (want"
                " 0x%08X) %s\n", scale, px[i], py[i], got[i], want,
                got[i] == want ? "PASS" : "FAIL");
        f += got[i] != want;
    }
    return f;
}

/* Red, then a 160x120 clip at (100,100) and a clear rect of (50,50)-(399,299)
 * cleared green: the rect alone, ended by the clip's far corner (where a
 * pitch surface ends, as xemu sizes it), so (50,50)-(259,219) turns green,
 * above and left of the clip too (nv2a_clear_box, upstream #212's rule).
 * Read from the D3D11 target, or with `cpu` from guest memory after the CPU
 * path's clear. Returns the failures. */
static int rect_past_clip(uint32_t buf, int cpu)
{
    static const struct { uint32_t x, y, argb; } want[5] = {
        {50, 50, 0xFF00FF00u}, {259, 219, 0xFF00FF00u}, {150, 150, 0xFF00FF00u},
        {49, 150, 0xFFFF0000u}, {260, 150, 0xFFFF0000u},
    };
    const uint8_t *g = (const uint8_t *)xbox_GetMemoryOffset() + nv2a_pb_dma_resolve(buf);
    int f = 0, k;

    surf_clear(buf, 640, 480, 0xFFFF0000u);
    M(0x0200, (160u << 16) | 100u);
    M(0x0204, (120u << 16) | 100u);
    M(0x1D98, (399u << 16) | 50u);
    M(0x1D9C, (299u << 16) | 50u);
    M(0x1D90, 0xFF00FF00u);
    M(0x1D94, 0xF0);
    M(0x1D98, (639u << 16) | 0u);
    M(0x1D9C, (479u << 16) | 0u);
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    for (k = 0; k < 5; k++) {
        uint32_t got = cpu ? *(const uint32_t *)(g + want[k].y * 2560u + want[k].x * 4u)
                           : nv2a_pb_d3d11_pixel(buf, want[k].x, want[k].y);
        fprintf(stderr, "[backend-smoke] %s rect past clip %d: 0x%08X (want 0x%08X) %s\n",
                cpu ? "cpu" : "d3d11", k, got, want[k].argb,
                got == want[k].argb ? "PASS" : "FAIL");
        f += got != want[k].argb;
    }
    return f;
}

/* One back buffer drawn under two clip extents in a frame (Burnout 3's
 * 640x480 buffer with 159x344 and smaller clips): one target, grown to the
 * largest; a clear bounded by its clear rect or clip leaves the rest, and a
 * draw by its clip; and a
 * flip whose frame is smaller than the target shows only the frame. Returns
 * the failures. */
static int clip_cases(const uint32_t *buf)
{
    uint32_t c[4], tw = 0, th = 0, sw = 0, sh = 0;
    int f = 0, k;

    /* Red over the whole 640x480, then a 160x120 clip at (100,100) cleared
     * blue with the clip as the clear rect, as D3D's Clear sends it: only
     * the clip turns blue. */
    surf_clear(buf[0], 640, 480, 0xFFFF0000u);
    M(0x0200, (160u << 16) | 100u);
    M(0x0204, (120u << 16) | 100u);
    M(0x1D98, (259u << 16) | 100u);
    M(0x1D9C, (219u << 16) | 100u);
    M(0x1D90, 0xFF0000FFu);
    M(0x1D94, 0xF0);
    /* The whole clip again, with a clear rect of (300,300)-(339,339): green
     * there and nowhere else. */
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    M(0x1D98, (339u << 16) | 300u);
    M(0x1D9C, (339u << 16) | 300u);
    M(0x1D90, 0xFF00FF00u);
    M(0x1D94, 0xF0);
    /* The clear rect is global state, as on the hardware, and stays set
     * across surfaces: back to the whole surface for the cases after. */
    M(0x1D98, (639u << 16) | 0u);
    M(0x1D9C, (479u << 16) | 0u);
    c[0] = nv2a_pb_d3d11_pixel(buf[0], 50, 50);      /* red: outside both */
    c[1] = nv2a_pb_d3d11_pixel(buf[0], 150, 150);    /* blue: inside the clip */
    c[2] = nv2a_pb_d3d11_pixel(buf[0], 320, 320);    /* green: the clear rect */
    c[3] = nv2a_pb_d3d11_pixel(buf[0], 350, 350);    /* red: past it */
    {
        static const uint32_t want[4] = {0xFFFF0000u, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
        for (k = 0; k < 4; k++) {
            fprintf(stderr, "[backend-smoke] clip clear %d: 0x%08X (want 0x%08X) %s\n", k,
                    c[k], want[k], c[k] == want[k] ? "PASS" : "FAIL");
            f += c[k] != want[k];
        }
    }
    f += rect_past_clip(buf[0], 0);
    /* Red again, then under the 160x120 clip a blue quad past it on every
     * side: the clip scissors the draw, as on the hardware and the CPU path
     * (Burnout 3's HUD past its 640x448 clip at y 16). (100,100) and
     * (259,219) are inside, one pixel past each edge is not. */
    {
        static const struct { int x, y; uint32_t argb; } want[6] = {
            {100, 100, 0xFF0000FFu}, {259, 219, 0xFF0000FFu}, {99, 150, 0xFFFF0000u},
            {150, 99, 0xFFFF0000u},  {260, 150, 0xFFFF0000u}, {150, 220, 0xFFFF0000u},
        };
        int a;
        surf_clear(buf[0], 640, 480, 0xFFFF0000u);
        M(0x0200, (160u << 16) | 100u);
        M(0x0204, (120u << 16) | 100u);
        M(0x0304, 0);                          /* no blend, depth or stencil */
        M(0x030C, 0);
        M(0x035C, 0);
        M(0x032C, 0);
        for (a = 0; a < 16; a++)
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        M(0x17FC, 8);
        vertex(40.0f, 40.0f, 0xFF0000FFu);
        vertex(320.0f, 40.0f, 0xFF0000FFu);
        vertex(320.0f, 300.0f, 0xFF0000FFu);
        vertex(40.0f, 300.0f, 0xFF0000FFu);
        M(0x17FC, 0);
        M(0x0200, 640u << 16);
        M(0x0204, 480u << 16);
        for (k = 0; k < 6; k++) {
            uint32_t got = nv2a_pb_d3d11_pixel(buf[0], want[k].x, want[k].y);
            fprintf(stderr, "[backend-smoke] draw clip %d: 0x%08X (want 0x%08X) %s\n", k,
                    got, want[k].argb, got == want[k].argb ? "PASS" : "FAIL");
            f += got != want[k].argb;
        }
    }
    /* The frame is 320x240 of that grown target: the flip shows 320x240.
     * The walker presents the largest fresh surface, so the 640x480 entries
     * (this clip's and the frames' above) must first go stale: more than
     * PRESENT_STALE_FLIPS (120) flips drawing only the 320x240 clip. The
     * walker counts a flip at FLIP_INCREMENT_WRITE, as the XDK's swap
     * pushes it, not at FLIP_STALL alone.
     *
     * The present itself is checked by back_probes. */
    f += crop_flips(buf[0]);
    if (!nv2a_pb_d3d11_shown(buf[0], &tw, &th, &sw, &sh) || tw != 640 || th != 480
            || sw != 320 || sh != 240) {
        fprintf(stderr, "[backend-smoke] crop: target %ux%u, shown %ux%u (want 640x480,"
                " 320x240) FAIL\n", tw, th, sw, sh);
        f++;
    } else {
        fprintf(stderr, "[backend-smoke] crop: target 640x480, shown 320x240 PASS\n");
    }
    /* At render.scale 2 the present is the blit, and its uv_scale does the
     * crop: the 640x480-host frame fills the 640x480 window, so there is no
     * band, but back-buffer (400,300) samples guest (200,150) of the frame,
     * blue. Without the crop the whole 1280x960 target is squeezed in and
     * (400,300) samples guest (400,300), red. */
    {
        struct nv2a_host_opts o = *nv2a_host_opts(), stock = o;
        o.render_scale = 2;
        nv2a_host_opts_set(&o);
        surf_clear(CROP2, 640, 480, 0xFFFF0000u);
        f += crop_flips(CROP2);
        nv2a_host_opts_set(&stock);
    }
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    M(0x0210, buf[0]);
    return f;
}

/* The display mode's frame (present-mode-height): with a 640x480 mode set,
 * the walker publishes 640x480 whatever clip drew the buffer, and this
 * backend shows that much. Crop with a mode: crop_flips' stream, whose
 * 320x240 frame of a 640x480 target is shown 320x240 without a mode, is
 * shown 640x480, so back-buffer (400,300) is the red the whole frames left
 * there. Grow: a buffer only ever drawn with a 320x240 clip has a 320x240
 * target, which present_extent grows to 640x480 and shows whole; (100,100)
 * is the clip's blue on screen. The grow margin is not checked: it is black
 * here and the guest's bytes on Metal and the CPU path (a TASKS follow-up).
 * Each case ends with no display mode. Returns the failures. */
static void mode_set(uint32_t buf)
{
    /* What the AvSetDisplayMode bridge records: NTSC-M 480i, 640x480,
     * LIN_A8R8G8B8 at pitch 2560, the resolved address. */
    xbox_SetDisplayMode(0x80000000u | buf, 2560u, 0x0801010Du, 0x12);
    xbox_DisplayModeBump();
}

static void mode_clear(void)
{
    xbox_SetDisplayFramebuffer(0, 0);
    xbox_DisplayModeBump();
}

static int mode_cases(uint32_t buf)
{
    uint32_t tw = 0, th = 0, sw = 0, sh = 0, got;
    int f = 0, k, ok;

    /* Crop with a mode. */
    mode_set(buf);
    M(0x0210, buf);
    M(0x020C, 640u * 4u);
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    for (k = 0; k < 2; k++) {
        M(0x1D90, 0xFFFF0000u);
        M(0x1D94, 0xF0);
        M(0x012C, 0);
        M(0x0130, 0);
    }
    M(0x0200, 320u << 16);
    M(0x0204, 240u << 16);
    nv2a_pb_d3d11_back_pixel(400, 300);
    for (k = 0; k < 122; k++) {
        M(0x1D90, 0xFF0000FFu);
        M(0x1D94, 0xF0);
        M(0x012C, 0);
        M(0x0130, 0);
    }
    got = nv2a_pb_d3d11_back_pixel(-1, -1);
    ok = nv2a_pb_d3d11_shown(buf, &tw, &th, &sw, &sh) && tw == 640 && th == 480
        && sw == 640 && sh == 480 && got == 0xFFFF0000u;
    fprintf(stderr, "[backend-smoke] mode-height: crop with a 640x480 mode, target %ux%u"
            " shown %ux%u, back buffer (400,300) 0x%08X (want 640x480, 640x480,"
            " 0xFFFF0000) %s\n", tw, th, sw, sh, got, ok ? "PASS" : "FAIL");
    f += !ok;
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    mode_clear();

    /* Grow at present. */
    {
        /* The guest bytes under it green, written from the CPU, so no
         * draw makes a 640x480 target first. */
        uint32_t *g = (uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + GROW);
        for (k = 0; k < 640 * 480; k++)
            g[k] = 0xFF00FF00u;
    }
    mode_set(GROW);
    M(0x0200, 320u << 16);
    M(0x0204, 240u << 16);
    M(0x0208, 0x00000128u);
    M(0x020C, 640u * 4u);
    M(0x0210, GROW);
    nv2a_pb_d3d11_back_pixel(100, 100);
    for (k = 0; k < 3; k++) {
        M(0x1D90, 0xFF0000FFu);
        M(0x1D94, 0xF0);
        M(0x012C, 0);
        M(0x0130, 0);
    }
    got = nv2a_pb_d3d11_back_pixel(-1, -1);
    ok = nv2a_pb_d3d11_shown(GROW, &tw, &th, &sw, &sh) && tw == 640 && th == 480
        && sw == 640 && sh == 480 && got == 0xFF0000FFu;
    fprintf(stderr, "[backend-smoke] mode-height: 320x240 clip under a 640x480 mode,"
            " target %ux%u shown %ux%u, back buffer (100,100) 0x%08X (want 640x480,"
            " 640x480, 0xFF0000FF) %s\n", tw, th, sw, sh, got, ok ? "PASS" : "FAIL");
    f += !ok;
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    M(0x0210, buf);
    mode_clear();
    return f;
}

/* clip_cases' two clears on the CPU path, read from guest memory: the
 * clear covers its clip, then only its clear rect, whose max edge is
 * inclusive ((339,339) green, (340,339) red). Run last: it rewrites the back
 * buffer's bytes under the D3D11 target. Returns the failures. */
static int cpu_clip_cases(uint32_t buf)
{
    static const struct { uint32_t x, y, argb; } want[6] = {
        {50, 50, 0xFFFF0000u}, {150, 150, 0xFF0000FFu},   /* clip clear */
        {320, 320, 0xFF00FF00u}, {350, 350, 0xFFFF0000u}, /* clear rect */
        {339, 339, 0xFF00FF00u}, {340, 339, 0xFFFF0000u},
    };
    const struct nv2a_pb_backend *b = nv2a_pb_get_backend();
    const uint8_t *g;
    uint32_t c[6];
    int f = 0, k;

    nv2a_pb_set_backend(nv2a_pb_cpu_backend());
    g = (const uint8_t *)xbox_GetMemoryOffset() + nv2a_pb_dma_resolve(buf);
    surf_clear(buf, 640, 480, 0xFFFF0000u);
    M(0x0200, (160u << 16) | 100u);
    M(0x0204, (120u << 16) | 100u);
    M(0x1D98, (259u << 16) | 100u);
    M(0x1D9C, (219u << 16) | 100u);
    M(0x1D90, 0xFF0000FFu);
    M(0x1D94, 0xF0);
    M(0x1D98, (639u << 16) | 0u);
    M(0x1D9C, (479u << 16) | 0u);
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    for (k = 0; k < 2; k++)
        c[k] = *(const uint32_t *)(g + want[k].y * 2560u + want[k].x * 4u);
    surf_clear(buf, 640, 480, 0xFFFF0000u);
    M(0x1D98, (339u << 16) | 300u);
    M(0x1D9C, (339u << 16) | 300u);
    M(0x1D90, 0xFF00FF00u);
    M(0x1D94, 0xF0);
    M(0x1D98, (639u << 16) | 0u);   /* back to the whole surface */
    M(0x1D9C, (479u << 16) | 0u);
    for (k = 2; k < 6; k++)
        c[k] = *(const uint32_t *)(g + want[k].y * 2560u + want[k].x * 4u);
    for (k = 0; k < 6; k++) {
        fprintf(stderr, "[backend-smoke] cpu clip clear %d: 0x%08X (want 0x%08X) %s\n", k,
                c[k], want[k].argb, c[k] == want[k].argb ? "PASS" : "FAIL");
        f += c[k] != want[k].argb;
    }
    f += rect_past_clip(buf, 1);
    nv2a_pb_set_backend(b);
    return f;
}

/* Texture address mode 4, border, with the border colour as the source
 * (FORMAT bit 3), as Burnout 3 sets stages 0-3: the swizzled checker on a
 * quad whose UVs run from -0.5 to 1.5, 1/32 pixel off the grid so no pixel
 * centre lands on a texel edge. Past the texture the tap is
 * SET_TEXTURE_BORDER_COLOR (any colour on D3D11; Metal's three are
 * nv2a_backend_smoke's), the middle texel (32,32) is orange. With the
 * texture as the source the edge texel stands in: green left of it. D3D11
 * first, then the CPU path from guest memory. Returns the failures. */
static int border_cases(uint32_t buf)
{
    static const uint32_t colour[3] = {0x00000000u, 0xFF20C040u, 0xFFFFFFFFu};
    static const float uv[4][2] = {{-0.5f, -0.5f}, {1.5f, -0.5f}, {1.5f, 1.5f}, {-0.5f, 1.5f}};
    static const uint32_t at[4][2] = {{120, 200}, {290, 200}, {200, 290}, {200, 200}};
    const struct nv2a_pb_backend *b = nv2a_pb_get_backend();
    const uint8_t *g = (const uint8_t *)xbox_GetMemoryOffset() + nv2a_pb_dma_resolve(buf);
    const float o = 1.0f / 32.0f;
    int f = 0, path, bc, a, k;

    for (path = 0; path < 2; path++) {
        if (path)
            nv2a_pb_set_backend(nv2a_pb_cpu_backend());
        for (bc = 0; bc < 3; bc++) {
            int texsrc = bc == 2;
            surf_clear(buf, 640, 480, 0xFF404040u);
            M(0x0304, 0);
            for (a = 0; a < 16; a++)
                M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
            M(0x1B00, TEX_GUEST);
            M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24) | (1u << 4)
                      | (2u << 0) | (texsrc ? 0u : 1u << 3));
            M(0x1B08, 0x00000404u);            /* border u, border v */
            M(0x1B0C, 0x40000000u);
            M(0x1B24, colour[bc]);             /* TEXTURE_BORDER_COLOR(0) */
            M(0x17FC, 8);
            for (k = 0; k < 4; k++)
                vertex_uv(k == 1 || k == 2 ? 300.0f + o : 100.0f + o,
                          k >= 2 ? 300.0f + o : 100.0f + o, 0xFFFFFFFFu, uv[k]);
            M(0x17FC, 0);
            M(0x1B08, 0x00000101u);
            M(0x1B24, 0);
            M(0x1B0C, 0);
            M(0x1760 + 4 * 9, 0x02u);
            for (k = 0; k < 4; k++) {
                uint32_t x = at[k][0], y = at[k][1];
                /* Transparent black keeps its alpha 0 (blend off): all 32
                 * bits there, so opaque black cannot pass for it. */
                uint32_t opaque = k != 3 && colour[bc] == 0 ? 0 : 0xFF000000u;
                uint32_t want = k == 3 ? 0xFFFF8000u
                              : texsrc ? 0xFF00FF00u : colour[bc] | opaque;
                uint32_t got;
                if (texsrc && k != 0 && k != 3)
                    continue;   /* the edge texels there are the checker's own */
                got = (path ? *(const uint32_t *)(g + y * 2560u + x * 4u)
                            : nv2a_pb_d3d11_pixel(buf, x, y)) | opaque;
                fprintf(stderr, "[backend-smoke] %s border 0x%08X%s (%u,%u): 0x%08X"
                        " (want 0x%08X) %s\n", path ? "cpu" : "d3d11", colour[bc],
                        texsrc ? " (texture source)" : "", x, y, got, want,
                        got == want ? "PASS" : "FAIL");
                f += got != want;
            }
        }
    }
    nv2a_pb_set_backend(b);
    return f;
}

/* present.surface=internal follows a live render.scale change (the
 * in-game menu): the swap chain becomes the frame's host size, 1280x960 at
 * 2x, instead of staying at the starting 640x480 behind a scaling blit, and
 * returns at 1x. The request applies after the flip that follows it, so the
 * new size shows from the flip after that. Run last: it leaves the window at
 * whatever the last case sized it to. Returns the failures. */
static int surface_cases(uint32_t buf)
{
    static const struct { unsigned scale, w, h; } want[2] = {
        {2, 1280, 960}, {1, 640, 480},
    };
    struct nv2a_host_opts o = *nv2a_host_opts(), stock = o;
    uint32_t bw, bh;
    int f = 0, i, k;

    o.present_surface = NV2A_PRESENT_SURFACE_INTERNAL;
    nv2a_host_opts_set(&o);
    M(0x0210, buf);
    M(0x0200, 640u << 16);
    M(0x0204, 480u << 16);
    for (i = 0; i < 2; i++) {
        o.render_scale = want[i].scale;
        nv2a_host_opts_request(&o, NV2A_OPT_SCALE);
        for (k = 0; k < 3; k++) {
            M(0x1D90, 0xFF00FF00u);
            M(0x1D94, 0xF0);
            M(0x012C, 0);
            M(0x0130, 0);
        }
        nv2a_pb_d3d11_back_size(&bw, &bh);
        fprintf(stderr, "[backend-smoke] surface internal x%u: back buffer %ux%u (want"
                " %ux%u) %s\n", want[i].scale, bw, bh, want[i].w, want[i].h,
                bw == want[i].w && bh == want[i].h ? "PASS" : "FAIL");
        f += bw != want[i].w || bh != want[i].h;
    }
    nv2a_host_opts_set(&stock);
    return f;
}

int main(int argc, char **argv)
{
    const char *log = recomp_env(RENV_STDIO_LOG);
    int frames = argc > 1 ? atoi(argv[1]) : 180;
    uint32_t buf[2];
    int f, a;

    if (log)
        freopen(log, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!recomp_env(RENV_PB_BACKEND))
        recomp_env_set(RENV_PB_BACKEND, "d3d11");
    fprintf(stderr, "[backend-smoke] %d frames\n", frames);
    int have_reports = 0;
    {
        /* The first free 64 KB below 16 MB (Wine has most of it in use). */
        uintptr_t ra = 0x00010000u;
        MEMORY_BASIC_INFORMATION mi;
        while (ra < 0x01000000u && !have_reports
                && VirtualQuery((void *)ra, &mi, sizeof mi)) {
            uintptr_t lo = ((uintptr_t)mi.BaseAddress + 0xFFFFu) & ~(uintptr_t)0xFFFFu;
            uintptr_t hi = (uintptr_t)mi.BaseAddress + mi.RegionSize;
            if (mi.State == MEM_FREE && hi >= lo + 0x10000u && lo + 0x10000u <= 0x01000000u
                    && VirtualAlloc((void *)lo, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE)) {
                s_report_guest = (uint32_t)lo;
                have_reports = 1;
            }
            ra = hi;
        }
    }
    if (!have_reports) {
        /* ... or a static or heap block that happens to sit there. */
        static uint8_t st[128];
        uint8_t *h = (uint8_t *)malloc(128);
        uintptr_t c = (uintptr_t)st < 0x01000000u ? (uintptr_t)st
                    : h && (uintptr_t)h < 0x01000000u ? (uintptr_t)h : 0;
        fprintf(stderr, "[backend-smoke] static %p heap %p\n", (void *)st, (void *)h);
        if (c) {
            s_report_guest = (uint32_t)((c + 15u) & ~(uintptr_t)15u);
            have_reports = 1;
        }
    }
    if (have_reports)
        fprintf(stderr, "[backend-smoke] reports at 0x%08X\n", s_report_guest);
    else
        fprintf(stderr, "[backend-smoke] no report memory below 16 MB\n");
    if (!make_texture())
        fprintf(stderr, "[backend-smoke] no texture memory at 0x%08X\n", TEX_GUEST);
    {
        static const uint32_t bases[] = {0x01000000u, 0x12000000u, 0x22000000u, 0x32000000u};
        unsigned i;
        for (i = 0; i < sizeof bases / sizeof bases[0]; i++)
            if (VirtualAlloc((void *)(uintptr_t)bases[i], 0x900000, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE))
                break;
        if (i == sizeof bases / sizeof bases[0]) {
            fprintf(stderr, "[backend-smoke] FAIL: no 9 MB block for the surfaces\n");
            return 1;
        }
        s_surf_base = bases[i];
        buf[0] = s_surf_base;
        buf[1] = s_surf_base + 0x12C000u;
        fprintf(stderr, "[backend-smoke] surfaces at 0x%08X\n", s_surf_base);
    }

    for (f = 0; f < frames; f++) {
        float t = (float)(f % 120) / 120.0f;
        float x = 80.0f + 320.0f * t, y = 120.0f;
        uint32_t clear = 0xFF000000u | ((uint32_t)(255 * t) << 16) | 0x30u;
        /* Report 3 across the flip: the last frame pushed an open test into
         * it and did not wait. The flip settles every report still pending,
         * so here, before any poll, it is complete:
         * that frame's count (exact), the frame before's (stale: the
         * address's last count), or visible while the address has no
         * count younger than 250 ms (first). Never TESTINCOMPLETE, which a
         * title reading last frame's test takes as hidden. It runs without a poll between
         * the flip and this read because the smoke is single-threaded: no
         * ack thread ticks on_poll during the Sleep(16). */
        if (have_reports && f > 0) {
            volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)REPORT_GUEST;
            /* First-time: no count read yet, or the last one is older
             * than the tracker keeps (a hitch over 250 ms). */
            int no_hist = (!s_cross_exact && !s_cross_stale)
                       || GetTickCount() - s_cross_hist_ms > 250;
            if (r[15] == 0 && r[14] == CROSS_COUNT(f - 1)) {
                s_cross_exact++;
                s_cross_hist_ms = s_cross_push[(f - 1) & 1];
            } else if (r[15] == 0 && f > 1 && r[14] == CROSS_COUNT(f - 2)) {
                s_cross_stale++;
                s_cross_hist_ms = s_cross_push[(f - 2) & 1];
            } else if (r[15] == 0 && r[14] == OCC_VISIBLE && no_hist) {
                s_cross_first++;
            } else {
                s_cross_bad++;
                fprintf(stderr, "[backend-smoke] frame %d cross-frame report FAIL:"
                        " count %u status 0x%08X after the flip (want %u, %u or"
                        " first-time 0x%X, status 0)\n", f - 1, r[14], r[15],
                        CROSS_COUNT(f - 1), CROSS_COUNT(f - 2), OCC_VISIBLE);
            }
        }

        M(0x0200, 640u << 16);                 /* SURFACE_CLIP_HORIZONTAL */
        M(0x0204, 480u << 16);                 /* SURFACE_CLIP_VERTICAL   */
        M(0x0208, 0x00000128u);                /* SURFACE_FORMAT          */
        M(0x020C, 640u * 4u);                  /* SURFACE_PITCH           */
        M(0x0210, buf[f & 1]);                 /* SURFACE_COLOR_OFFSET    */
        M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
        M(0x1D90, clear);                      /* COLOR_CLEAR_VALUE       */
        M(0x1D8C, 0xFFFFFF00u);                /* ZSTENCIL_CLEAR_VALUE: far */
        M(0x1D94, 0xF3);                       /* CLEAR_SURFACE: colour, Z, stencil 0 */

        for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        M(0x17FC, 8);                          /* BEGIN_END: QUADS */
        vertex(x,          y,          0xFFFF0000u);
        vertex(x + 160.0f, y,          0xFF00FF00u);
        vertex(x + 160.0f, y + 240.0f, 0xFF0000FFu);
        vertex(x,          y + 240.0f, 0xFFFFFFFFu);
        M(0x17FC, 0);                          /* BEGIN_END: end */

        /* Stage 0: the swizzled texture; a textured quad on the right,
         * diffuse white so the texels show unmodified. */
        M(0x1B00, TEX_GUEST);                  /* TEXTURE_OFFSET(0)  */
        M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
                  | (1u << 4) | (2u << 0));    /* FORMAT: A8R8G8B8, 2D, 64x64 */
        M(0x1B08, 0x00000101u);                /* ADDRESS: wrap u, wrap v */
        M(0x1B0C, 0x40000000u);                /* CONTROL0: enable */
        M(0x1760 + 4 * 9, 0x22u);              /* attr9: float2 */
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
            vertex_uv(440.0f, 40.0f,  0xFFFFFFFFu, uv[0]);
            vertex_uv(620.0f, 40.0f,  0xFFFFFFFFu, uv[1]);
            vertex_uv(620.0f, 220.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(440.0f, 220.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);

        /* The YUY2 image, linear: size from IMAGE_RECT, pitch from CONTROL1,
         * and texture coordinates in texels. */
        M(0x1B00, TEX_GUEST + (4u << (2 * TEX_LOG2)));
        M(0x1B04, (0x24u << 8) | (1u << 4) | (2u << 0)); /* LC_CR8YB8CB8YA8 */
        M(0x1B08, 0x00000303u);                /* clamp */
        M(0x1B10, 128u << 16);                 /* CONTROL1: pitch */
        M(0x1B1C, (64u << 16) | 32u);          /* IMAGE_RECT */
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 32}, {0, 32}};
            vertex_uv(440.0f, 260.0f, 0xFFFFFFFFu, uv[0]);
            vertex_uv(620.0f, 260.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(620.0f, 350.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(440.0f, 350.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);

        /* Register combiners: a title's control word and final combiner
         * (ctl 0x11104 and fcw 0x130C0300/0x00001C80, as the XDK programs
         * them for a 2D menu: four stages, mux on R0.a's MSB, per-stage constants,
         * final = fog.a * R0 + (1 - fog.a) * fog, alpha R0.a, V1+R0 clamped)
         * with four stages of our own that exercise them:
         *   0: R0 = T0 * V0 (rgb and alpha)
         *   1: R1 = mux(R0 * 1, C0[1] * 1): R0.a >= 0.5 picks C0[1]
         *   2: R0 = R1 * C1[2], output x2 (C1[2] = 0.5 so the colour holds)
         *   3: R0 = R0 * 1
         * The quad's diffuse alpha goes 0 -> 1 left to right, so the left
         * half shows the checker and the right half magenta. */
        M(0x1B00, TEX_GUEST);
        M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
                  | (1u << 4) | (2u << 0));
        M(0x1B08, 0x00000101u);
        M(0x1E70, 1);                          /* SHADER_STAGE_PROGRAM: T0 2D */
        {
            static const uint32_t cicw[4] = {0x08040000u, 0x0C200120u, 0x0D020000u, 0x0C200000u};
            static const uint32_t cocw[4] = {0x000000C0u, 0x00004D00u, 0x000100C0u, 0x000000C0u};
            static const uint32_t aicw[4] = {0x18140000u, 0x1C200000u, 0x1D200000u, 0x1C200000u};
            static const uint32_t aocw[4] = {0x000000C0u, 0x00000D00u, 0x000000C0u, 0x000000C0u};
            static const float uv[4][2] = {{0, 0}, {4, 0}, {4, 1}, {0, 1}};
            int s;
            for (s = 0; s < 4; s++) {
                M(0x0AC0 + 4 * s, cicw[s]);    /* COMBINER_COLOR_ICW */
                M(0x1E40 + 4 * s, cocw[s]);    /* COMBINER_COLOR_OCW */
                M(0x0260 + 4 * s, aicw[s]);    /* COMBINER_ALPHA_ICW */
                M(0x0AA0 + 4 * s, aocw[s]);    /* COMBINER_ALPHA_OCW */
                M(0x0A60 + 4 * s, s == 1 ? 0xFFFF00FFu : 0xFF000000u); /* FACTOR0 */
                M(0x0A80 + 4 * s, s == 2 ? 0x80808080u : 0xFF000000u); /* FACTOR1 */
            }
            M(0x02A8, 0xFF203040u);            /* FOG_COLOR */
            M(0x0288, 0x130C0300u);            /* SPECULAR_FOG_CW0 */
            M(0x028C, 0x00001C80u);            /* SPECULAR_FOG_CW1 */
            M(0x1E60, 0x00011104u);            /* COMBINER_CONTROL */
            M(0x17FC, 8);
            vertex_uv(20.0f,  380.0f, 0x00FFFFFFu, uv[0]);
            vertex_uv(420.0f, 380.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(420.0f, 470.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(20.0f,  470.0f, 0x00FFFFFFu, uv[3]);
            M(0x17FC, 0);
            M(0x1E60, 0);                      /* combiners off: 0 stages */
        }

        M(0x1760 + 4 * 9, 0x02u);              /* attr9 off again */
        M(0x1B0C, 0);                          /* stage 0 off */

        /* Depth: LESS with writes on. The near green quad goes first, then
         * a far red one overlapping it, which must stay behind: green wins
         * where they overlap. Then a blue quad between them in depth with
         * writes off and GREATER: it shows only over the green one. */
        M(0x1E70, 0);                          /* no texture stages */
        M(0x030C, 1);                          /* DEPTH_TEST_ENABLE */
        M(0x0354, 0x201);                      /* DEPTH_FUNC: LESS */
        M(0x035C, 1);                          /* DEPTH_MASK: write */
        M(0x17FC, 8);
        vertex_z(440.0f, 370.0f, 4194304.0f, 0xFF00C000u);   /* z 0.25 */
        vertex_z(560.0f, 370.0f, 4194304.0f, 0xFF00C000u);
        vertex_z(560.0f, 450.0f, 4194304.0f, 0xFF00C000u);
        vertex_z(440.0f, 450.0f, 4194304.0f, 0xFF00C000u);
        vertex_z(500.0f, 390.0f, 12582912.0f, 0xFFE00000u);  /* z 0.75 */
        vertex_z(620.0f, 390.0f, 12582912.0f, 0xFFE00000u);
        vertex_z(620.0f, 470.0f, 12582912.0f, 0xFFE00000u);
        vertex_z(500.0f, 470.0f, 12582912.0f, 0xFFE00000u);
        M(0x17FC, 0);
        M(0x0354, 0x204);                      /* DEPTH_FUNC: GREATER */
        M(0x035C, 0);                          /* no writes */
        M(0x17FC, 8);
        vertex_z(470.0f, 380.0f, 8388608.0f, 0xFF2040FFu);   /* z 0.5 */
        vertex_z(600.0f, 380.0f, 8388608.0f, 0xFF2040FFu);
        vertex_z(600.0f, 420.0f, 8388608.0f, 0xFF2040FFu);
        vertex_z(470.0f, 420.0f, 8388608.0f, 0xFF2040FFu);
        M(0x17FC, 0);
        /* Two visibility tests, colour writes off: a 100x60 quad at z 0.75
         * wholly behind the green one (count 0) and a 10x10 one at z 0.5 in
         * the open (count 100). The reports arrive a little later: the
         * previous frame's are printed at the top of the next. A report
         * settled at the flip before its result came reads the address's
         * last count, the same here, or visible while there is none. */
        if (have_reports) {
            volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)REPORT_GUEST;
            /* Spin on the status as a title would, ticking on_poll the way
             * the ack thread does, at most 300 ms, then once more. The
             * backend's 250 ms fallback has fired by then (one thread, so
             * nothing else holds a report back): a report still pending is
             * one the backend lost, and a title spinning on it would hang. */
            if (f > 0) {
                const struct nv2a_pb_backend *b = nv2a_pb_get_backend();
                DWORD t0 = GetTickCount();
                int spins = 0;
                uint32_t reuse_first = 0xFFFFFFFFu;
                /* No history yet, or none younger than 250 ms (judged by
                 * report 3's): a settle reads visible (first-time). */
                int fresh = (!s_cross_exact && !s_cross_stale)
                         || GetTickCount() - s_cross_hist_ms > 250;
                int bad, late;
                while ((r[3] || r[7] || r[11] || r[15]) && GetTickCount() - t0 < 300) {
                    if (b->on_poll)
                        b->on_poll();
                    if (reuse_first == 0xFFFFFFFFu && !r[11])
                        reuse_first = r[10];   /* the first count seen done */
                    spins++;
                }
                if (b->on_poll)
                    b->on_poll();
                if (reuse_first == 0xFFFFFFFFu && !r[11])
                    reuse_first = r[10];
                /* Wrong: a report done with the wrong count. */
                bad = (!r[3] && r[2] != 0 && !(fresh && r[2] == OCC_VISIBLE))
                   || (!r[7] && r[6] != 100 && !(fresh && r[6] == OCC_VISIBLE))
                   || (reuse_first != 0xFFFFFFFFu && reuse_first != 0
                       && !(fresh && reuse_first == OCC_VISIBLE));
                late = r[3] || r[7] || r[11] || r[15];
                if (bad)
                    s_vis_failures++;
                if (late)
                    s_vis_late++;
                if (f < 4 || f == frames - 1 || bad || late)
                    fprintf(stderr, "[backend-smoke] frame %d visibility%s: occluded"
                            " count %u status 0x%08X, open count %u status 0x%08X,"
                            " reused slot first count %u status 0x%08X, cross-frame"
                            " count %u status 0x%08X (want 0, 100, 0, %u)"
                            " (%d polls, %lu ms)\n", f - 1,
                            bad ? " FAIL" : late ? " LOST" : "",
                            r[2], r[3], r[6], r[7], reuse_first, r[11],
                            r[14], r[15], CROSS_COUNT(f - 1), spins, GetTickCount() - t0);
            }
            M(0x0358, 0);                      /* COLOR_MASK: none */
            M(0x0354, 0x201);                  /* DEPTH_FUNC: LESS */
            vis_test(0, 450.0f, 380.0f, 550.0f, 440.0f, 12582912.0f);
            vis_test(1, 300.0f, 300.0f, 310.0f, 310.0f, 8388608.0f);
            /* A reused slot, as the XDK recycles them: an open test (100)
             * then, with the first still pending, an occluded one (0) into
             * the same report. Only the second may ever show. */
            vis_test(2, 300.0f, 300.0f, 310.0f, 310.0f, 8388608.0f);
            vis_test(2, 450.0f, 380.0f, 550.0f, 440.0f, 12582912.0f);
            /* Report 3: an open test, not waited for (see the frame top);
             * 10x10 on even frames, 10x20 on odd ones (CROSS_COUNT). */
            vis_test(3, 300.0f, 300.0f, 310.0f, f & 1 ? 320.0f : 310.0f, 8388608.0f);
            s_cross_push[f & 1] = GetTickCount();
            M(0x0358, 0x01010101u);
        }
        M(0x030C, 0);                          /* depth test off */

        /* Stencil: a stencil-only quad (colour writes off) marks y 200..300
         * with 1, then a band x 20..75, y 150..370, drawn EQUAL 1 in yellow
         * and again NOTEQUAL 1 in cyan: yellow only over the mark, cyan
         * around it. */
        M(0x032C, 1);                          /* STENCIL_TEST_ENABLE */
        M(0x0360, 0xFF);                       /* STENCIL_MASK (write) */
        M(0x036C, 0xFF);                       /* STENCIL_FUNC_MASK */
        M(0x0368, 1);                          /* STENCIL_FUNC_REF */
        M(0x0364, 0x207);                      /* STENCIL_FUNC: ALWAYS */
        M(0x0370, 0x1E00);                     /* OP_FAIL: KEEP */
        M(0x0374, 0x1E00);                     /* OP_ZFAIL: KEEP */
        M(0x0378, 0x1E01);                     /* OP_ZPASS: REPLACE */
        M(0x0358, 0);                          /* COLOR_MASK: none */
        M(0x17FC, 8);
        vertex(20.0f, 200.0f, 0xFFFFFFFFu);
        vertex(75.0f, 200.0f, 0xFFFFFFFFu);
        vertex(75.0f, 300.0f, 0xFFFFFFFFu);
        vertex(20.0f, 300.0f, 0xFFFFFFFFu);
        M(0x17FC, 0);
        M(0x0358, 0x01010101u);                /* COLOR_MASK: all */
        M(0x0378, 0x1E00);                     /* OP_ZPASS: KEEP */
        M(0x0364, 0x202);                      /* STENCIL_FUNC: EQUAL */
        M(0x17FC, 8);
        vertex(20.0f, 150.0f, 0xFFFFFF00u);
        vertex(75.0f, 150.0f, 0xFFFFFF00u);
        vertex(75.0f, 370.0f, 0xFFFFFF00u);
        vertex(20.0f, 370.0f, 0xFFFFFF00u);
        M(0x17FC, 0);
        M(0x0364, 0x205);                      /* STENCIL_FUNC: NOTEQUAL */
        M(0x17FC, 8);
        vertex(20.0f, 150.0f, 0xFF00FFFFu);
        vertex(75.0f, 150.0f, 0xFF00FFFFu);
        vertex(75.0f, 370.0f, 0xFF00FFFFu);
        vertex(20.0f, 370.0f, 0xFF00FFFFu);
        M(0x17FC, 0);
        M(0x032C, 0);                          /* stencil test off */
        M(0x1E70, 1);                          /* stage 0 2D again */

        /* Render to texture. The top-left inset samples the offscreen
         * surface (as a linear A8R8G8B8 texture at its address, texel
         * coordinates) that the previous frame drew below: a backend that
         * decodes guest memory instead shows whatever is there. */
        M(0x1B00, OFFSCREEN);
        M(0x1B04, (0x12u << 8) | (1u << 4) | (2u << 0)); /* LU_IMAGE_A8R8G8B8 */
        M(0x1B08, 0x00000303u);
        M(0x1B0C, 0x40000000u);
        M(0x1B10, (320u * 4u) << 16);
        M(0x1B1C, (320u << 16) | 240u);
        M(0x1760 + 4 * 9, 0x22u);
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {320, 0}, {320, 240}, {0, 240}};
            vertex_uv(20.0f,  20.0f,  0xFFFFFFFFu, uv[0]);
            vertex_uv(180.0f, 20.0f,  0xFFFFFFFFu, uv[1]);
            vertex_uv(180.0f, 140.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(20.0f,  140.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);

        /* A palettized (P8) texture, and next to it one in a format with no
         * decoder (0x2A, a swizzled depth format), which must come out
         * magenta and be logged once. */
        {
            static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            int q;
            for (q = 0; q < 2; q++) {
                float x0 = q ? 300.0f : 200.0f;
                M(0x1B00, P8_GUEST);
                M(0x1B04, ((q ? 0x2Au : 0x0Bu) << 8) | (4u << 20) | (4u << 24)
                          | (1u << 4) | (2u << 0));
                M(0x1B08, 0x00000101u);
                M(0x1B20, PAL_GUEST);          /* TEXTURE_PALETTE: 256 entries */
                /* The P8 one magnified with TEXTURE_FILTER MIN/MAG linear:
                 * soft band edges; everything else stays nearest. */
                M(0x1B14, q ? 0x01010000u : 0x02020000u);
                M(0x17FC, 8);
                vertex_uv(x0,         20.0f,  0xFFFFFFFFu, uv[0]);
                vertex_uv(x0 + 80.0f, 20.0f,  0xFFFFFFFFu, uv[1]);
                vertex_uv(x0 + 80.0f, 100.0f, 0xFFFFFFFFu, uv[2]);
                vertex_uv(x0,         100.0f, 0xFFFFFFFFu, uv[3]);
                M(0x17FC, 0);
            }
            M(0x1B14, 0x01010000u);
        }
        /* A vertex-program batch: mov oPos, v0; mov oD0, v3; mov oT0, v9,
         * hand-encoded, with the checker texture. It draws the same as a
         * pass-through quad would (stream output and the CPU interpreter
         * must agree). */
        {
            static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            static const uint32_t outs[3][2] = {{0, 0}, {3, 3}, {9, 9}}; /* {o, v} */
            int k;
            M(0x1E9C, 0);                      /* PROGRAM_LOAD: slot 0 */
            for (k = 0; k < 3; k++) {
                M(0x0B00 + 16 * k + 0, 0);
                M(0x0B00 + 16 * k + 4, (1u << 21) | (outs[k][1] << 9) | 0x1Bu);
                M(0x0B00 + 16 * k + 8, 2u << 26);          /* A = v */
                M(0x0B00 + 16 * k + 12, (0xFu << 12) | (1u << 11)
                                        | (outs[k][0] << 3) | (k == 2 ? 1u : 0u));
            }
            M(0x1EA0, 0);                      /* PROGRAM_START */
            M(0x1E94, 2);                      /* EXECUTION_MODE: program */
            M(0x1B00, TEX_GUEST);
            M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
                      | (1u << 4) | (2u << 0));
            M(0x1B08, 0x00000101u);
            M(0x17FC, 8);
            vertex_uv(300.0f, 110.0f, 0xFFFFFFFFu, uv[0]);
            vertex_uv(380.0f, 110.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(380.0f, 190.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(300.0f, 190.0f, 0xFFFFFFFFu, uv[3]);
            M(0x17FC, 0);
            M(0x1E94, 0);                      /* fixed function again */
        }
        /* The mipmapped texture with MIN nearest-level (TEXTURE_FILTER MIN 3)
         * at 32, 16 and 8 pixels: red (level 0), green (1), blue (2). With
         * RECOMP_D3D11_NO_MIPS=1 all three are red. CONTROL0 allows levels
         * 0..5 (MAX_LOD_CLAMP 5). */
        {
            static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            static const float px[3][2] = {{220, 150}, {260, 150}, {284, 150}};
            int k;
            M(0x1B00, MIP_GUEST);
            M(0x1B04, (0x06u << 8) | (6u << 16) | (5u << 20) | (5u << 24)
                      | (1u << 4) | (2u << 0));
            M(0x1B08, 0x00000303u);
            M(0x1B0C, 0x40000000u | (5u << 6));
            M(0x1B14, 0x01030000u);
            for (k = 0; k < 3; k++) {
                float d = (float)(32 >> k), x0 = px[k][0], y0 = px[k][1];
                M(0x17FC, 8);
                vertex_uv(x0,     y0,     0xFFFFFFFFu, uv[0]);
                vertex_uv(x0 + d, y0,     0xFFFFFFFFu, uv[1]);
                vertex_uv(x0 + d, y0 + d, 0xFFFFFFFFu, uv[2]);
                vertex_uv(x0,     y0 + d, 0xFFFFFFFFu, uv[3]);
                M(0x17FC, 0);
            }
            M(0x1B14, 0x01010000u);
        }
        /* A post pass reading the surface it draws into (linear A8R8G8B8 at
         * the colour offset, texel coordinates): a quarter-size copy of the
         * frame so far, bottom right. Bound straight from guest memory, it
         * is black. */
        M(0x1B00, buf[f & 1]);
        M(0x1B04, (0x12u << 8) | (1u << 4) | (2u << 0));
        M(0x1B08, 0x00000303u);
        M(0x1B0C, 0x40000000u);
        M(0x1B10, (640u * 4u) << 16);
        M(0x1B1C, (640u << 16) | 480u);
        M(0x1760 + 4 * 9, 0x22u);
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {640, 0}, {640, 480}, {0, 480}};
            vertex_uv(460.0f, 360.0f, 0xFFFFFFFFu, uv[0]);
            vertex_uv(620.0f, 360.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(620.0f, 480.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(460.0f, 480.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);
        M(0x1760 + 4 * 9, 0x02u);
        M(0x1B0C, 0);

        /* ... and the offscreen surface itself, drawn last in the frame the
         * way a title renders a glow or shadow target: the flip must still
         * present the 640x480 back buffer, not this. A dark teal clear with
         * an orange quad sliding across it. */
        M(0x0200, 320u << 16);
        M(0x0204, 240u << 16);
        M(0x020C, 320u * 4u);
        M(0x0210, OFFSCREEN);
        M(0x1D90, 0xFF004040u);
        M(0x1D94, 0xF0);                       /* colour only */
        M(0x17FC, 8);
        vertex(20.0f + 200.0f * t, 60.0f,  0xFFFF8000u);
        vertex(120.0f + 200.0f * t, 60.0f, 0xFFFF8000u);
        vertex(120.0f + 200.0f * t, 180.0f, 0xFFFF8000u);
        vertex(20.0f + 200.0f * t, 180.0f, 0xFFFF8000u);
        M(0x17FC, 0);

        /* next buffer, as the XDK does -- except the first flip, with the
         * offscreen target still bound: named at a flip, it must still not
         * be presented over the back buffers. */
        M(0x0210, f == 0 ? OFFSCREEN : buf[(f + 1) & 1]);
        M(0x0130, 0);                          /* FLIP_STALL */
        Sleep(16);
    }
    if (have_reports && frames > 0) {
        /* The last frame's reports: spin and check them too (report 2 was
         * superseded by the occluded test, report 3 holds an open one, exact
         * or settled stale at the last flip). */
        volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)REPORT_GUEST;
        const struct nv2a_pb_backend *b = nv2a_pb_get_backend();
        DWORD t0 = GetTickCount();
        while ((r[3] || r[7] || r[11] || r[15]) && GetTickCount() - t0 < 300)
            if (b->on_poll)
                b->on_poll();
        if (b->on_poll)
            b->on_poll();
        if (r[3] || r[7] || r[11] || r[15]) {
            s_vis_late++;
            fprintf(stderr, "[backend-smoke] last frame visibility LOST: status"
                    " 0x%08X 0x%08X 0x%08X 0x%08X\n", r[3], r[7], r[11], r[15]);
        } else if (r[2] != 0 || r[6] != 100 || r[10] != 0
                   || (r[14] != CROSS_COUNT(frames - 1)
                       && (frames < 2 || r[14] != CROSS_COUNT(frames - 2)))) {
            s_vis_failures++;
            fprintf(stderr, "[backend-smoke] last frame visibility FAIL: counts"
                    " %u %u %u %u (want 0, 100, 0, %u)\n", r[2], r[6], r[10], r[14],
                    CROSS_COUNT(frames - 1));
        }
    }
    if (have_reports) {
        fprintf(stderr, "[backend-smoke] visibility over %d frames and the last:"
                " %d wrong, %d lost; cross-frame report complete after the flip"
                " in %d of %d (exact %d, stale %d, first %d)\n",
                frames > 0 ? frames - 1 : 0, s_vis_failures, s_vis_late,
                s_cross_exact + s_cross_stale + s_cross_first,
                frames > 0 ? frames - 1 : 0, s_cross_exact, s_cross_stale,
                s_cross_first);
        if (s_vis_failures || s_vis_late) {
            fprintf(stderr, "[backend-smoke] FAIL: %d frames with a wrong visibility"
                    " result, %d frames with a report the backend lost\n",
                    s_vis_failures, s_vis_late);
            return 1;
        }
        if (s_cross_bad) {
            fprintf(stderr, "[backend-smoke] FAIL: %d frames where report 3 was not"
                    " complete after the flip that ended its frame\n", s_cross_bad);
            return 2;
        }
    }
    if (clip_cases(buf)) {
        fprintf(stderr, "[backend-smoke] FAIL: clip extents (clear, present crop)\n");
        return 5;
    }
    if (mode_cases(buf[0])) {
        fprintf(stderr, "[backend-smoke] FAIL: display mode's frame (crop, grow)\n");
        return 9;
    }
    if (alias_cases(buf)) {
        fprintf(stderr, "[backend-smoke] FAIL: render-target ownership\n");
        return 3;
    }
    if (index_cases(buf)) {
        fprintf(stderr, "[backend-smoke] FAIL: index batches\n");
        return 4;
    }
    if (cpu_clip_cases(buf[0])) {
        fprintf(stderr, "[backend-smoke] FAIL: CPU clip clear or clear rect\n");
        return 6;
    }
    if (edge_cases(buf)) {
        fprintf(stderr, "[backend-smoke] FAIL: 0.53125-offset edges\n");
        return 7;
    }
    if (border_cases(buf[0])) {
        fprintf(stderr, "[backend-smoke] FAIL: texture border colour\n");
        return 10;
    }
    if (clipplane_cases(buf[0])) {
        fprintf(stderr, "[backend-smoke] FAIL: CLIPPLANE texture-shader kills\n");
        return 11;
    }
    if (surface_cases(buf[0])) {
        fprintf(stderr, "[backend-smoke] FAIL: present.surface=internal scale change\n");
        return 8;
    }
    fprintf(stderr, "[backend-smoke] done\n");
    return 0;
}
