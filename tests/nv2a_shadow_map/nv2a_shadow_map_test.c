/* A shadow map on the software rasteriser: a depth buffer drawn with no
 * colour, then sampled as a depth texture with SET_SHADOW_COMPARE_FUNC.
 *
 *   - a quad over the left half of a 16x16 D24S8 depth buffer at depth 1000,
 *     colour writes off and no colour surface, as a title draws a light's
 *     shadow map;
 *   - a light pass whose stage 0 reads that buffer as LIN_D24S8 with r = 2000
 *     everywhere and the compare LESS (r < depth: lit): black on the left,
 *     where the caster is nearer, white on the right, where the clear is;
 *   - with a linear filter, the pixel on the edge passes half;
 *   - a depth texture the rasteriser did not draw is read from guest memory.
 *
 * The rasteriser keeps its depth buffers on the host, so before this the
 * stage had no format to read and sampled white: every light lit everything. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 16
#define H 16
#define COLOR_AT  0x10000u
#define ZETA_AT   0x20000u
#define TEX_AT    0x30000u
#define POS_AT    0x40000u
#define TC_AT     0x41000u
#define DIFF_AT   0x42000u
#define COPY_AT   0x44000u
#define MEM_BYTES 0x50000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
/* Memory model 2 is off here. */
int xbox_mem2_phys_to_guest(uint32_t pa, uint32_t *va) { (void)pa; (void)va; return 0; }
int xbox_mem2_arena_has(uint32_t va, uint32_t bytes) { (void)va; (void)bytes; return 0; }
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
void xbox_RegisterTitleEvent(uint32_t guest_va) { (void)guest_va; }
void xbox_SetTitleEvent(uint32_t guest_va) { (void)guest_va; }
int xbox_Nv2aPgraphRaise(uint32_t addr, uint32_t data) { (void)addr; (void)data; return 0; }
int xbox_Nv2aPgraphWaitAck(uint32_t timeout_ms) { (void)timeout_ms; return 1; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* A quad from x0 to x1 over the full height at depth z, texture coordinates
 * (x + du, y, r): linear textures are addressed in texels. */
static void quad(float x0, float x1, float z, float du, float r)
{
    const float c[6][2] = { {x0, 0}, {x1, 0}, {x1, H}, {x0, 0}, {x1, H}, {x0, H} };
    uint32_t i, white = 0xFFFFFFFFu;

    for (i = 0; i < 6; i++) {
        float p[3] = { c[i][0], c[i][1], z }, t[3] = { c[i][0] + du, c[i][1], r };
        memcpy(s_mem + POS_AT + 12u * i, p, 12);
        memcpy(s_mem + TC_AT + 12u * i, t, 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
    }
    m(0x17FC, 5);                                  /* BEGIN_END: triangles */
    m(0x1810, 5u << 24);                           /* DRAW_ARRAYS: 6 from 0 */
    m(0x17FC, 0);
}

static int gray(int x, int y)
{
    uint32_t p;
    memcpy(&p, s_mem + COLOR_AT + 4u * (uint32_t)(y * W + x), 4);
    return (int)(p & 0xFFu);
}

/* The light pass over the whole surface, stage 0 reading `tex_at`. */
static void light(uint32_t tex_at, uint32_t mag, float du)
{
    memset(s_mem + COLOR_AT, 0x80, 4u * W * H);
    m(0x0210, COLOR_AT);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x0358, 0x01010101u);                        /* SET_COLOR_MASK: all */
    m(0x030C, 0);                                  /* DEPTH_TEST_ENABLE: off */
    m(0x035C, 0);                                  /* DEPTH_MASK: off */
    m(0x1B00, tex_at);                             /* TEXTURE_OFFSET */
    m(0x1B04, (0x2Eu << 8) | (1u << 16));          /* FORMAT: LIN_D24S8, 1 level */
    m(0x1B08, 3u | (3u << 8));                     /* ADDRESS: clamp */
    m(0x1B10, (W * 4u) << 16);                     /* CONTROL1: pitch */
    m(0x1B14, (mag << 24) | (1u << 16));           /* FILTER */
    m(0x1B1C, ((uint32_t)W << 16) | H);            /* IMAGE_RECT */
    m(0x1E6C, 4);                                  /* SHADOW_COMPARE_FUNC: LESS */
    quad(0, W, 0.0f, du, 2000.0f);
}

/* The depth buffer read as a LIN_A8R8G8B8 texture and drawn into COPY_AT, as a
 * title copies its shadow map: the words it holds, depth in the top 24 bits. */
static void copy_depths(void)
{
    memset(s_mem + COPY_AT, 0x80, 4u * W * H);
    m(0x0210, COPY_AT);                            /* SET_SURFACE_COLOR_OFFSET */
    m(0x0358, 0x01010101u);                        /* SET_COLOR_MASK: all */
    m(0x030C, 0);                                  /* DEPTH_TEST_ENABLE: off */
    m(0x035C, 0);                                  /* DEPTH_MASK: off */
    m(0x1B00, ZETA_AT);                            /* TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16));          /* FORMAT: LIN_A8R8G8B8, 1 level */
    m(0x1B08, 3u | (3u << 8));                     /* ADDRESS: clamp */
    m(0x1B10, (W * 4u) << 16);                     /* CONTROL1: pitch */
    m(0x1B14, (1u << 24) | (1u << 16));            /* FILTER: nearest */
    m(0x1B1C, ((uint32_t)W << 16) | H);            /* IMAGE_RECT */
    m(0x028C, 0x18u << 8);                         /* final alpha: T0's */
    quad(0, W, 0.0f, 0.0f, 0.0f);
    m(0x028C, 0x20u << 8);                         /* final alpha: 1 again */
}

int main(void)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t x, y;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=1");
#else
    setenv("RECOMP_RASTER_THREADS", "1", 1);
#endif

    m(0x0208, 0x0028);                             /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    m(0x0200, (uint32_t)W << 16);                  /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);                  /* CLIP_VERTICAL */
    m(0x020C, ((W * 4u) << 16) | (W * 4u));        /* SURFACE_PITCH: zeta, colour */
    m(0x0214, ZETA_AT);                            /* SURFACE_ZETA_OFFSET */
    for (x = 0; x < 16; x++)
        mf(0x0680 + 4 * x, I[x]);                  /* composite: positions are pixels */
    m(0x1E94, 0);                                  /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
    m(0x1744, TC_AT);   m(0x1784, 2 | (3 << 4) | (12 << 8));       /* attr 9: float3 */
    m(0x1E60, 0);                                  /* COMBINER_CONTROL: no general stages */
    m(0x1E70, 1);                                  /* stage 0: 2D projective */
    m(0x0288, 8u);                                 /* final: D = T0 */
    m(0x028C, 0x20u << 8);                         /* final alpha: 1 */

    /* The shadow map: cleared to the farthest depth, then the caster drawn
     * with no colour surface and colour writes off. */
    m(0x0210, 0);                                  /* no colour surface */
    m(0x1D8C, 0xFFFFFF00u);                        /* ZSTENCIL_CLEAR_VALUE */
    m(0x1D94, 0x03);                               /* CLEAR_SURFACE: Z and stencil */
    m(0x0358, 0);                                  /* SET_COLOR_MASK: none */
    m(0x030C, 1);                                  /* DEPTH_TEST_ENABLE */
    m(0x0354, 0x207);                              /* DEPTH_FUNC: ALWAYS */
    m(0x035C, 1);                                  /* DEPTH_MASK */
    m(0x1E70, 0);                                  /* stage 0: none */
    quad(0, W / 2, 1000.0f, 0.0f, 0.0f);
    m(0x1E70, 1);

    light(ZETA_AT, 1, 0.0f);                       /* nearest */
    printf("nearest: x=0 %d x=7 %d x=8 %d x=15 %d\n", gray(0, 8), gray(7, 8), gray(8, 8), gray(15, 8));
    for (y = 0; y < H; y += 5)
        for (x = 0; x < W; x++)
            CHECK(gray((int)x, (int)y) == (x < W / 2 ? 0 : 255),
                  "nearest (%u,%u) is %d, want %d", x, y, gray((int)x, (int)y),
                  x < W / 2 ? 0 : 255);

    /* Linear: coordinates half a texel left, so pixel x reads texels x-1 and
     * x equally; at x = 8 one is shadowed and one is lit. */
    light(ZETA_AT, 2, -0.5f);
    printf("linear: x=7 %d x=8 %d x=9 %d\n", gray(7, 8), gray(8, 8), gray(9, 8));
    CHECK(gray(7, 8) == 0, "linear x=7 is %d, want 0", gray(7, 8));
    CHECK(gray(8, 8) >= 126 && gray(8, 8) <= 129, "linear x=8 is %d, want about 128", gray(8, 8));
    CHECK(gray(9, 8) == 255, "linear x=9 is %d, want 255", gray(9, 8));

    /* The depth buffer read as colour: the rasteriser keeps it on the host, and
     * the copy has its words, the far clear's whole. */
    copy_depths();
    {
        uint32_t near, far;
        memcpy(&near, s_mem + COPY_AT + 4u * (8 * W + 3), 4);
        memcpy(&far, s_mem + COPY_AT + 4u * (8 * W + 12), 4);
        printf("copy: near %08X far %08X\n", near, far);
        CHECK(near == (1000u << 8), "copy: near word %08X, want %08X", near, 1000u << 8);
        CHECK(far == 0xFFFFFF00u, "copy: far word %08X, want FFFFFF00", far);
    }

    /* Not a depth buffer the rasteriser drew: the words in memory, depth in
     * the top 24 bits. Near on the right half this time. */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t word = x < W / 2 ? 0xFFFFFF00u : (1000u << 8) | 0x55u;
            memcpy(s_mem + TEX_AT + 4u * (y * W + x), &word, 4);
        }
    light(TEX_AT, 1, 0.0f);
    printf("memory: x=0 %d x=7 %d x=8 %d x=15 %d\n", gray(0, 8), gray(7, 8), gray(8, 8),
           gray(15, 8));
    for (x = 0; x < W; x++)                        /* a texel is a word */
        CHECK(gray((int)x, 8) == (x < W / 2 ? 255 : 0), "memory (%u,8) is %d, want %d",
              x, gray((int)x, 8), x < W / 2 ? 255 : 0);

    if (!s_fail)
        printf("nv2a shadow map: ok\n");
    return s_fail ? 1 : 0;
}
