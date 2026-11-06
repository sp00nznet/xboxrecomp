/* Texture filtering on the software rasteriser: nearest (the default),
 * RECOMP_TEX_BILINEAR=1, and RECOMP_TEX_ANISO=8 or 16.
 *
 * The switches are read once per process, so CTest runs this binary once per
 * mode and passes the mode as argv[1]: nearest, bilinear, aniso8, aniso16.
 * argv[2] picks which sampler draws: 2d (raster_triangle, pixel positions),
 * xf (the perspective path, no combiners) or rc (the perspective path through
 * the register combiners).
 *
 * Two quads, 64x64 pixels, each textured through a one-row linear texture:
 *
 *   ramp     2x1 texels, black then white, clamped, stretched over the quad
 *            (u 0..2). Nearest is a hard step at the middle. Filtered, the
 *            middle pixel is the blend of its two texels and the row rises
 *            smoothly from black to white.
 *   stripes  64x1 texels, odd ones white, even ones black, u 0..192: three
 *            texels to a pixel along x, none along y. Nearest and bilinear
 *            alias into an alternating black and white row (a pixel centre
 *            lands on a texel centre, so the blend is that one texel).
 *            Anisotropic filtering averages the three texels each pixel
 *            covers, so every pixel comes out near mid-grey.
 *
 * Harness as tests/nv2a_raster_batch: no back end is registered, so the
 * software path draws. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 64
#define H 64
#define COLOR1_AT 0x10000u
#define COLOR2_AT 0x20000u
#define TEX1_AT   0x30000u
#define TEX2_AT   0x31000u
#define POS_AT    0x40000u
#define UV_AT     0x41000u
#define DIFF_AT   0x42000u
#define MEM_BYTES 0x50000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void put_vertex(uint32_t n, float x, float y, float u, float v)
{
    float p[3] = { x, y, 0.0f }, t[2] = { u, v };
    uint32_t white = 0xFFFFFFFFu;
    memcpy(s_mem + POS_AT + 12u * n, p, 12);
    memcpy(s_mem + UV_AT + 8u * n, t, 8);
    memcpy(s_mem + DIFF_AT + 4u * n, &white, 4);
}

/* A 64x64 quad (two triangles) with u running 0..u1 and v 0..1 across it,
 * drawn on the surface at `color_at` through the texture at `tex_at`. */
static void draw_quad(uint32_t color_at, uint32_t tex_at, uint32_t tex_w,
                      float u1, uint32_t wrap)
{
    m(0x0210, color_at);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x1B00, tex_at);                             /* TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16));          /* FORMAT: LIN_A8R8G8B8, 1 level */
    m(0x1B08, wrap | (wrap << 8));                 /* ADDRESS: u and v */
    m(0x1B10, (tex_w * 4u) << 16);                 /* CONTROL1: pitch */
    m(0x1B1C, (tex_w << 16) | 1u);                 /* IMAGE_RECT: tex_w x 1 */
    put_vertex(0, 0,  0,  0,  0);
    put_vertex(1, W,  0,  u1, 0);
    put_vertex(2, W,  H,  u1, 1);
    put_vertex(3, 0,  0,  0,  0);
    put_vertex(4, W,  H,  u1, 1);
    put_vertex(5, 0,  H,  0,  1);
    m(0x17FC, 5);                                  /* BEGIN_END: triangles */
    m(0x1810, (6u - 1u) << 24);                    /* DRAW_ARRAYS: 6 from 0 */
    m(0x17FC, 0);
}

static int gray(uint32_t color_at, int x, int y)
{
    uint32_t p;
    memcpy(&p, s_mem + color_at + 4u * (uint32_t)(y * W + x), 4);
    return (int)(p & 0xFFu);
}

int main(int argc, char **argv)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const char *mode = argc > 1 ? argv[1] : "nearest";
    const char *variant = argc > 2 ? argv[2] : "2d";
    int nearest = !strcmp(mode, "nearest");
    int aniso = !strncmp(mode, "aniso", 5);
    uint32_t black = 0xFF000000u, white = 0xFFFFFFFFu;
    int x, lo = 255, hi = 0, mid;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=1");
#else
    setenv("RECOMP_RASTER_THREADS", "1", 1);
#endif

    memcpy(s_mem + TEX1_AT,     &black, 4);
    memcpy(s_mem + TEX1_AT + 4, &white, 4);
    for (x = 0; x < 64; x++)                     /* odd texels white */
        memcpy(s_mem + TEX2_AT + 4 * x, (x & 1) ? &white : &black, 4);

    m(0x0208, 0x0008);                           /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);                /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);                /* CLIP_VERTICAL */
    m(0x020C, W * 4);                            /* SET_SURFACE_PITCH */
    if (strcmp(variant, "2d") != 0) {
        /* A composite matrix routes the batch through the perspective path
         * (raster_xf_triangle); without one a batch of pixel positions is
         * drawn by raster_triangle. */
        for (x = 0; x < 16; x++)
            mf(0x0680 + 4 * x, I[x]);            /* composite: positions are pixels */
    }
    if (!strcmp(variant, "rc")) {
        /* Register combiners passing stage 0 straight through: the final
         * combiner's D is T0, alpha 1. This is the sampler the games use. */
        m(0x1E60, 0);                            /* COMBINER_CONTROL: no general stages */
        m(0x1E70, 1);                            /* stage 0: 2D texture */
        m(0x0288, 8u);                           /* final: D = T0 */
        m(0x028C, 0x20u << 8);                   /* final alpha: 1 */
    }
    m(0x1E94, 0);                                /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
    m(0x1744, UV_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));        /* attr 9: float2 */

    draw_quad(COLOR1_AT, TEX1_AT, 2, 2.0f, 3);       /* ramp: clamp */
    draw_quad(COLOR2_AT, TEX2_AT, 64, 192.0f, 1);    /* stripes: wrap */

    /* ramp, row 32 */
    mid = gray(COLOR1_AT, 32, 32);
    printf("ramp: x=0 %d  x=31 %d  x=32 %d  x=33 %d  x=63 %d\n",
           gray(COLOR1_AT, 0, 32), gray(COLOR1_AT, 31, 32), mid,
           gray(COLOR1_AT, 33, 32), gray(COLOR1_AT, 63, 32));
    if (nearest) {
        for (x = 0; x < W; x++)
            CHECK(gray(COLOR1_AT, x, 32) == (x < 32 ? 0 : 255),
                  "ramp x=%d is %d, nearest wants %d", x, gray(COLOR1_AT, x, 32), x < 32 ? 0 : 255);
    } else {
        /* Pixel 32's centre is half a texel past the boundary: 0.5156 white. */
        CHECK(mid >= 126 && mid <= 138, "ramp middle %d, want about 132", mid);
        CHECK(gray(COLOR1_AT, 0, 32) <= 2 && gray(COLOR1_AT, 63, 32) >= 253,
              "ramp ends %d %d, want 0 and 255", gray(COLOR1_AT, 0, 32), gray(COLOR1_AT, 63, 32));
        for (x = 1; x < W; x++)
            CHECK(gray(COLOR1_AT, x, 32) >= gray(COLOR1_AT, x - 1, 32),
                  "ramp falls at x=%d (%d after %d)", x, gray(COLOR1_AT, x, 32), gray(COLOR1_AT, x - 1, 32));
    }

    /* stripes, row 32 */
    for (x = 0; x < W; x++) {
        int g = gray(COLOR2_AT, x, 32);
        if (g < lo) lo = g;
        if (g > hi) hi = g;
    }
    printf("stripes: min %d max %d\n", lo, hi);
    if (aniso) {
        /* Three texels per pixel averaged: near mid-grey everywhere. */
        CHECK(lo >= 127 - 48 && hi <= 128 + 48,
              "aniso stripes span %d..%d, want within 79..176", lo, hi);
    } else {
        /* Pure black and white, alternating (a blend never leaves them by more
         * than the float rounding of the texture coordinate). */
        for (x = 0; x < W; x++) {
            int want = (x & 1) ? 0 : 255, g = gray(COLOR2_AT, x, 32);
            CHECK(g >= want - 3 && g <= want + 3,
                  "stripes x=%d is %d, want %d", x, g, want);
        }
    }

    printf("%s %s: %s\n", mode, variant, s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
