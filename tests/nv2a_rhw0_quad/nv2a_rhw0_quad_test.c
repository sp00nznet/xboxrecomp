/* A 2D quad sent with RHW 0, drawn by the executor's own rasteriser.
 *
 * Pre-transformed vertices with RHW 0 are common: 2D quads, menus, movie
 * frames. Xbox D3D runs them through a program that takes the reciprocal, so
 * oPos.w and the W-fog distance come out +inf. A stand-in program here moves
 * them straight from the vertex (w = +inf in v0.w):
 *
 *   mov o[0], v0          position, w = +inf
 *   mov o[3], v3          diffuse
 *
 * and the test checks that the quad is drawn, in its diffuse colour.
 *
 * No back end is registered, so the executor rasterises into guest memory, a
 * buffer here, addressed by the offsets the title would use. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nv2a_vsh_interp.h"

#define MEM_BYTES 0x10000u
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

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

enum { W = 16, H = 16, SURF = 0x8000, PITCH = W * 4 };
enum { V = 2 };                                 /* source mux: input */
#define XYZW 0x1B

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

/* mov o[out].xyzw, v[in] */
static void mov(uint32_t slot, uint32_t out, uint32_t in, int final)
{
    uint32_t w[4] = {0};
    w[1] = (1u << 21) | (in << 9);              /* MAC mov, input index */
    w[1] |= XYZW;
    w[2] = (uint32_t)V << 26;                   /* A: input */
    w[3] = (0xFu << 12) | (1u << 11) | (out << 3) | (final ? 1u : 0u);
    nv2a_vsh_set_instruction(slot, w);
}

/* The surface's centre pixel, RGB only, after one quad. */
static uint32_t draw_quad(void)
{
    uint32_t px;

    memset(s_mem + SURF, 0, PITCH * H);
    m(0x17FC, 5);                               /* BEGIN_END: triangles */
    m(0x1810, 5u << 24);                        /* DRAW_ARRAYS: start 0, 6 vertices */
    m(0x17FC, 0);
    memcpy(&px, s_mem + SURF + (H / 2) * PITCH + (W / 2) * 4, 4);
    return px & 0x00FFFFFFu;
}

int main(void)
{
    static const float xy[6][2] = {
        {0, 0}, {W, 0}, {0, H}, {W, 0}, {W, H}, {0, H}
    };
    uint32_t i;

    m(0x0208, 0x0008);                          /* SET_SURFACE_FORMAT: A8R8G8B8 */
    m(0x0200, (uint32_t)W << 16);               /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);               /* CLIP_VERTICAL */
    m(0x020C, PITCH);                           /* SET_SURFACE_PITCH */
    m(0x0210, SURF);                            /* SET_SURFACE_COLOR_OFFSET */

    for (i = 0; i < 6; i++) {
        putf(0x2000 + 16 * i, xy[i][0]);
        putf(0x2004 + 16 * i, xy[i][1]);
        putf(0x2008 + 16 * i, 0.0f);
        putf(0x200C + 16 * i, INFINITY);        /* 1 / RHW 0 */
        put32(0x3000 + 4 * i, 0xFF0000FFu);     /* blue */
    }
    m(0x1720, 0x2000); m(0x1760, 2 | (4 << 4) | (16 << 8));      /* v0: float4 */
    m(0x172C, 0x3000); m(0x176C, 0 | (4 << 4) | (4 << 8));       /* v3: D3DCOLOR */

    mov(0, 0, 0, 0);                            /* oPos = v0 */
    mov(1, 3, 3, 1);                            /* oD0 = v3, final */
    m(0x1E94, 2);                               /* TRANSFORM_EXECUTION_MODE: program */

    /* w = +inf is drawn, as w = 0 is: the quad covers the surface. */
    CHECK(draw_quad() == 0x0000FFu);

    if (!s_fail)
        printf("nv2a RHW-0 quad: ok\n");
    return s_fail;
}
