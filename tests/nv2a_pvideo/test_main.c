/* PVIDEO, the NV2A video overlay: the scanout that draws a YUY2 bank into
 * the framebuffer, and the register model the title's D3D drives it with.
 *
 *   - scanout_*: pvideo_scanout converts, places and scales a bank, honours
 *     the colour key, and refuses what it cannot draw.
 *   - stop_*: STOP is a command. The title waits for BUFFER's pending bits to
 *     clear after it; on main STOP was stored and BUFFER kept its bits.
 *   - consume_*: the scanout hands a bank back by clearing only its bit.
 *
 * Run with: ctest --test-dir <build> */
#include "../../src/video/pvideo_scanout.h"
#include "nv2a_state.h"
#include "nv2a_mmio_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int s_failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

#define W 8
#define H 4
#define PVIDEO 0x8000u        /* PVIDEO's block in the register aperture */

static uint32_t s_regs[0x1000 / 4];
static uint32_t s_dst[W * H];

#define REG(r) s_regs[(r) / 4]

/* Bank 0: a 4x2 YUY2 image drawn 1:1 at (1,1). Row 0 is white, row 1 black;
 * the right pixel pair of row 0 is pure blue (Y 41, U 240, V 110). */
static void setup_bank0(uint8_t *yuy2)
{
    static const uint8_t white[4] = { 235, 128, 235, 128 };
    static const uint8_t black[4] = { 16, 128, 16, 128 };
    static const uint8_t blue[4] = { 41, 240, 41, 110 };

    memset(s_regs, 0, sizeof(s_regs));
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    REG(NV_PVIDEO_FORMAT) = (NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 << 16) | 8;
    REG(NV_PVIDEO_SIZE_IN) = (2u << 16) | 4u;
    REG(NV_PVIDEO_POINT_OUT) = (1u << 16) | 1u;
    REG(NV_PVIDEO_SIZE_OUT) = (2u << 16) | 4u;
    REG(NV_PVIDEO_DS_DX) = NV_PVIDEO_DIN_DOUT_UNITY;
    REG(NV_PVIDEO_DT_DY) = NV_PVIDEO_DIN_DOUT_UNITY;
    REG(NV_PVIDEO_LIMIT) = 0xFFFFFFFFu;
    memcpy(yuy2 + 0, white, 4);
    memcpy(yuy2 + 4, blue, 4);
    memcpy(yuy2 + 8, black, 4);
    memcpy(yuy2 + 12, black, 4);
    for (int i = 0; i < W * H; i++)
        s_dst[i] = 0x12345678u;
}

static uint32_t px(int x, int y) { return s_dst[y * W + x]; }

static void scanout_converts_and_places(void)
{
    uint8_t yuy2[16];

    setup_bank0(yuy2);
    CHECK(pvideo_scanout(s_regs, yuy2, sizeof(yuy2), s_dst, W, H, 0),
          "a pending YUY2 bank was refused");
    CHECK(px(1, 1) == 0xFFFFFFFFu && px(2, 1) == 0xFFFFFFFFu,
          "white came out %08X %08X", px(1, 1), px(2, 1));
    CHECK(px(3, 1) == 0xFF0000FFu, "blue came out %08X", px(3, 1));
    CHECK(px(1, 2) == 0xFF000000u && px(4, 2) == 0xFF000000u,
          "black came out %08X %08X", px(1, 2), px(4, 2));
    CHECK(px(0, 1) == 0x12345678u && px(5, 1) == 0x12345678u &&
          px(1, 0) == 0x12345678u && px(1, 3) == 0x12345678u,
          "pixels outside the output rectangle changed");
}

static void scanout_scales(void)
{
    uint8_t yuy2[16];

    /* Twice as tall on screen: each source row covers two output rows. */
    setup_bank0(yuy2);
    REG(NV_PVIDEO_POINT_OUT) = 0;
    REG(NV_PVIDEO_SIZE_OUT) = (4u << 16) | 4u;
    REG(NV_PVIDEO_DT_DY) = NV_PVIDEO_DIN_DOUT_UNITY / 2;
    CHECK(pvideo_scanout(s_regs, yuy2, sizeof(yuy2), s_dst, W, H, 0),
          "a scaled bank was refused");
    CHECK(px(0, 0) == 0xFFFFFFFFu && px(0, 1) == 0xFFFFFFFFu &&
          px(0, 2) == 0xFF000000u && px(0, 3) == 0xFF000000u,
          "rows did not double: %08X %08X %08X %08X",
          px(0, 0), px(0, 1), px(0, 2), px(0, 3));
}

static void scanout_honours_color_key(void)
{
    uint8_t yuy2[16];

    setup_bank0(yuy2);
    REG(NV_PVIDEO_FORMAT) |= NV_PVIDEO_FORMAT_DISPLAY;
    REG(NV_PVIDEO_COLOR_KEY) = 0x00FF00FFu;
    s_dst[1 * W + 1] = 0xFFFF00FFu;            /* only this pixel is keyed */
    CHECK(pvideo_scanout(s_regs, yuy2, sizeof(yuy2), s_dst, W, H, 0),
          "a colour-keyed bank was refused");
    CHECK(px(1, 1) == 0xFFFFFFFFu, "the keyed pixel was not drawn");
    CHECK(px(2, 1) == 0x12345678u, "an unkeyed pixel was drawn over");
}

static void scanout_refuses(void)
{
    uint8_t yuy2[16];

    setup_bank0(yuy2);
    REG(NV_PVIDEO_BUFFER) = 0;
    CHECK(!pvideo_scanout(s_regs, yuy2, sizeof(yuy2), s_dst, W, H, 0),
          "drew a bank that is not pending");
    setup_bank0(yuy2);
    REG(NV_PVIDEO_STOP) = 1;
    CHECK(!pvideo_scanout(s_regs, yuy2, sizeof(yuy2), s_dst, W, H, 0),
          "drew while stopped");
    setup_bank0(yuy2);
    CHECK(!pvideo_scanout(s_regs, yuy2, sizeof(yuy2) - 1, s_dst, W, H, 0),
          "drew from fewer bytes than the image holds");
    setup_bank0(yuy2);
    REG(NV_PVIDEO_FORMAT) &= ~NV_PVIDEO_FORMAT_COLOR;
    CHECK(!pvideo_scanout(s_regs, yuy2, sizeof(yuy2), s_dst, W, H, 0),
          "drew a format other than YUY2");
    CHECK(px(1, 1) == 0x12345678u, "a refused bank drew anyway");
}

static void stop_clears_pending_banks(NV2AState *d)
{
    nv2a_mmio_write(d, PVIDEO + NV_PVIDEO_BUFFER,
                    NV_PVIDEO_BUFFER_0_USE | NV_PVIDEO_BUFFER_1_USE, 4);
    nv2a_mmio_write(d, PVIDEO + NV_PVIDEO_STOP, 1, 4);
    CHECK(nv2a_mmio_read(d, PVIDEO + NV_PVIDEO_BUFFER, 4) == 0,
          "BUFFER kept %08llX after STOP",
          (unsigned long long)nv2a_mmio_read(d, PVIDEO + NV_PVIDEO_BUFFER, 4));
    CHECK(nv2a_mmio_read(d, PVIDEO + NV_PVIDEO_STOP, 4) == 0,
          "STOP was stored");
}

static void consume_clears_its_bank(NV2AState *d)
{
    uint32_t regs[0x1000 / 4];

    nv2a_mmio_write(d, PVIDEO + NV_PVIDEO_BUFFER,
                    NV_PVIDEO_BUFFER_0_USE | NV_PVIDEO_BUFFER_1_USE, 4);
    nv2a_hook_pvideo_consume(0);
    CHECK(nv2a_hook_pvideo_snapshot(regs), "no snapshot with a model");
    CHECK(regs[NV_PVIDEO_BUFFER / 4] == NV_PVIDEO_BUFFER_1_USE,
          "consuming bank 0 left BUFFER %08X", regs[NV_PVIDEO_BUFFER / 4]);
}

int main(void)
{
    static uint8_t vram[0x10000], ramin[0x10000];
    NV2AState *d;

    scanout_converts_and_places();
    scanout_scales();
    scanout_honours_color_key();
    scanout_refuses();

    d = nv2a_init_standalone(vram, sizeof(vram), ramin, sizeof(ramin));
    if (!d) { fprintf(stderr, "no NV2A model\n"); return 2; }
    stop_clears_pending_banks(d);
    consume_clears_its_bank(d);

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: PVIDEO scanout draws, scales and keys; STOP clears pending "
         "banks; consume clears only its bank.");
    return 0;
}
