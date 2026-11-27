/* Polygon offset on the software rasteriser.
 *
 *   - a plane sloping 37.5 depth steps per pixel in x is drawn into a D24S8
 *     buffer with SET_POLY_OFFSET_FILL_ENABLE, scale 2 and bias 20, so each
 *     depth moves 2 * 37.5 + 20 = 95 steps back;
 *   - the same plane drawn again without the offset and DEPTH_FUNC LESS
 *     passes at every pixel, as a shadow receiver in front of its own map;
 *   - the stored depth is the moved one: the plane 94 steps back still
 *     passes LESS everywhere, 96 steps back nowhere;
 *   - with the enable off, the receiver fails LESS everywhere;
 *   - a negative bias stops at depth 0 (EQUAL against 0 passes).
 *
 * Before the offset was applied, the receiver met its own depths: a strict
 * compare failed everywhere, and in a shadow map a lit floor shadowed itself
 * in a noise of dots wherever rounding put the two depths either way. */
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
#define POS_AT    0x40000u
#define DIFF_AT   0x42000u
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

#define SLOPE 37.5f
#define LESS  0x201
#define EQUAL 0x202

/* The whole surface as two triangles, depth z0 + slope * x. */
static void plane(float z0, float slope)
{
    const float c[6][2] = { {0, 0}, {W, 0}, {W, H}, {0, 0}, {W, H}, {0, H} };
    uint32_t i, white = 0xFFFFFFFFu;

    for (i = 0; i < 6; i++) {
        float p[3] = { c[i][0], c[i][1], z0 + slope * c[i][0] };
        memcpy(s_mem + POS_AT + 12u * i, p, 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
    }
    m(0x17FC, 5);                                  /* BEGIN_END: triangles */
    m(0x1810, 5u << 24);                           /* DRAW_ARRAYS: 6 from 0 */
    m(0x17FC, 0);
}

/* Depth only, written whatever is there, with the offset on or off. */
static void caster(float z0, float slope, int offset, float bias)
{
    m(0x0210, 0);                                  /* no colour surface */
    m(0x1D8C, 0xFFFFFF00u);                        /* ZSTENCIL_CLEAR_VALUE */
    m(0x1D94, 0x03);                               /* CLEAR_SURFACE: Z and stencil */
    m(0x0358, 0);                                  /* SET_COLOR_MASK: none */
    m(0x030C, 1);                                  /* DEPTH_TEST_ENABLE */
    m(0x0354, 0x207);                              /* DEPTH_FUNC: ALWAYS */
    m(0x035C, 1);                                  /* DEPTH_MASK */
    m(0x0338, offset ? 1u : 0u);                   /* SET_POLY_OFFSET_FILL_ENABLE */
    mf(0x0384, 2.0f);                              /* SET_POLYGON_OFFSET_SCALE_FACTOR */
    mf(0x0388, bias);                              /* SET_POLYGON_OFFSET_BIAS */
    plane(z0, slope);
    m(0x0338, 0);
}

/* A plane in white against what the caster left, depth kept; returns how
 * many pixels it drew. */
static int receiver(float z0, float slope, uint32_t func)
{
    uint32_t k, p;
    int n = 0;

    memset(s_mem + COLOR_AT, 0, 4u * W * H);
    m(0x0210, COLOR_AT);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x0358, 0x01010101u);                        /* SET_COLOR_MASK: all */
    m(0x0354, func);                               /* DEPTH_FUNC */
    m(0x035C, 0);                                  /* DEPTH_MASK: off */
    plane(z0, slope);
    for (k = 0; k < W * H; k++) {
        memcpy(&p, s_mem + COLOR_AT + 4u * k, 4);
        n += (p & 0xFFu) == 0xFFu;
    }
    return n;
}

int main(void)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float z0 = 100000.0f;
    int x, n;

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
    m(0x1E60, 0);                                  /* COMBINER_CONTROL: no general stages */
    m(0x1E70, 0);                                  /* stage 0: none */
    m(0x0288, 4u);                                 /* final: D = diffuse */
    m(0x028C, 0x20u << 8);                         /* final alpha: 1 */

    /* Offset on: the receiver is in front of the map everywhere, and the map
     * sits 95 steps behind the plane: 94 back passes, 96 back does not. */
    caster(z0, SLOPE, 1, 20.0f);
    n = receiver(z0, SLOPE, LESS);
    printf("offset on: %d of %d pixels lit\n", n, W * H);
    CHECK(n == W * H, "offset on: %d of %d pixels pass LESS", n, W * H);
    n = receiver(z0 + 94.0f, SLOPE, LESS);
    CHECK(n == W * H, "94 steps back: %d of %d pixels pass LESS", n, W * H);
    n = receiver(z0 + 96.0f, SLOPE, LESS);
    CHECK(n == 0, "96 steps back: %d pixels pass LESS, want 0", n);

    /* Offset off: the receiver meets its own depths and LESS fails. */
    caster(z0, SLOPE, 0, 20.0f);
    n = receiver(z0, SLOPE, LESS);
    printf("offset off: %d of %d pixels lit\n", n, W * H);
    CHECK(n == 0, "offset off: %d pixels pass LESS, want 0", n);

    /* A flat plane near 0 pulled forward past it: clamped, not negative. */
    caster(5.0f, 0.0f, 1, -20.0f);
    n = receiver(0.0f, 0.0f, EQUAL);
    printf("clamped: %d of %d pixels at 0\n", n, W * H);
    CHECK(n == W * H, "negative bias: %d of %d pixels at depth 0", n, W * H);

    if (!s_fail)
        printf("nv2a poly offset: ok\n");
    return s_fail ? 1 : 0;
}
