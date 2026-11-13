/* A frame that only clears its surface (then CPU-blits into it, as a video
 * player does) presents that surface at the flip, not the last one drawn. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
#define FB_A 0x8000u
#define FB_B 0x9000u
#define CLEAR_PIXEL 0xFF102030u

static uint32_t s_memory[MEM_BYTES / sizeof(uint32_t)];
static uint8_t *const s_mem = (uint8_t *)s_memory;
static int s_fail;
static uint32_t s_presented_va, s_presented_pixel;
static unsigned s_present_count;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

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
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    (void)pitch;
    s_present_count++;
    s_presented_va = va;
    if (va <= MEM_BYTES - sizeof(uint32_t))
        memcpy(&s_presented_pixel, s_mem + va, sizeof(uint32_t));
}

static void draw_surface(const Nv2aSurface *s, const Nv2aBatch *batch)
{
    (void)s; (void)batch;
}
/* Draws only: clears stay on the executor's own path. */
static const Nv2aBackend s_backend = {NULL, draw_surface, NULL};

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void method(uint32_t number, uint32_t value) { nv2a_pb_exec_method(0, number, value); }
static void write_float(uint32_t address, float value) { memcpy(s_mem + address, &value, 4); }

static void draw_triangle(void)
{
    static const float pos[3][3] = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}};
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 3; j++)
            write_float(0x2000 + i * 12u + j * 4u, pos[i][j]);
    method(0x1720, 0x2000);
    method(0x1760, 2 | (3 << 4) | (12 << 8));
    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
}

int main(void)
{
    method(0x0208, 0x0008);     /* A8R8G8B8 */
    method(0x0200, 1u << 16);   /* 1x1 clip */
    method(0x0204, 1u << 16);
    method(0x020C, 4);          /* pitch */
    nv2a_backend_register(&s_backend);

    /* Frame 1 draws into A. */
    method(0x0210, FB_A);
    draw_triangle();
    method(0x0130, 0);          /* flip */
    CHECK(s_present_count == 1 && s_presented_va == FB_A);

    /* Frame 2 only clears B. */
    method(0x0210, FB_B);
    method(0x1D90, CLEAR_PIXEL);
    method(0x1D94, 0xF0);       /* colour */
    method(0x0130, 0);
    CHECK(s_present_count == 2 && s_presented_va == FB_B);
    CHECK(s_presented_pixel == CLEAR_PIXEL);

    nv2a_backend_register(NULL);
    if (!s_fail)
        puts("NV2A clear-only frame: ok");
    return s_fail;
}
