/*
 * NV2A Push Buffer Test
 *
 * Generates NV2A GPU commands directly into the push buffer to validate
 * the push buffer → command parser → PGRAPH method dispatch pipeline.
 *
 * Phase 1: Basic commands (NOP, surface setup, clear)
 * Phase 2: Full frame sequence (viewport, transforms, clear color,
 *          begin/end draw, inline vertex data)
 */

#include "nv2a_state.h"
#include <stdio.h>
#include <math.h>

/* Xbox memory access (from recomp_types.h) */
extern ptrdiff_t g_xbox_mem_offset;
#define NV2A_TEST_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define NV2A_TEST_MEM32(addr) (*(volatile uint32_t *)NV2A_TEST_PTR(addr))

/* Push buffer pointers - game-specific, override before including.
 * Default values are from Burnout 3 as reference. Your game will
 * have different addresses - find them via xemu GDB debugging. */
#ifndef PB_BASE_ADDR
#define PB_BASE_ADDR  0x35D69C
#endif
#ifndef PB_WRITE_ADDR
#define PB_WRITE_ADDR 0x35D6A0
#endif
#ifndef PB_END_ADDR
#define PB_END_ADDR   0x35D6A4
#endif

/* NV2A Kelvin (NV097) method addresses.
 * Use nv2a_regs.h if available; these are fallbacks for standalone use. */
#include "nv2a_regs.h"

/* All NV097 defines come from nv2a_regs.h above.
 * Supplemental defines not in the register file: */
#ifndef NV097_SET_BEGIN_END_OP_END
#define NV097_SET_BEGIN_END_OP_END      0x00
#endif
#ifndef NV097_SET_BEGIN_END_OP_TRIANGLES
#define NV097_SET_BEGIN_END_OP_TRIANGLES 0x04
#endif

/* Push buffer command encoding */
#define PB_METHOD_INC(subchannel, method, count) \
    (((count) << 18) | ((subchannel) << 13) | (method))
#define PB_METHOD_NON_INC(subchannel, method, count) \
    (0x40000000 | ((count) << 18) | ((subchannel) << 13) | (method))

/* Float-as-uint32 helper */
static uint32_t f2u(float f) {
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

static int g_pb_test_frame = 0;
static int g_pb_test_active = 0;

/*
 * Generate a complete frame of NV2A commands.
 * This mimics what the Xbox D3D8 runtime would generate for:
 *   1. Set viewport (640x480)
 *   2. Set clear color (cycling hue)
 *   3. Clear framebuffer
 *   4. Set render state (no depth test, no blending)
 *   5. Set combiner for passthrough (vertex color only)
 *   6. Draw a triangle with inline vertex data
 *   7. Flip
 *
 * Returns number of dwords written.
 */
static uint32_t nv2a_generate_frame_commands(uint32_t *buf, int frame)
{
    uint32_t *p = buf;

    /* ── Surface setup ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_SURFACE_FORMAT, 3);
    *p++ = 0x00000121;                          /* X8R8G8B8, Z24S8 */
    *p++ = (640 * 4) | ((640 * 4) << 16);       /* color | zeta pitch */
    *p++ = 0;                                   /* color offset */

    *p++ = PB_METHOD_INC(0, NV097_SET_SURFACE_ZETA_OFFSET, 1);
    *p++ = 640 * 480 * 4;                       /* zeta after color */

    /* ── Viewport clip ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_SURFACE_CLIP_HORIZONTAL, 2);
    *p++ = (640 << 16) | 0;                     /* width << 16 | x */
    *p++ = (480 << 16) | 0;                     /* height << 16 | y */

    /* ── Viewport transform ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_VIEWPORT_OFFSET, 4);
    *p++ = f2u(320.0f);     /* offset X */
    *p++ = f2u(240.0f);     /* offset Y */
    *p++ = f2u(0.0f);       /* offset Z */
    *p++ = f2u(0.0f);       /* offset W */

    *p++ = PB_METHOD_INC(0, NV097_SET_VIEWPORT_SCALE, 4);
    *p++ = f2u(320.0f);     /* scale X */
    *p++ = f2u(-240.0f);    /* scale Y (inverted) */
    *p++ = f2u(16777215.0f); /* scale Z (24-bit depth) */
    *p++ = f2u(0.0f);       /* scale W */

    /* ── Clear color (cycle hue over time) ── */
    {
        float t = (float)(frame % 360) / 360.0f;
        /* Simple RGB cycle: R→G→B */
        uint8_t r = (uint8_t)(sinf(t * 6.283185f) * 127.0f + 128.0f);
        uint8_t g = (uint8_t)(sinf(t * 6.283185f + 2.094395f) * 127.0f + 128.0f);
        uint8_t b = (uint8_t)(sinf(t * 6.283185f + 4.188790f) * 127.0f + 128.0f);
        uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

        *p++ = PB_METHOD_INC(0, NV097_SET_COLOR_CLEAR_VALUE, 1);
        *p++ = color;
    }

    /* ── Clear rect ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_CLEAR_RECT_HORIZONTAL, 2);
    *p++ = (640 << 16) | 0;
    *p++ = (480 << 16) | 0;

    /* ── Clear surface (color + zeta) ── */
    *p++ = PB_METHOD_INC(0, NV097_CLEAR_SURFACE, 1);
    *p++ = 0x000000F0;  /* F0 = clear color R,G,B,A + Z + stencil */

    /* ── Render state setup ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_DEPTH_TEST_ENABLE, 1);
    *p++ = 0;   /* depth test off */

    *p++ = PB_METHOD_INC(0, NV097_SET_BLEND_ENABLE, 1);
    *p++ = 0;   /* blending off */

    *p++ = PB_METHOD_INC(0, NV097_SET_CULL_FACE_ENABLE, 1);
    *p++ = 0;   /* culling off */

    *p++ = PB_METHOD_INC(0, NV097_SET_ALPHA_TEST_ENABLE, 1);
    *p++ = 0;

    *p++ = PB_METHOD_INC(0, NV097_SET_COLOR_MASK, 1);
    *p++ = 0x01010101; /* write R,G,B,A */

    *p++ = PB_METHOD_INC(0, NV097_SET_SHADE_MODE, 1);
    *p++ = 2;   /* D3DSHADE_GOURAUD */

    /* ── Transform: fixed-function passthrough ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_TRANSFORM_EXECUTION_MODE, 1);
    *p++ = 0;   /* fixed function mode */

    /* ── Combiner setup: passthrough vertex color ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_COMBINER_CONTROL, 1);
    *p++ = 1;   /* 1 stage */

    *p++ = PB_METHOD_INC(0, NV097_SET_SHADER_STAGE_PROGRAM, 1);
    *p++ = 0;   /* program 0 = passthrough */

    *p++ = PB_METHOD_INC(0, NV097_SET_COMBINER_COLOR_ICW, 1);
    *p++ = 0x04200000; /* diffuse color pass-through */

    *p++ = PB_METHOD_INC(0, NV097_SET_COMBINER_ALPHA_ICW, 1);
    *p++ = 0x14200000; /* diffuse alpha pass-through */

    *p++ = PB_METHOD_INC(0, NV097_SET_COMBINER_COLOR_OCW, 1);
    *p++ = 0x00000C00; /* output AB */

    *p++ = PB_METHOD_INC(0, NV097_SET_COMBINER_ALPHA_OCW, 1);
    *p++ = 0x00000C00;

    /* ── Draw a rotating triangle ── */
    /* Begin: triangle list */
    *p++ = PB_METHOD_INC(0, NV097_SET_BEGIN_END, 1);
    *p++ = NV097_SET_BEGIN_END_OP_TRIANGLES;

    /* Inline vertex data: 3 vertices, each = {x, y, z, w, color}
     * Using NV097_INLINE_ARRAY (non-incrementing method) */
    {
        float angle = (float)(frame % 360) * 3.14159f / 180.0f;
        float cx = 320.0f, cy = 240.0f, radius = 150.0f;

        /* 3 vertices with screen-space coords and ARGB colors */
        struct { float x, y, z, w; uint32_t color; } verts[3];

        verts[0].x = cx + radius * sinf(angle);
        verts[0].y = cy - radius * cosf(angle);
        verts[0].z = 0.5f;
        verts[0].w = 1.0f;
        verts[0].color = 0xFFFF0000; /* red */

        verts[1].x = cx + radius * sinf(angle + 2.094395f);
        verts[1].y = cy - radius * cosf(angle + 2.094395f);
        verts[1].z = 0.5f;
        verts[1].w = 1.0f;
        verts[1].color = 0xFF00FF00; /* green */

        verts[2].x = cx + radius * sinf(angle + 4.188790f);
        verts[2].y = cy - radius * cosf(angle + 4.188790f);
        verts[2].z = 0.5f;
        verts[2].w = 1.0f;
        verts[2].color = 0xFF0000FF; /* blue */

        /* Each vertex is 5 dwords (pos XYZW + color) */
        *p++ = PB_METHOD_NON_INC(0, NV097_INLINE_ARRAY, 15);
        for (int v = 0; v < 3; v++) {
            *p++ = f2u(verts[v].x);
            *p++ = f2u(verts[v].y);
            *p++ = f2u(verts[v].z);
            *p++ = f2u(verts[v].w);
            *p++ = verts[v].color;
        }
    }

    /* End */
    *p++ = PB_METHOD_INC(0, NV097_SET_BEGIN_END, 1);
    *p++ = NV097_SET_BEGIN_END_OP_END;

    /* ── Flip ── */
    *p++ = PB_METHOD_INC(0, NV097_SET_FLIP_READ, 1);
    *p++ = 0;
    *p++ = PB_METHOD_INC(0, NV097_SET_FLIP_WRITE, 1);
    *p++ = 0;
    *p++ = PB_METHOD_INC(0, NV097_SET_FLIP_MODULO, 1);
    *p++ = 1;
    *p++ = PB_METHOD_INC(0, NV097_FLIP_INCREMENT_WRITE, 1);
    *p++ = 0;

    return (uint32_t)(p - buf);
}

/*
 * Called once per frame to inject test commands into the push buffer.
 */
void nv2a_pb_test_frame(void)
{
    if (!g_pb_test_active) return;

    g_pb_test_frame++;

    uint32_t base = NV2A_TEST_MEM32(PB_BASE_ADDR);
    uint32_t write_ptr = NV2A_TEST_MEM32(PB_WRITE_ADDR);
    uint32_t end_ptr = NV2A_TEST_MEM32(PB_END_ADDR);

    if (base == 0 || end_ptr == 0) return;

    /* Need ~512 bytes for the full frame command sequence */
    if (write_ptr + 512 >= end_ptr) {
        NV2A_TEST_MEM32(PB_WRITE_ADDR) = base;
        write_ptr = base;
    }

    /* Write test commands into push buffer */
    uint32_t *buf = (uint32_t *)NV2A_TEST_PTR(write_ptr);
    uint32_t dwords = nv2a_generate_frame_commands(buf, g_pb_test_frame);

    /* Advance write pointer */
    uint32_t new_write = write_ptr + dwords * 4;
    NV2A_TEST_MEM32(PB_WRITE_ADDR) = new_write;

    /* Log first few frames and periodically */
    if (g_pb_test_frame <= 3 || (g_pb_test_frame % 300) == 0) {
        fprintf(stderr, "[PB-TEST] Frame %d: wrote %u dwords (%u bytes) at 0x%08X\n",
                g_pb_test_frame, dwords, dwords * 4, write_ptr);
        fflush(stderr);
    }

    /* Parse and dispatch commands through PGRAPH */
    {
        NV2AState *nv2a = nv2a_get_state();
        if (!nv2a) return;

        uint32_t pos = write_ptr;
        while (pos < new_write) {
            uint32_t word = NV2A_TEST_MEM32(pos);
            pos += 4;

            if ((word & 0xe0030003) == 0) {
                /* increasing methods */
                uint32_t count = (word >> 18) & 0x7ff;
                uint32_t method = word & 0x1ffc;
                uint32_t subchan = (word >> 13) & 7;

                for (uint32_t i = 0; i < count && pos < new_write; i++) {
                    uint32_t param = NV2A_TEST_MEM32(pos);
                    pos += 4;
                    pgraph_method(nv2a, subchan, method + i * 4, param);
                }
            } else if ((word & 0xe0030003) == 0x40000000) {
                /* non-increasing methods */
                uint32_t count = (word >> 18) & 0x7ff;
                uint32_t method = word & 0x1ffc;
                uint32_t subchan = (word >> 13) & 7;

                for (uint32_t i = 0; i < count && pos < new_write; i++) {
                    uint32_t param = NV2A_TEST_MEM32(pos);
                    pos += 4;
                    pgraph_method(nv2a, subchan, method, param);
                }
            } else {
                break;
            }
        }
    }
}

/*
 * Enable/disable the push buffer test.
 */
void nv2a_pb_test_set_active(int active)
{
    g_pb_test_active = active;
    g_pb_test_frame = 0;
    fprintf(stderr, "[PB-TEST] Push buffer test %s\n", active ? "ENABLED" : "disabled");
}

int nv2a_pb_test_is_active(void)
{
    return g_pb_test_active;
}
