/* Colours across a triangle in perspective on the software rasteriser: a
 * fixed-function triangle with a near white corner (w = 1) and two far black
 * ones (w = 8). At a pixel a third of the way from each edge the screen
 * weights are about equal, but the near corner counts eight times as much
 * per unit of screen: the colour is about 200, not 80. Texture coordinates
 * were already interpolated so, and a GPU does the same with colours.
 *
 * Linear on the screen, a triangle whose near corners are clipped far off
 * the surface takes its colour from the far corner alone: a light beam
 * coloured at its source and black at its far end came out black. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 16
#define H 16
#define COLOR_AT  0x10000u
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

int main(void)
{
    /* x, y and w out as they come in, w = the input's z. */
    static const float M[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 1, 0 };
    /* (screen x, screen y, w) and the colour. */
    static const float c[3][3] = { {0, 0, 1}, {16, 0, 8}, {0, 16, 8} };
    static const uint32_t col[3] = { 0xFFFFFFFFu, 0xFF000000u, 0xFF000000u };
    uint32_t i, p;
    int g;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=1");
#else
    setenv("RECOMP_RASTER_THREADS", "1", 1);
#endif

    m(0x0208, 0x0028);                             /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    m(0x0200, (uint32_t)W << 16);                  /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);                  /* CLIP_VERTICAL */
    m(0x020C, ((W * 4u) << 16) | (W * 4u));        /* SURFACE_PITCH */
    m(0x0210, COLOR_AT);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x0358, 0x01010101u);                        /* SET_COLOR_MASK: all */
    m(0x030C, 0);                                  /* DEPTH_TEST_ENABLE: off */
    m(0x035C, 0);                                  /* DEPTH_MASK: off */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);                  /* composite */
    m(0x1E94, 0);                                  /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
    for (i = 0; i < 3; i++) {
        float pos[3] = { c[i][0] * c[i][2], c[i][1] * c[i][2], c[i][2] };
        memcpy(s_mem + POS_AT + 12u * i, pos, 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &col[i], 4);
    }
    m(0x17FC, 5);                                  /* BEGIN_END: triangles */
    m(0x1810, 2u << 24);                           /* DRAW_ARRAYS: 3 from 0 */
    m(0x17FC, 0);

    /* Pixel (5, 5): screen weights 0.3125 for the white corner and 0.34375
     * for each black one; in perspective 0.3125 / (0.3125 + 2 * 0.34375 / 8). */
    memcpy(&p, s_mem + COLOR_AT + 4u * (5 * W + 5), 4);
    g = (int)((p >> 8) & 0xFF);
    printf("pixel (5,5) green %d, want 200 (80 linear on the screen)\n", g);
    CHECK(g >= 198 && g <= 202, "pixel (5,5) green %d, want about 200", g);

    if (!s_fail)
        printf("nv2a perspective colour: ok\n");
    return s_fail ? 1 : 0;
}
