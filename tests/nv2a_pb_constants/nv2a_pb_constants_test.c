/* SET_TRANSFORM_CONSTANT words through the executor's short path
 * (nv2a_pb_exec_constant_word) and through the general dispatcher
 * (nv2a_pb_exec_method) must land the same constants:
 *
 *   - before the executor's one-time setup the short path declines the word,
 *     so the dispatcher sets up first;
 *   - after it, a constant uploaded either way reads back the same, with
 *     _CONSTANT_LOAD and the auto-increment across rows behaving alike;
 *
 * Then both are timed on a stream of constant words; the times are printed,
 * not checked. Harness as tests/nv2a_ffp_backend, without a back end. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;   /* #189 made the stack placement runtime state */
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static uint32_t s_fence_va;                           /* the mirrored fence word, if any */
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
int nv2a_pb_dma_walk(void) { return 1; }             /* RECOMP_PB_WALK=dma: 0x0310 is latched */
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
/* The flip hold (nv2a_flip_hold.c, linked in). No vblank moves here, so the
 * executor never holds a flip; the walk is never inside a pushbuffer CALL. */
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }

static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

#include <time.h>
#include "nv2a_vsh_interp.h"

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_constant_word(uint32_t method, uint32_t param);

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* n constant rows from row `at`, values from `base`, one way or the other. */
static void upload(uint32_t at, uint32_t rows, float base, int fast)
{
    uint32_t i;

    nv2a_pb_exec_method(0, 0x1EA4, at);                 /* _CONSTANT_LOAD */
    for (i = 0; i < rows * 4; i++) {
        uint32_t method = 0x0B80u + 4u * (i & 31u);
        uint32_t param = f2u(base + (float)i);
        if (!fast || !nv2a_pb_exec_constant_word(method, param))
            nv2a_pb_exec_method(0, method, param);
    }
}

int main(void)
{
    uint32_t r, k;

    /* Nothing has reached the dispatcher yet: the short path declines. */
    CHECK(nv2a_pb_exec_constant_word(0x0B80, 0) == 0);
    nv2a_pb_exec_method(0, 0x0100, 0);                   /* NOP: the setup runs */

    /* Twelve rows each way -- more than one 32-word method run -- into
     * different places, then the same values compared row by row. */
    upload(100, 12, 1.0f, 1);
    upload(140, 12, 1.0f, 0);
    for (r = 0; r < 12; r++) {
        const float *a = nv2a_vsh_constant(100 + r);
        const float *b = nv2a_vsh_constant(140 + r);
        for (k = 0; k < 4; k++) {
            CHECK(a[k] == b[k]);
            CHECK(a[k] == 1.0f + (float)(r * 4 + k));
        }
    }

    /* Timing: 20 million constant words each way, in 32-word runs from
     * c[96], as D3D uploads its shader constants. */
    {
        const uint32_t n = 20000000u;
        double t0, td, tf;
        uint32_t i;

        t0 = now_s();
        for (i = 0; i < n; i++) {
            if ((i & 127u) == 0)
                nv2a_pb_exec_method(0, 0x1EA4, 96);
            nv2a_pb_exec_method(0, 0x0B80u + 4u * (i & 31u), i);
        }
        td = now_s() - t0;
        t0 = now_s();
        for (i = 0; i < n; i++) {
            uint32_t method = 0x0B80u + 4u * (i & 31u);
            if ((i & 127u) == 0)
                nv2a_pb_exec_method(0, 0x1EA4, 96);
            if (!nv2a_pb_exec_constant_word(method, i))
                nv2a_pb_exec_method(0, method, i);
        }
        tf = now_s() - t0;
        printf("constant words: dispatcher %.2f ns a word, short path %.2f ns a word (%.1fx)\n",
               td * 1e9 / n, tf * 1e9 / n, td / tf);
    }

    if (!s_fail)
        printf("nv2a constant short path: ok\n");
    return s_fail;
}
