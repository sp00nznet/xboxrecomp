/**
 * pb_stride0: a vertex array with a size but stride 0 is still an array.
 * Every vertex reads the one element at its offset, and the inline value
 * (SET_VERTEX_DATA4F) is used only when the size is 0. That is the NV2A rule
 * xemu follows (pgraph/gl/vertex.c pgraph_gl_bind_vertex_attributes: count 0
 * is disabled, stride 0 binds the element at the offset to every vertex).
 * SSX 3's rider passes its per-mesh offset in v4 this way.
 *
 * A DRAW_ARRAYS quad on the CPU rasteriser with mov oPos, v0; mov oD0, v4.
 * v0 is an ordinary float4 array. v4 is a float3 array at stride 0 whose
 * element is green, the element after it blue, and the inline v4 red:
 *
 *   stride0   every pixel green and opaque (w filled with 1), never the
 *             inline red or the next element's blue
 *   disabled  size 0 with the same offset: the inline red
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define POS   0x02800000u
#define COL   0x02801000u

#define BLACK  0xFF000000u
#define RED    0xFFFF0000u
#define GREEN  0xFF00FF00u

/* SET_VERTEX_DATA_ARRAY_FORMAT: stride [31:8], size [7:4], type [3:0]. */
#define FMT(type, size, stride) (((uint32_t)(stride) << 8) | ((size) << 4) | (type))
#define F 2u                    /* float */

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

static void vertex_program(void)
{
    static const uint32_t outs[2][2] = {{0, 0}, {3, 4}}; /* {o, v} */
    int k;
    M(0x1E9C, 0);                          /* PROGRAM_LOAD: slot 0 */
    for (k = 0; k < 2; k++) {
        M(0x0B00 + 16 * k + 0, 0);
        M(0x0B00 + 16 * k + 4, (1u << 21) | (outs[k][1] << 9) | 0x1Bu);
        M(0x0B00 + 16 * k + 8, 2u << 26);  /* A = v */
        M(0x0B00 + 16 * k + 12, (0xFu << 12) | (1u << 11)
                                | (outs[k][0] << 3) | (k == 1 ? 1u : 0u));
    }
    M(0x1EA0, 0);                          /* PROGRAM_START */
    M(0x1E94, 2);                          /* EXECUTION_MODE: program */
}

static void frame(uint32_t v4_format)
{
    int a;
    M(0x0200, W << 16);                    /* SURFACE_CLIP_HORIZONTAL */
    M(0x0204, H << 16);                    /* SURFACE_CLIP_VERTICAL   */
    M(0x0208, 0x00000128u);                /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    M(0x020C, W * 4u);                     /* SURFACE_PITCH           */
    M(0x0210, BUF0);                       /* SURFACE_COLOR_OFFSET    */
    M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
    M(0x1D98, (W - 1u) << 16);             /* CLEAR_RECT: the surface */
    M(0x1D9C, (H - 1u) << 16);
    M(0x1D90, BLACK);                      /* COLOR_CLEAR_VALUE       */
    M(0x1D94, 0xF0);                       /* CLEAR_SURFACE: colour   */
    M(0x0304, 0);                          /* BLEND_ENABLE off        */
    M(0x030C, 0);                          /* DEPTH_TEST_ENABLE off   */
    M(0x1B0C, 0);                          /* stage 0 off             */
    for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
        M(0x1760 + 4 * a, a == 0 ? FMT(F, 4, 16) : a == 4 ? v4_format : F);
    M(0x1720 + 4 * 0, POS);                /* VERTEX_DATA_ARRAY_OFFSET */
    M(0x1720 + 4 * 4, COL);
    M(0x1A40, f2u(1.0f));                  /* VERTEX_DATA4F(4): red   */
    M(0x1A44, f2u(0.0f));
    M(0x1A48, f2u(0.0f));
    M(0x1A4C, f2u(1.0f));
    vertex_program();
    M(0x17FC, 8);                          /* BEGIN_END: QUADS */
    M(0x1810, (3u << 24) | 0u);            /* DRAW_ARRAYS: 4 from 0 */
    M(0x17FC, 0);
}

static void end_frame(void)
{
    M(0x0210, BUF1);
    M(0x012C, 0);                          /* FLIP_INCREMENT_WRITE */
    M(0x0130, 0);                          /* FLIP_STALL */
}

/* All 32 bits: the array's missing w must come out as 1 (opaque). */
static uint32_t px32(uint32_t x, uint32_t y)
{
    return *(const uint32_t *)(guest(BUF0) + y * W * 4u + x * 4u);
}

static void expect_px(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    fprintf(stderr, "[pb_stride0] %-52s %08X want %08X %s\n", what, got, want,
            ok ? "PASS" : "FAIL");
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
    /* (8,8)-(56,56), 1/32 pixel off the grid. */
    const float o = 1.0f / 32.0f, a = 8.0f + o, b = 56.0f + o;
    const float pos[4][4] = {{a, a, 1, 1}, {b, a, 1, 1}, {b, b, 1, 1}, {a, b, 1, 1}};
    /* The element, then what a walk at a 12-byte stride would read. */
    const float col[2][3] = {{0, 1, 0}, {0, 0, 1}};
    static const uint32_t at[5][2] = {{12, 12}, {50, 12}, {50, 50}, {12, 50}, {32, 32}};
    static uint8_t xbe[0x1000];
    char what[96];
    int k;

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_stride0] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);             /* the CPU rasteriser */
    memcpy(guest(POS), pos, sizeof pos);
    memcpy(guest(COL), col, sizeof col);

    frame(FMT(F, 3, 0));
    for (k = 0; k < 5; k++) {
        snprintf(what, sizeof what, "stride 0, size 3: the element at (%u,%u)",
                 at[k][0], at[k][1]);
        expect_px(what, px32(at[k][0], at[k][1]), GREEN);
    }
    expect_px("stride 0: outside the quad", px32(2, 2), BLACK);
    end_frame();

    frame(FMT(F, 0, 0));
    expect_px("size 0: the inline value", px32(32, 32), RED);
    end_frame();

    if (s_fail) {
        fprintf(stderr, "[pb_stride0] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[pb_stride0] done: all checks pass\n");
    return 0;
}
