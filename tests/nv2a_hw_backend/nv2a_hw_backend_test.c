/* The D3D11 renderer (src/video/nv2a_hw_d3d11.c) against the executor's own
 * CPU rasteriser.
 *
 * Each scene goes in as a title's pushbuffer methods, twice: once with no
 * back end (the CPU rasterises into guest memory), once with the renderer
 * registered on WARP (the GPU draws, present_sync writes the surface back at
 * the flip). The two images of guest memory are compared pixel by pixel.
 * Rasterisers differ on edge pixels and rounding, so a pixel counts as
 * different when a colour channel is more than 8 apart, and a scene passes
 * when at most 1% of its pixels differ and it drew something at all.
 *
 * Scenes: a vertex-program triangle with a colour per vertex; the same at
 * 2x2 anti-aliasing; two overlapping triangles with a depth test; the same
 * with the second drawn under a narrowed SET_CLIP_MAX; alpha
 * blending; a fixed-function triangle (the surface-pixel path); a 16-bit
 * surface; a texture (swizzled A8R8G8B8, point sampled) times diffuse; a
 * swizzled render target, alone and cleared through a linear view before it
 * is sampled; and the register combiners (two textures, fog, dot product and
 * mux), each through vertex programs and through CPU-transformed vertices. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_hw_d3d11.h"

#define MEM_BYTES (16u << 20)
static uint8_t *s_mem;

/* ---- what the executor expects the runtime to provide (as tests/nv2a_ffp_backend) */
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
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }

#define COLOR_VA 0x100000u
#define ZETA_VA  0x600000u
#define TEX_VA   0xA00000u
#define BACKGROUND 0xFF203040u

static int s_fail;

enum { MUX_TEMP = 1, MUX_INPUT = 2, MUX_CONST = 3 };
static void src(uint32_t w[4], int which, uint32_t mux, uint32_t reg, uint32_t swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= reg << 28 | mux << 26; }
    else if (which == 1) { w[2] |= swz << 17 | reg << 13 | mux << 11; }
    else { w[2] |= swz << 2 | (reg >> 2); w[3] |= (reg & 3) << 30 | mux << 28; }
}

/* mov o[out], v[in] */
static void mov_out(uint32_t p[4], uint32_t out, uint32_t in)
{
    p[1] = 1u << 21 | in << 9;
    src(p, 0, MUX_INPUT, 0, 0x1B);
    p[3] |= 0xFu << 12 | 1u << 11 | out << 3;
}

/* dp4 o[0], v0, c[100..103]; mov o[3], v3; mov o[9], v9; and for the
 * combiner scenes mov o[10], v9 (stage 1 shares the coordinates),
 * mov o[5], v0 (fog from x), mov o[4], v3; the D3D epilogue. */
static void upload_program(void)
{
    uint32_t p[11][4], i, k;

    memset(p, 0, sizeof p);
    for (i = 0; i < 4; i++) {
        p[i][1] = 7u << 21 | (100u + i) << 13;
        src(p[i], 0, MUX_INPUT, 0, 0x1B); src(p[i], 1, MUX_CONST, 0, 0x1B);
        p[i][3] |= (8u >> i) << 12 | 1u << 11;
    }
    mov_out(p[4], 3, 3);
    mov_out(p[5], 9, 9);
    mov_out(p[6], 10, 9);
    mov_out(p[7], 5, 0);
    mov_out(p[8], 4, 3);
    p[9][1] = 3u << 25 | 2u << 21 | 58u << 13;
    src(p[9], 0, MUX_TEMP, 12, 0x1B); src(p[9], 1, MUX_CONST, 0, 0x1B); src(p[9], 2, MUX_TEMP, 12, 0xFF);
    p[9][3] |= 0xEu << 12 | 1u << 11 | 0x8u << 16;
    p[10][1] = 4u << 21 | 59u << 13;
    src(p[10], 0, MUX_TEMP, 12, 0x1B); src(p[10], 1, MUX_TEMP, 1, 0x00); src(p[10], 2, MUX_CONST, 0, 0x1B);
    p[10][3] |= 0xEu << 12 | 1u << 11 | 1u;
    m(0x1E9C, 0);
    for (i = 0; i < 11; i++)
        for (k = 0; k < 4; k++)
            m(0x0B00 + 4 * ((i * 4 + k) % 32), p[i][k]);
    m(0x1EA0, 0);
}

static void constant(uint32_t index, float x, float y, float z, float w)
{
    m(0x1EA4, index);
    m(0x0B80, f2u(x)); m(0x0B84, f2u(y)); m(0x0B88, f2u(z)); m(0x0B8C, f2u(w));
}

/* A 640x480 (x aa) surface with a Z24S8 depth buffer, cleared. */
static void surface(int aa, int bpp16)
{
    uint32_t a = aa ? 0x2000u : 0;
    uint32_t pitch = (aa ? 1280u : 640u) * (bpp16 ? 2u : 4u);

    m(0x0208, a | 0x20u | (bpp16 ? 0x03u : 0x08u));  /* AA, Z24S8, R5G6B5 / A8R8G8B8 */
    m(0x0200, 640u << 16);
    m(0x0204, 480u << 16);
    m(0x020C, pitch | (pitch << 16));
    m(0x0210, COLOR_VA);
    m(0x0214, ZETA_VA);
    m(0x0398, f2u(16777215.0f));                      /* SET_CLIP_MAX */
    m(0x1D90, BACKGROUND);
    m(0x1D8C, 0xFFFFFF00u);                           /* depth 1.0, stencil 0 */
    m(0x1D94, 0xF3);                                  /* clear colour, Z, stencil */
}

/* A triangle as program input: v0 position (screen-ish, w), v3 colour, v9 uv. */
static void vertex(uint32_t i, float x, float y, float z, uint32_t argb, float u, float v)
{
    putf(0x2000 + 16 * i, x); putf(0x2004 + 16 * i, y); putf(0x2008 + 16 * i, z); putf(0x200C + 16 * i, 1.0f);
    put32(0x3000 + 4 * i, argb);
    putf(0x4000 + 8 * i, u); putf(0x4004 + 8 * i, v);
}

static void program_state(void)
{
    upload_program();
    constant(58, 320, -240, 16777215, 0);              /* viewport scale */
    constant(59, 320, 240, 0, 0);                      /* offset; near plane at window z 0 */
    constant(100, 1, 0, 0, 0); constant(101, 0, 1, 0, 0); constant(102, 0, 0, 1, 0); constant(103, 0, 0, 0, 1);
    m(0x1E94, 2);
    m(0x1720, 0x2000); m(0x1760, 2 | (4 << 4) | (16 << 8));
    m(0x172C, 0x3000); m(0x176C, 0 | (4 << 4) | (4 << 8));
    m(0x1744, 0x4000); m(0x1784, 2 | (2 << 4) | (8 << 8));
}

static void draw(uint32_t first, uint32_t count)
{
    m(0x17FC, 5);
    m(0x1810, first | ((count - 1) << 24));           /* DRAW_ARRAYS */
    m(0x17FC, 0);
}

static void reset_state(void)
{
    m(0x0304, 0);                                     /* blend off */
    m(0x030C, 0);                                     /* depth test off */
    m(0x035C, 1);
    m(0x0354, 0x201);                                 /* LESS */
    m(0x1B0C, 0);                                     /* texture 0 off */
    m(0x0308, 0);                                     /* culling off */
}

typedef void (*Scene)(void);

static void scene_gouraud(void)
{
    surface(0, 0); reset_state(); program_state();
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF0000u, 0, 0);
    vertex(1, 0.9f, -0.6f, 0.5f, 0xFF00FF00u, 0, 0);
    vertex(2, 0.0f, 0.9f, 0.5f, 0xFF0000FFu, 0, 0);
    draw(0, 3);
}

static void scene_aa(void)
{
    surface(1, 0); reset_state(); program_state();
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF8000u, 0, 0);
    vertex(1, 0.9f, -0.6f, 0.5f, 0xFF00FF80u, 0, 0);
    vertex(2, 0.0f, 0.9f, 0.5f, 0xFF8000FFu, 0, 0);
    draw(0, 3);
}

static void scene_depth(void)
{
    surface(0, 0); reset_state(); program_state();
    m(0x030C, 1);                                     /* depth test, LESS, write */
    vertex(0, -0.9f, -0.5f, 0.3f, 0xFFFF0000u, 0, 0); /* near: red */
    vertex(1, 0.5f, -0.5f, 0.3f, 0xFFFF0000u, 0, 0);
    vertex(2, -0.2f, 0.8f, 0.3f, 0xFFFF0000u, 0, 0);
    vertex(3, -0.5f, -0.8f, 0.7f, 0xFF0000FFu, 0, 0); /* far: blue, drawn after */
    vertex(4, 0.9f, -0.8f, 0.7f, 0xFF0000FFu, 0, 0);
    vertex(5, 0.2f, 0.6f, 0.7f, 0xFF0000FFu, 0, 0);
    draw(0, 6);
}

/* Window z is compared as it is: a layer drawn under a narrowed SET_CLIP_MAX
 * (z 0.2 of the range, clip max 0.25) still sorts in front of one at 0.4. A
 * back end that scaled each draw by its clip range would put it at 0.8. */
static void scene_narrow_clip(void)
{
    surface(0, 0); reset_state(); program_state();
    m(0x030C, 1);
    vertex(0, -0.9f, -0.5f, 0.4f, 0xFFFF0000u, 0, 0); /* red at 0.4, full range */
    vertex(1, 0.5f, -0.5f, 0.4f, 0xFFFF0000u, 0, 0);
    vertex(2, -0.2f, 0.8f, 0.4f, 0xFFFF0000u, 0, 0);
    draw(0, 3);
    m(0x0398, f2u(0.25f * 16777215.0f));             /* narrowed */
    vertex(3, -0.5f, -0.8f, 0.2f, 0xFF00FF00u, 0, 0); /* green at 0.2: in front */
    vertex(4, 0.9f, -0.8f, 0.2f, 0xFF00FF00u, 0, 0);
    vertex(5, 0.2f, 0.6f, 0.2f, 0xFF00FF00u, 0, 0);
    draw(3, 3);
}

/* A quad as a 4-vertex strip with back faces culled (XDK's default). The
 * strip's second triangle comes in reversed unless the executor alternates
 * the winding, which the renderer turns on (RECOMP_STRIP_ALT_WINDING): both
 * halves must be drawn, as the CPU rasteriser (which never culls) draws them. */
static void scene_cull_strip(void)
{
    surface(0, 0); reset_state(); program_state();
    m(0x0308, 1);                                     /* SET_CULL_FACE_ENABLE */
    m(0x039C, 0x405);                                 /* GL_BACK */
    m(0x03A0, 0x901);                                 /* front face GL_CCW */
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF0000u, 0, 0);
    vertex(1, 0.8f, -0.8f, 0.5f, 0xFF00FF00u, 0, 0);
    vertex(2, -0.8f, 0.8f, 0.5f, 0xFF0000FFu, 0, 0);
    vertex(3, 0.8f, 0.8f, 0.5f, 0xFFFFFF00u, 0, 0);
    m(0x17FC, 6);                                     /* TRIANGLE_STRIP */
    m(0x1810, 0 | (3u << 24));
    m(0x17FC, 0);
}

static void scene_blend(void)
{
    surface(0, 0); reset_state(); program_state();
    m(0x0304, 1);                                     /* blend: SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD */
    m(0x0344, 0x302); m(0x0348, 0x303); m(0x0350, 0x8006);
    vertex(0, -0.8f, -0.8f, 0.5f, 0x80FF0000u, 0, 0);
    vertex(1, 0.9f, -0.6f, 0.5f, 0x80FFFF00u, 0, 0);
    vertex(2, 0.0f, 0.9f, 0.5f, 0x8000FFFFu, 0, 0);
    draw(0, 3);
}

static void scene_fixed_function(void)
{
    static const float M[16] = { 320, 0, 0, 320, 0, -240, 0, 240, 0, 0, 16777215, 0, 0, 0, 0, 1 };
    uint32_t i;

    surface(0, 0); reset_state();
    for (i = 0; i < 16; i++)
        m(0x0680 + 4 * i, f2u(M[i]));
    m(0x1E94, 0);                                     /* fixed function */
    m(0x0314, 0);                                     /* lighting off */
    putf(0x5000, -0.7f); putf(0x5004, -0.7f); putf(0x5008, 0.5f);
    putf(0x500C, 0.8f);  putf(0x5010, -0.3f); putf(0x5014, 0.5f);
    putf(0x5018, -0.1f); putf(0x501C, 0.8f);  putf(0x5020, 0.5f);
    put32(0x6000, 0xFFFFFF00u); put32(0x6004, 0xFF00FFFFu); put32(0x6008, 0xFFFF00FFu);
    m(0x1720, 0x5000); m(0x1760, 2 | (3 << 4) | (12 << 8));
    m(0x172C, 0x6000); m(0x176C, 0 | (4 << 4) | (4 << 8));
    m(0x1744, 0);      m(0x1784, 2);                  /* no texcoords */
    draw(0, 3);
}

static void scene_16bit(void)
{
    surface(0, 1); reset_state(); program_state();
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF0000u, 0, 0);
    vertex(1, 0.9f, -0.6f, 0.5f, 0xFF00FF00u, 0, 0);
    vertex(2, 0.0f, 0.9f, 0.5f, 0xFF0000FFu, 0, 0);
    draw(0, 3);
}

/* 64x64 swizzled A8R8G8B8: a gradient, so a texel off by one is a small step. */
static void scene_texture(void)
{
    uint32_t x, y, b;

    surface(0, 0); reset_state(); program_state();
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            uint32_t at = 0;
            for (b = 0; b < 6; b++)
                at |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
            put32(TEX_VA + 4 * at, 0xFF000080u | (x * 4) << 16 | (y * 4) << 8);
        }
    m(0x1B00, TEX_VA);                                /* SET_TEXTURE_OFFSET(0) */
    m(0x1B04, 1u | 2u << 4 | 0x06u << 8 | 1u << 16 | 6u << 20 | 6u << 24);   /* 2D, SZ_A8R8G8B8, 64x64 */
    m(0x1B08, 3u | 3u << 8);                          /* clamp U, V */
    m(0x1B0C, 0x40000000u);                           /* enable */
    m(0x1B14, 1u << 16 | 1u << 24);                   /* point min / mag */
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFFFFFFu, 0.0f, 1.0f);
    vertex(1, 0.8f, -0.8f, 0.5f, 0xFFFFFFFFu, 1.0f, 1.0f);
    vertex(2, 0.8f, 0.8f, 0.5f, 0xFFC0C0C0u, 1.0f, 0.0f);
    vertex(3, -0.8f, -0.8f, 0.5f, 0xFFFFFFFFu, 0.0f, 1.0f);
    vertex(4, 0.8f, 0.8f, 0.5f, 0xFFC0C0C0u, 1.0f, 0.0f);
    vertex(5, -0.8f, 0.8f, 0.5f, 0xFFFFFFFFu, 0.0f, 0.0f);
    draw(0, 6);
}

/* A swizzled 256x256 render target (render-to-texture): the CPU rasteriser
 * keeps its pixels in Morton order, and so must the renderer's write-back,
 * or a title sampling it later reads blocks. The comparison is of guest
 * memory as it lies, so the layout itself is checked. */
static void scene_swizzled(void)
{
    m(0x0208, 0x200u | 0x20u | 0x08u | 8u << 16 | 8u << 24);   /* swizzled, Z24S8, A8R8G8B8, 2^8 x 2^8 */
    m(0x0200, 256u << 16);
    m(0x0204, 256u << 16);
    m(0x020C, 1024u | (1024u << 16));
    m(0x0210, COLOR_VA);
    m(0x0214, ZETA_VA);
    m(0x0398, f2u(16777215.0f));
    m(0x1D90, BACKGROUND);
    m(0x1D8C, 0xFFFFFF00u);
    m(0x1D94, 0xF3);
    reset_state(); program_state();
    constant(58, 128, -128, 16777215, 0);
    constant(59, 128, 128, 0, 0);
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF0000u, 0, 0);
    vertex(1, 0.9f, -0.6f, 0.5f, 0xFF00FF00u, 0, 0);
    vertex(2, 0.0f, 0.9f, 0.5f, 0xFF0000FFu, 0, 0);
    draw(0, 3);
}

/* Raw 2's render-to-texture sequence: draw into a swizzled 256x256 surface,
 * clear its depth through a linear view of the same memory (as the title
 * clears every render target), then draw it onto the screen as a swizzled
 * texture. The clear must not change how the surface is laid out. */
#define RTT_VA 0x300000u
static void scene_swizzled_sampled(void)
{
    m(0x0208, 0x200u | 0x20u | 0x08u | 8u << 16 | 8u << 24);   /* swizzled 256x256 */
    m(0x0200, 256u << 16);
    m(0x0204, 256u << 16);
    m(0x020C, 1024u | (1024u << 16));
    m(0x0210, RTT_VA);
    m(0x0214, ZETA_VA);
    m(0x0398, f2u(16777215.0f));
    m(0x1D90, 0xFF000000u);
    m(0x1D8C, 0xFFFFFF00u);
    m(0x1D94, 0xF3);
    reset_state(); program_state();
    /* The executor keeps the last texture bound whatever SET_TEXTURE_CONTROL0
     * says, so bind one: else this triangle samples whatever the previous run
     * left there (the CPU run ends with the render target itself bound). */
    m(0x1B00, TEX_VA);
    m(0x1B04, 1u | 2u << 4 | 0x06u << 8 | 1u << 16 | 6u << 20 | 6u << 24);   /* scene_texture's 64x64 */
    constant(58, 128, -128, 16777215, 0);
    constant(59, 128, 128, 0, 0);
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF0000u, 0, 0);
    vertex(1, 0.9f, -0.6f, 0.5f, 0xFF00FF00u, 0, 0);
    vertex(2, 0.0f, 0.9f, 0.5f, 0xFF0000FFu, 0, 0);
    draw(0, 3);
    m(0x0208, 0x100u | 0x20u | 0x08u);                /* the same memory, linear */
    m(0x1D94, 0x03);                                  /* depth and stencil only */
    surface(0, 0); reset_state(); program_state();
    m(0x1B00, RTT_VA);
    m(0x1B04, 1u | 2u << 4 | 0x06u << 8 | 1u << 16 | 8u << 20 | 8u << 24);   /* SZ_A8R8G8B8 256x256 */
    m(0x1B08, 3u | 3u << 8);
    m(0x1B0C, 0x40000000u);
    m(0x1B14, 1u << 16 | 1u << 24);
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFFFFFFu, 0.0f, 1.0f);
    vertex(1, 0.8f, -0.8f, 0.5f, 0xFFFFFFFFu, 1.0f, 1.0f);
    vertex(2, 0.8f, 0.8f, 0.5f, 0xFFFFFFFFu, 1.0f, 0.0f);
    vertex(3, -0.8f, -0.8f, 0.5f, 0xFFFFFFFFu, 0.0f, 1.0f);
    vertex(4, 0.8f, 0.8f, 0.5f, 0xFFFFFFFFu, 1.0f, 0.0f);
    vertex(5, -0.8f, 0.8f, 0.5f, 0xFFFFFFFFu, 0.0f, 0.0f);
    draw(0, 6);
}

/* ---- register combiner scenes. They run last: once a title programs the
 * combiners the executor draws every later batch through them. */

/* Texture stage `st` at `va`: 64x64 swizzled A8R8G8B8 from `texel(x, y)`. */
static void stage_texture(uint32_t st, uint32_t va, uint32_t (*texel)(uint32_t, uint32_t), uint32_t filter)
{
    uint32_t x, y, b, r = 0x1B00 + 0x40 * st;

    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            uint32_t at = 0;
            for (b = 0; b < 6; b++)
                at |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
            put32(va + 4 * at, texel(x, y));
        }
    m(r + 0x00, va);
    m(r + 0x04, 1u | 2u << 4 | 0x06u << 8 | 1u << 16 | 6u << 20 | 6u << 24);
    m(r + 0x08, 3u | 3u << 8);                        /* clamp U, V */
    m(r + 0x0C, 0x40000000u);
    m(r + 0x14, filter);
}

static uint32_t tex_gradient(uint32_t x, uint32_t y) { return 0xFF000080u | (x * 4) << 16 | (y * 4) << 8; }
static uint32_t tex_checker(uint32_t x, uint32_t y) { return ((x ^ y) & 8) ? 0xFFFFFFFFu : 0xFF8040C0u; }
static uint32_t tex_alpha_ramp(uint32_t x, uint32_t y) { return (x * 4) << 24 | (y * 4) << 16 | 0x8040u; }

/* A quad over most of the surface, uv 0..1 on v9, a colour per corner. */
static void combiner_quad(void)
{
    vertex(0, -0.8f, -0.8f, 0.5f, 0xFFFF8040u, 0.0f, 1.0f);
    vertex(1, 0.8f, -0.8f, 0.5f, 0xFF40FF80u, 1.0f, 1.0f);
    vertex(2, 0.8f, 0.8f, 0.5f, 0xFF8040FFu, 1.0f, 0.0f);
    vertex(3, -0.8f, -0.8f, 0.5f, 0xFFFF8040u, 0.0f, 1.0f);
    vertex(4, 0.8f, 0.8f, 0.5f, 0xFF8040FFu, 1.0f, 0.0f);
    vertex(5, -0.8f, 0.8f, 0.5f, 0x80FFFFFFu, 0.0f, 0.0f);
    draw(0, 6);
}

/* Two textures: r0.rgb = (t0 * t1 + v0) / 2, r1.rgb = t0 * t1 / 2,
 * r0.a = t0.a * v0.a; no final combiner, so the pixel is r0. Stage 1 is
 * bilinear. */
static void scene_rc_two_textures(void)
{
    surface(0, 0); reset_state(); program_state();
    stage_texture(0, TEX_VA, tex_gradient, 1u << 16 | 1u << 24);
    stage_texture(1, TEX_VA + 0x10000, tex_checker, 2u << 16 | 2u << 24);
    m(0x1E70, 1u | 1u << 5);                          /* stages 0, 1: PROJECT2D */
    m(0x0AC0, 0x08u << 24 | 0x09u << 16 | 0x04u << 8 | 0x20u);   /* A t0, B t1, C v0, D 1 */
    m(0x1E40, 0x30u << 12 | 12u << 8 | 13u << 4);   /* half; sum -> r0, AB -> r1 */
    m(0x0260, 0x18u << 24 | 0x14u << 16);            /* t0.a * v0.a */
    m(0x0AA0, 12u << 8);                              /* sum -> r0.a */
    m(0x0288, 0); m(0x028C, 0);
    m(0x1E60, 1);                                     /* one general stage */
    combiner_quad();
}

/* The final combiner mixing in linear fog by oFog.x (= x): fog colour where
 * the factor is 0, r0 = t0 * v0 where it is 1. */
static void scene_rc_fog(void)
{
    surface(0, 0); reset_state(); program_state();
    stage_texture(0, TEX_VA, tex_gradient, 1u << 16 | 1u << 24);
    m(0x1B4C, 0);                                     /* stage 1 off */
    m(0x1E70, 1u);
    m(0x0AC0, 0x08u << 24 | 0x04u << 16);            /* t0 * v0 */
    m(0x1E40, 12u << 4);                              /* AB -> r0 */
    m(0x0260, 0x18u << 24 | 0x30u << 16);            /* t0.a * 1 */
    m(0x0AA0, 12u << 4);
    m(0x0288, 0x13u << 24 | 0x0Cu << 16 | 0x03u << 8);   /* A fog.a, B r0, C fog, D 0 */
    m(0x028C, 0x1Cu << 8 | 0x80u);                   /* G r0.a; clamp the V1+R0 sum */
    m(0x02A4, 1);                                     /* fog on, linear */
    m(0x029C, 0x2601);
    m(0x09C0, f2u(1.5f)); m(0x09C4, f2u(0.5f));      /* factor 0.5 + 0.5 x */
    m(0x02A8, 0x00FF8000u);                           /* R 0x00, G 0x80, B 0xFF */
    m(0x1E60, 1);
    combiner_quad();
    m(0x02A4, 0);
}

/* Two stages with per-stage constants, a dot product, expand-normal and the
 * MSB mux on r0.a (from a texture alpha ramp):
 * stage 0: r1.rgb = dot(c0, expand(v0)); r0.rgb = t0 * c1; r0.a = t0.a
 * stage 1: r0.rgb = r0.a >= 0.5 ? r0 : r1 */
static void scene_rc_dot_mux(void)
{
    surface(0, 0); reset_state(); program_state();
    stage_texture(0, TEX_VA, tex_alpha_ramp, 1u << 16 | 1u << 24);
    m(0x1B4C, 0);
    m(0x1E70, 1u);
    m(0x0A60, 0xFF8040C0u); m(0x0A64, 0xFF20E060u);  /* c0 per stage */
    m(0x0A80, 0xFFC0A080u); m(0x0A84, 0xFF000000u);  /* c1 (stage 0's used) */
    m(0x0AC0, 0x01u << 24 | 0x44u << 16 | 0x08u << 8 | 0x02u);   /* c0 . expand(v0); t0 * c1 */
    m(0x1E40, 2u << 12 | 13u << 4 | 12u);            /* AB dot -> r1, CD -> r0 */
    m(0x0260, 0x18u << 24 | 0x30u << 16);
    m(0x0AA0, 12u << 4);
    m(0x0AC4, 0x0Du << 24 | 0x20u << 16 | 0x0Cu << 8 | 0x20u);   /* r1 * 1, r0 * 1 */
    m(0x1E44, 4u << 12 | 12u << 8);                  /* mux -> r0 */
    m(0x0264, 0x1Cu << 24 | 0x20u << 16);
    m(0x0AA4, 12u << 4);
    m(0x0288, 0); m(0x028C, 0);
    m(0x1E60, 2u | (0x001u | 0x010u) << 8);          /* MSB mux, c0 per stage */
    combiner_quad();
}

/* The scene's surface in guest memory, as 0x00RRGGBB per pixel. */
static void snapshot(uint32_t *out, int aa, int bpp16)
{
    uint32_t w = aa ? 1280 : 640, h = aa ? 960 : 480, x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            if (bpp16) {
                uint16_t v;
                memcpy(&v, s_mem + COLOR_VA + (y * w + x) * 2, 2);
                out[y * w + x] = ((v >> 11) & 31u) << 19 | ((v >> 5) & 63u) << 10 | (v & 31u) << 3;
            } else {
                uint32_t v;
                memcpy(&v, s_mem + COLOR_VA + (y * w + x) * 4, 4);
                out[y * w + x] = v & 0x00FFFFFFu;
            }
        }
}

/* programs: the renderer takes vertex-program batches (0: the executor
 * transforms them and hands over surface pixels). */
static void run_as(const char *name, Scene scene, int aa, int bpp16, int programs)
{
    static uint32_t cpu[1280 * 960], gpu[1280 * 960];
    uint32_t n = (aa ? 1280u : 640u) * (aa ? 960u : 480u), i, bad = 0, drawn_cpu = 0, drawn_gpu = 0;
    uint32_t bg = bpp16 ? 0x00203840u : (BACKGROUND & 0x00FFFFFFu);
    int worst = 0;

    nv2a_backend_register(NULL);                      /* the CPU rasteriser */
    memset(s_mem + COLOR_VA, 0xCD, 1280 * 960 * 4);
    scene();
    m(0x0130, 0);                                     /* FLIP_STALL */
    snapshot(cpu, aa, bpp16);

    if (!nv2a_hw_d3d11_register(1)) {                 /* the GPU, on WARP */
        printf("FAIL %s: no D3D11 device\n", name);
        s_fail = 1;
        return;
    }
    nv2a_backend_set_program_batches(programs);
    memset(s_mem + COLOR_VA, 0xCD, 1280 * 960 * 4);
    scene();
    m(0x0130, 0);                                     /* present_sync queues the GPU copy ... */
    m(0x0130, 0);                                     /* ... and the next one writes it to guest memory */
    snapshot(gpu, aa, bpp16);

    for (i = 0; i < n; i++) {
        int k, d = 0;
        for (k = 0; k < 24; k += 8) {
            int e = abs((int)((cpu[i] >> k) & 0xFF) - (int)((gpu[i] >> k) & 0xFF));
            if (e > d) d = e;
        }
        if (d > worst) worst = d;
        if (d > 8) bad++;
        drawn_cpu += cpu[i] != bg;
        drawn_gpu += gpu[i] != bg;
    }
    printf("%-15s %7u px, CPU drew %6u, GPU drew %6u, %5u differ (%.3f%%), worst channel %d\n",
           name, n, drawn_cpu, drawn_gpu, bad, 100.0 * bad / n, worst);
    if (!drawn_cpu || !drawn_gpu || bad * 100 > n) {
        printf("FAIL %s\n", name);
        s_fail = 1;
    }
}

static void run(const char *name, Scene scene, int aa, int bpp16)
{
    run_as(name, scene, aa, bpp16, 1);
}

int main(void)
{
    uint32_t st[6];

    s_mem = (uint8_t *)calloc(MEM_BYTES, 1);
    if (!s_mem)
        return 2;
    run("gouraud", scene_gouraud, 0, 0);
    run("aa 2x2", scene_aa, 1, 0);
    run("depth", scene_depth, 0, 0);
    run("narrow clip", scene_narrow_clip, 0, 0);
    run("cull strip", scene_cull_strip, 0, 0);
    run("blend", scene_blend, 0, 0);
    run("fixed-function", scene_fixed_function, 0, 0);
    run("16-bit", scene_16bit, 0, 1);
    run("texture", scene_texture, 0, 0);
    run("swizzled RT", scene_swizzled, 0, 0);
    run("RT clear+sample", scene_swizzled_sampled, 0, 0);
    run("rc 2 textures", scene_rc_two_textures, 0, 0);
    run_as("rc 2 tex (xf)", scene_rc_two_textures, 0, 0, 0);
    run("rc fog", scene_rc_fog, 0, 0);
    run_as("rc fog (xf)", scene_rc_fog, 0, 0, 0);
    run("rc dot mux", scene_rc_dot_mux, 0, 0);
    run_as("rc dot mux (xf)", scene_rc_dot_mux, 0, 0, 0);
    nv2a_hw_d3d11_stats(st);
    printf("renderer: %u draws, %u program batches, %u programs compiled, %u loads, %u write-backs, %u textures\n",
           st[0], st[1], st[2], st[3], st[4], st[5]);
    printf("%s\n", s_fail ? "FAIL" : "nv2a D3D11 renderer against the CPU rasteriser: ok");
    return s_fail;
}
