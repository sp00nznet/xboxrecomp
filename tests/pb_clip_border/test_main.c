/**
 * pb_clip_border: two rules of the CPU rasteriser's vertex-program path
 * (raster_batch_vsh), the path a title's 3D draws take.
 *
 *   near     the near plane (z >= 0) clips with the depth test off too, as
 *            the NV2A and the GPU backends do (upstream #211's rule): a
 *            quad whose z runs from -100 on its left edge to +100 on its
 *            right draws only its right half. The far plane stays a
 *            depth-on rule
 *   border   texture address mode 4 with the border colour as the source
 *            (FORMAT bit 3), as Burnout 3 sets stages 0-3: the taps past the
 *            texture are SET_TEXTURE_BORDER_COLOR, read through the
 *            decoded-texture cache (texel_cached), nearest and bilinear.
 *            With the texture as the source the edge texel stands in
 *   clear16  SET_COLOR_CLEAR_VALUE in the colour surface's format: an
 *            R5G6B5 or X1R5G5B5 surface takes the low 16 bits as they are
 *            (JSRF's logos), and the format at CLEAR_SURFACE decides, not
 *            the one current when the value arrived
 *
 * A pass-through program (mov oPos, v0; mov oD0, v3; mov oT0, v9, as
 * pb_tex_cache) on a 64x64 surface; pixels are read back from guest memory.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d8_swizzle.h"
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
#define TEX   0x02800000u
#define LOG2  4u                /* 16x16 */

#define BLACK  0xFF000000u
#define WHITE  0xFFFFFFFFu
#define RED    0xFFFF0000u
#define ODD    0xFF20C040u      /* a border colour no sampler preset has */
#define GREY   0xFF404040u      /* the border tests' clear: no border colour,
                                 * so a quad that misses cannot pass */

/* SET_TEXTURE_FILTER: MAG [27:24], MIN [21:16]; 1 nearest, 2 linear. */
#define FILTER_NEAREST ((1u << 24) | (1u << 16))
#define FILTER_LINEAR  ((2u << 24) | (2u << 16))

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

static void vertex(float x, float y, float z, float u, float v)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(z));
    M(0x1818, f2u(1.0f));
    M(0x1818, WHITE);
    M(0x1818, f2u(u));
    M(0x1818, f2u(v));
}

static void vertex_program(void)
{
    static const uint32_t outs[3][2] = {{0, 0}, {3, 3}, {9, 9}}; /* {o, v} */
    int k;
    M(0x1E9C, 0);                          /* PROGRAM_LOAD: slot 0 */
    for (k = 0; k < 3; k++) {
        M(0x0B00 + 16 * k + 0, 0);
        M(0x0B00 + 16 * k + 4, (1u << 21) | (outs[k][1] << 9) | 0x1Bu);
        M(0x0B00 + 16 * k + 8, 2u << 26);  /* A = v */
        M(0x0B00 + 16 * k + 12, (0xFu << 12) | (1u << 11)
                                | (outs[k][0] << 3) | (k == 2 ? 1u : 0u));
    }
    M(0x1EA0, 0);                          /* PROGRAM_START */
    M(0x1E94, 2);                          /* EXECUTION_MODE: program */
}

static void begin_frame(uint32_t buf, uint32_t clear)
{
    int a;
    M(0x0200, W << 16);                    /* SURFACE_CLIP_HORIZONTAL */
    M(0x0204, H << 16);                    /* SURFACE_CLIP_VERTICAL   */
    M(0x0208, 0x00000128u);                /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    M(0x020C, W * 4u);                     /* SURFACE_PITCH           */
    M(0x0210, buf);                        /* SURFACE_COLOR_OFFSET    */
    M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
    M(0x1D98, (W - 1u) << 16);             /* CLEAR_RECT: the surface */
    M(0x1D9C, (H - 1u) << 16);
    M(0x1D90, clear);                      /* COLOR_CLEAR_VALUE       */
    M(0x1D94, 0xF0);                       /* CLEAR_SURFACE: colour   */
    M(0x0304, 0);                          /* BLEND_ENABLE off        */
    M(0x030C, 0);                          /* DEPTH_TEST_ENABLE off   */
    M(0x1B0C, 0);                          /* stage 0 off             */
    for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : a == 9 ? 0x22u : 0x02u);
    vertex_program();
}

static void end_frame(uint32_t other)
{
    M(0x1B0C, 0);
    M(0x0210, other);
    M(0x012C, 0);                          /* FLIP_INCREMENT_WRITE */
    M(0x0130, 0);                          /* FLIP_STALL */
}

static uint32_t px(uint32_t buf, uint32_t x, uint32_t y)
{
    return *(const uint32_t *)(guest(buf) + y * W * 4u + x * 4u) | 0xFF000000u;
}

/* All 32 bits, alpha too: with blending off the border's alpha lands in the
 * surface, and transparent black must not pass for opaque black. */
static uint32_t px32(uint32_t buf, uint32_t x, uint32_t y)
{
    return *(const uint32_t *)(guest(buf) + y * W * 4u + x * 4u);
}

static uint16_t px16(uint32_t buf, uint32_t x, uint32_t y)
{
    return *(const uint16_t *)(guest(buf) + y * W * 2u + x * 2u);
}

static void expect_px(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    fprintf(stderr, "[pb_clip_border] %-52s %08X want %08X %s\n", what, got, want,
            ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

/* clear16: a 16-bit surface at buf, filled with `fill` from the CPU,
 * SET_COLOR_CLEAR_VALUE sent while the surface format is `at_value` and
 * CLEAR_SURFACE while it is `at_clear`, over the clear rect (8,8)-(23,23)
 * (max edge inclusive). */
static void clear16(uint32_t buf, uint16_t fill, uint32_t at_value, uint32_t at_clear,
                    uint32_t value)
{
    uint16_t *p = (uint16_t *)guest(buf);
    uint32_t i;
    for (i = 0; i < W * H; i++)
        p[i] = fill;
    M(0x0200, W << 16);
    M(0x0204, H << 16);
    M(0x020C, W * 2u);
    M(0x0210, buf);
    M(0x0208, at_value);
    M(0x1D98, (23u << 16) | 8u);
    M(0x1D9C, (23u << 16) | 8u);
    M(0x1D90, value);
    M(0x0208, at_clear);
    M(0x1D94, 0xF0);
    M(0x1D98, (W - 1u) << 16);             /* back to the whole surface */
    M(0x1D9C, (H - 1u) << 16);
}

/* (8,8)-(56,56), z from zl on the left edge to zr on the right, UVs from
 * uv0 to uv1 on both axes. 1/32 pixel off the grid: no pixel centre lands
 * on a texel edge. */
static void quad(float zl, float zr, float uv0, float uv1)
{
    const float o = 1.0f / 32.0f, a = 8.0f + o, b = 56.0f + o;
    M(0x17FC, 8);                          /* BEGIN_END: QUADS */
    vertex(a, a, zl, uv0, uv0);
    vertex(b, a, zr, uv1, uv0);
    vertex(b, b, zr, uv1, uv1);
    vertex(a, b, zl, uv0, uv1);
    M(0x17FC, 0);
}

/* Stage 0: the red 16x16 texture, one level, 2D, address mode 4 on both
 * axes, the border colour as the source unless texsrc. */
static void stage0(int texsrc, uint32_t border, uint32_t filter)
{
    M(0x1B00, TEX);                        /* TEXTURE_OFFSET(0) */
    M(0x1B04, (LOG2 << 24) | (LOG2 << 20) | (1u << 16) | (0x06u << 8)
              | (2u << 4) | (texsrc ? 0u : 1u << 3) | 2u);
    M(0x1B08, 0x00000404u);                /* border u, border v */
    M(0x1B0C, (1u << 30) | (7u << 6));     /* CONTROL0: enable, LOD max 7 */
    M(0x1B14, filter);
    M(0x1B24, border);                     /* TEXTURE_BORDER_COLOR(0) */
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
    static uint8_t xbe[0x1000];
    uint32_t *t, i;
    char what[96];
    int f;

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_clip_border] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);             /* the CPU rasteriser */
    t = (uint32_t *)guest(TEX);
    for (i = 0; i < (1u << LOG2) * (1u << LOG2); i++)
        t[i] = RED;

    /* near: untextured, white, z -100 .. +100 crossing 0 at x = 32. */
    begin_frame(BUF0, BLACK);
    quad(-100.0f, 100.0f, 0.0f, 1.0f);
    expect_px("near, depth off: the half in front (z < 0) is clipped", px(BUF0, 16, 32), BLACK);
    expect_px("near, depth off: the half behind it draws", px(BUF0, 48, 32), WHITE);
    M(0x030C, 1);                          /* DEPTH_TEST_ENABLE on, ALWAYS */
    M(0x0354, 0x0207);
    M(0x1D8C, 0xFFFFFF00u);                /* clear the depth to far */
    M(0x1D94, 0x03);
    M(0x1D90, BLACK);
    M(0x1D94, 0xF0);
    quad(-100.0f, 100.0f, 0.0f, 1.0f);
    expect_px("near, depth on: the half in front is clipped", px(BUF0, 16, 32), BLACK);
    expect_px("near, depth on: the half behind it draws", px(BUF0, 48, 32), WHITE);
    M(0x030C, 0);
    end_frame(BUF1);

    /* border: UVs -0.5 .. 1.5, the texture from x = 20 to 44. Left, right,
     * above and below it, then its middle. */
    for (f = 0; f < 2; f++) {
        static const uint32_t at[5][2] = {{12, 32}, {50, 32}, {32, 12}, {32, 50}, {32, 32}};
        static const uint32_t colour[2] = {ODD, 0x00000000u};
        const char *fn = f ? "linear" : "nearest";
        uint32_t filter = f ? FILTER_LINEAR : FILTER_NEAREST;
        int c, k;
        for (c = 0; c < 2; c++) {
            begin_frame(BUF0, GREY);
            stage0(0, colour[c], filter);
            quad(1.0f, 1.0f, -0.5f, 1.5f);
            for (k = 0; k < 5; k++) {
                snprintf(what, sizeof what, "border 0x%08X %s (%u,%u)", colour[c], fn,
                         at[k][0], at[k][1]);
                expect_px(what, px32(BUF0, at[k][0], at[k][1]),
                          k == 4 ? RED : colour[c]);
            }
            end_frame(BUF1);
        }
        begin_frame(BUF0, GREY);
        stage0(1, ODD, filter);
        quad(1.0f, 1.0f, -0.5f, 1.5f);
        for (k = 0; k < 5; k++) {
            snprintf(what, sizeof what, "texture source %s (%u,%u): the edge texel", fn,
                     at[k][0], at[k][1]);
            expect_px(what, px32(BUF0, at[k][0], at[k][1]), RED);
        }
        end_frame(BUF1);
    }

    /* clear16: JSRF's near-black on its R5G6B5 back buffer, with junk in the
     * high half, sent while the surface was still A8R8G8B8. */
    clear16(BUF0, 0xAAAAu, 0x00000128u, 0x00000123u, 0xFFFF20E4u);
    expect_px("clear16 R5G6B5 0xFFFF20E4, format changed: inside", px16(BUF0, 10, 10), 0x20E4u);
    expect_px("clear16 R5G6B5: the rect's max corner (23,23)", px16(BUF0, 23, 23), 0x20E4u);
    expect_px("clear16 R5G6B5: past the rect (24,23) untouched", px16(BUF0, 24, 23), 0xAAAAu);
    expect_px("clear16 R5G6B5: outside (30,30) untouched", px16(BUF0, 30, 30), 0xAAAAu);
    /* X1R5G5B5_O1R5G5B5: red as sent, not an A8R8G8B8 value reduced. */
    clear16(BUF0, 0xAAAAu, 0x00000122u, 0x00000122u, 0x00007C00u);
    expect_px("clear16 X1R5G5B5_O1R5G5B5 0x7C00", px16(BUF0, 10, 10), 0x7C00u);
    /* And back: a value sent on R5G6B5 and cleared on A8R8G8B8 is 32-bit. */
    M(0x020C, W * 4u);
    M(0x0210, BUF0);
    M(0x0208, 0x00000123u);
    M(0x1D90, 0x80204060u);
    M(0x0208, 0x00000128u);
    M(0x1D94, 0xF0);
    expect_px("clear16 value sent on R5G6B5, cleared on A8R8G8B8", px32(BUF0, 10, 10),
              0x80204060u);

    if (s_fail) {
        fprintf(stderr, "[pb_clip_border] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[pb_clip_border] done: all checks pass\n");
    return 0;
}
