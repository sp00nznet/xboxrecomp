/*
 * d3d8_smoke - smoke-test harness for the xbox_d3d8 pure format layer.
 *
 * Exercises the real format tables, predicates, swizzle classification and
 * software conversions without needing a D3D11 device:
 *   - d3d8_to_dxgi_format, d3d8_format_bpp, d3d8_format_is_*
 *   - d3d8_format_is_swizzled / unswizzle round-trip (d3d8_swizzle.h)
 *   - d3d8_convert_linear_pixels (channel swaps, sign extension)
 *
 * The D3D8 resource file is compiled alongside this driver; the handful of
 * device-accessor functions it references are satisfied by the stubs below.
 */

#define COBJMACROS
#include "d3d8_internal.h"
#include "d3d8_swizzle.h"
#include "d3d8_fvf.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- Stub accessors referenced by d3d8_resources.c (not used by the
 *      pure functions under test, but must link). ---- */
ID3D11Device        *d3d8_GetD3D11Device(void)   { return NULL; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void)  { return NULL; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return NULL; }
IDirect3DDevice8      *xbox_GetD3DDevice(void)  { return NULL; }

static DWORD g_palette[256];
const DWORD *d3d8_GetPalette(DWORD stage) { (void)stage; return g_palette; }

static int failures = 0;

#define CHECK(name, cond) \
    do { if (cond) { /* pass */ } else { printf("FAIL: %s\n", name); failures++; } } while (0)

#define CHECK_INT(name, got, expected) \
    do { if ((got) == (expected)) { } else { printf("FAIL: %s (got %d, want %d)\n", name, (int)(got), (int)(expected)); failures++; } } while (0)

#define CHECK_FMT(name, got, expected) \
    do { if ((got) == (expected)) { } else { printf("FAIL: %s (got %d, want %d)\n", name, (int)(got), (int)(expected)); failures++; } } while (0)

static void test_to_dxgi(void)
{
    printf("test_to_dxgi\n");
    CHECK_FMT("R16F",      d3d8_to_dxgi_format(D3DFMT_R16F),          DXGI_FORMAT_R16_FLOAT);
    CHECK_FMT("R32F",      d3d8_to_dxgi_format(D3DFMT_R32F),          DXGI_FORMAT_R32_FLOAT);
    CHECK_FMT("G16R16F",   d3d8_to_dxgi_format(D3DFMT_G16R16F),       DXGI_FORMAT_R16G16_FLOAT);
    CHECK_FMT("G32R32F",   d3d8_to_dxgi_format(D3DFMT_G32R32F),       DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("A16B16G16R16F", d3d8_to_dxgi_format(D3DFMT_A16B16G16R16F), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK_FMT("A32B32G32R32F", d3d8_to_dxgi_format(D3DFMT_A32B32G32R32F), DXGI_FORMAT_R32G32B32A32_FLOAT);
    CHECK_FMT("G16R16",    d3d8_to_dxgi_format(D3DFMT_G16R16),        DXGI_FORMAT_R16G16_UNORM);
    CHECK_FMT("A16L16",    d3d8_to_dxgi_format(D3DFMT_A16L16),        DXGI_FORMAT_R16G16_UNORM);
    CHECK_FMT("A16B16G16R16", d3d8_to_dxgi_format(D3DFMT_A16B16G16R16), DXGI_FORMAT_R16G16B16A16_UNORM);
    CHECK_FMT("A32B32G32R32", d3d8_to_dxgi_format(D3DFMT_A32B32G32R32), DXGI_FORMAT_R32G32B32A32_FLOAT);
    CHECK_FMT("G32R32",    d3d8_to_dxgi_format(D3DFMT_G32R32),        DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("L32",       d3d8_to_dxgi_format(D3DFMT_L32),           DXGI_FORMAT_R32_FLOAT);
    CHECK_FMT("A32L32",    d3d8_to_dxgi_format(D3DFMT_A32L32),        DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("V32U32",    d3d8_to_dxgi_format(D3DFMT_V32U32),        DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("Q16W16V16U16", d3d8_to_dxgi_format(D3DFMT_Q16W16V16U16), DXGI_FORMAT_R16G16B16A16_SNORM);
    CHECK_FMT("Q32W32V32U32", d3d8_to_dxgi_format(D3DFMT_Q32W32V32U32), DXGI_FORMAT_R32G32B32A32_SINT);
    CHECK_FMT("A2R10G10B10", d3d8_to_dxgi_format(D3DFMT_A2R10G10B10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("X2R10G10B10", d3d8_to_dxgi_format(D3DFMT_X2R10G10B10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("A2B10G10R10", d3d8_to_dxgi_format(D3DFMT_A2B10G10R10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("A2W10V10U10", d3d8_to_dxgi_format(D3DFMT_A2W10V10U10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("R11G11B10", d3d8_to_dxgi_format(D3DFMT_R11G11B10),     DXGI_FORMAT_R11G11B10_FLOAT);
    CHECK_FMT("D24X8",     d3d8_to_dxgi_format(D3DFMT_D24X8),         DXGI_FORMAT_D24_UNORM_S8_UINT);
    CHECK_FMT("D24FS8",    d3d8_to_dxgi_format(D3DFMT_D24FS8),        DXGI_FORMAT_D24_UNORM_S8_UINT);
    CHECK_FMT("D32",       d3d8_to_dxgi_format(D3DFMT_D32),           DXGI_FORMAT_D32_FLOAT);
    CHECK_FMT("DXN",       d3d8_to_dxgi_format(D3DFMT_DXN),           DXGI_FORMAT_BC5_UNORM);
    CHECK_FMT("DXT3A",     d3d8_to_dxgi_format(D3DFMT_DXT3A),         DXGI_FORMAT_BC2_UNORM);
    CHECK_FMT("DXT5A",     d3d8_to_dxgi_format(D3DFMT_DXT5A),         DXGI_FORMAT_BC3_UNORM);
    CHECK_FMT("CTX1",      d3d8_to_dxgi_format(D3DFMT_CTX1),          DXGI_FORMAT_BC1_UNORM);
    /* LIN variants */
    CHECK_FMT("LIN_R16F",  d3d8_to_dxgi_format(D3DFMT_LIN_R16F),      DXGI_FORMAT_R16_FLOAT);
    CHECK_FMT("LIN_A16B16G16R16", d3d8_to_dxgi_format(D3DFMT_LIN_A16B16G16R16), DXGI_FORMAT_R16G16B16A16_UNORM);
    CHECK_FMT("LIN_Q16W16V16U16", d3d8_to_dxgi_format(D3DFMT_LIN_Q16W16V16U16), DXGI_FORMAT_R16G16B16A16_SNORM);
}

static void test_bpp(void)
{
    printf("test_bpp\n");
    CHECK_INT("R16F 16",  d3d8_format_bpp(D3DFMT_R16F), 16);
    CHECK_INT("R32F 32",  d3d8_format_bpp(D3DFMT_R32F), 32);
    CHECK_INT("G16R16F",  d3d8_format_bpp(D3DFMT_G16R16F), 32);
    CHECK_INT("G32R32F",  d3d8_format_bpp(D3DFMT_G32R32F), 64);
    CHECK_INT("A16B16G16R16F", d3d8_format_bpp(D3DFMT_A16B16G16R16F), 64);
    CHECK_INT("A32B32G32R32F", d3d8_format_bpp(D3DFMT_A32B32G32R32F), 128);
    CHECK_INT("G16R16",   d3d8_format_bpp(D3DFMT_G16R16), 32);
    CHECK_INT("A16L16",   d3d8_format_bpp(D3DFMT_A16L16), 32);
    CHECK_INT("A16B16G16R16", d3d8_format_bpp(D3DFMT_A16B16G16R16), 64);
    CHECK_INT("A32B32G32R32", d3d8_format_bpp(D3DFMT_A32B32G32R32), 128);
    CHECK_INT("G32R32",   d3d8_format_bpp(D3DFMT_G32R32), 64);
    CHECK_INT("L32",      d3d8_format_bpp(D3DFMT_L32), 32);
    CHECK_INT("A32L32",   d3d8_format_bpp(D3DFMT_A32L32), 64);
    CHECK_INT("V32U32",   d3d8_format_bpp(D3DFMT_V32U32), 64);
    CHECK_INT("Q16W16V16U16", d3d8_format_bpp(D3DFMT_Q16W16V16U16), 64);
    CHECK_INT("Q32W32V32U32", d3d8_format_bpp(D3DFMT_Q32W32V32U32), 128);
    CHECK_INT("A2R10G10B10", d3d8_format_bpp(D3DFMT_A2R10G10B10), 32);
    CHECK_INT("R11G11B10", d3d8_format_bpp(D3DFMT_R11G11B10), 32);
    CHECK_INT("D32",      d3d8_format_bpp(D3DFMT_D32), 32);
    CHECK_INT("DXN",      d3d8_format_bpp(D3DFMT_DXN), 8);
    CHECK_INT("DXT3A",    d3d8_format_bpp(D3DFMT_DXT3A), 8);
    CHECK_INT("DXT5A",    d3d8_format_bpp(D3DFMT_DXT5A), 8);
    CHECK_INT("CTX1",     d3d8_format_bpp(D3DFMT_CTX1), 4);
    CHECK_INT("LIN_A16B16G16R16", d3d8_format_bpp(D3DFMT_LIN_A16B16G16R16), 64);
    CHECK_INT("LIN_A2R10G10B10", d3d8_format_bpp(D3DFMT_LIN_A2R10G10B10), 32);
}

static void test_predicates(void)
{
    printf("test_predicates\n");
    CHECK("compressed CTX1",  d3d8_format_is_compressed(D3DFMT_CTX1));
    CHECK("compressed DXN",   d3d8_format_is_compressed(D3DFMT_DXN));
    CHECK("compressed DXT3A", d3d8_format_is_compressed(D3DFMT_DXT3A));
    CHECK("compressed DXT5A", d3d8_format_is_compressed(D3DFMT_DXT5A));
    CHECK("not compressed R32F", !d3d8_format_is_compressed(D3DFMT_R32F));
    CHECK("depth D24FS8",     d3d8_format_is_depth(D3DFMT_D24FS8));
    CHECK("depth D24X8",      d3d8_format_is_depth(D3DFMT_D24X8));
    CHECK("depth D32",        d3d8_format_is_depth(D3DFMT_D32));
    CHECK("not depth R32F",   !d3d8_format_is_depth(D3DFMT_R32F));
}

static void test_swizzle_classification(void)
{
    printf("test_swizzle_classification\n");
    /* Swizzled (default) */
    CHECK("swizzled R32F",     d3d8_format_is_swizzled(D3DFMT_R32F) == 1);
    CHECK("swizzled A16B16G16R16", d3d8_format_is_swizzled(D3DFMT_A16B16G16R16) == 1);
    CHECK("swizzled A2R10G10B10", d3d8_format_is_swizzled(D3DFMT_A2R10G10B10) == 1);
    CHECK("swizzled D24FS8-not",  d3d8_format_is_swizzled(D3DFMT_D24FS8) == 0);   /* depth */
    CHECK("swizzled D32-not",     d3d8_format_is_swizzled(D3DFMT_D32) == 0);
    CHECK("swizzled DXN-not",     d3d8_format_is_swizzled(D3DFMT_DXN) == 0);     /* compressed */
    CHECK("swizzled CTX1-not",    d3d8_format_is_swizzled(D3DFMT_CTX1) == 0);
    /* Linear */
    CHECK("linear LIN_R16F",      d3d8_format_is_swizzled(D3DFMT_LIN_R16F) == 0);
    CHECK("linear LIN_A16B16G16R16", d3d8_format_is_swizzled(D3DFMT_LIN_A16B16G16R16) == 0);
    CHECK("linear LIN_A2R10G10B10", d3d8_format_is_swizzled(D3DFMT_LIN_A2R10G10B10) == 0);
    CHECK("linear LIN_Q16W16V16U16", d3d8_format_is_swizzled(D3DFMT_LIN_Q16W16V16U16) == 0);
    CHECK("linear LIN_R32F",      d3d8_format_is_swizzled(D3DFMT_LIN_R32F) == 0);
    CHECK("linear LIN_D24FS8",    d3d8_format_is_swizzled(D3DFMT_LIN_D24FS8) == 0);
    CHECK("linear LIN_DXN",       d3d8_format_is_swizzled(D3DFMT_LIN_DXN) == 0);
}

static void test_unswizzle_roundtrip(void)
{
    printf("test_unswizzle_roundtrip\n");
    /* Real round-trip: linear -> swizzle -> unswizzle must recover the
     * original linear data exactly, for a few sizes and bpp values. */
    {
        struct { UINT w, h, bpp; } cases[] = {
            { 4,   4,   1 }, { 4, 4, 2 }, { 4, 4, 4 },
            { 8,   8,   4 }, { 16, 4, 4 }, { 4, 16, 2 }, { 32, 8, 4 },
        };
        int c;
        for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
            UINT w = cases[c].w, h = cases[c].h, bpp = cases[c].bpp;
            UINT n = w * h * bpp;
            BYTE *lin = (BYTE *)malloc(n), *swz = (BYTE *)malloc(n), *re = (BYTE *)malloc(n);
            UINT i;
            for (i = 0; i < n; i++) lin[i] = (BYTE)(i * 31 + c);
            xbox_swizzle_rect(swz, lin, w, h, bpp);
            memset(re, 0, n);
            xbox_unswizzle_rect(re, swz, w, h, bpp);
            {
                char nm[64];
                snprintf(nm, sizeof(nm), "unswizzle roundtrip %ux%u bpp%u", w, h, bpp);
                CHECK(nm, memcmp(lin, re, n) == 0);
            }
            free(lin); free(swz); free(re);
        }
    }
}

/* Forward the D3D8 conversion helper through a tiny local wrapper so we
 * test the real d3d8_resources.c implementation. */
static void test_convert_linear(void)
{
    BYTE src[64], dst[64];
    printf("test_convert_linear\n");

    /* A16B16G16R16 -> R16G16B16A16 (channel swap, 64 bpp, 1 px).
     * src[0..7] = A(LE) B G R. Use A=1, B=2, G=3, R=4. */
    {
        UINT16 s[4] = { 1, 2, 3, 4 };  /* A=1, B=2, G=3, R=4 */
        UINT16 d[4] = { 0, 0, 0, 0 };
        d3d8_convert_linear_pixels(D3DFMT_A16B16G16R16, 1, 1, (const BYTE *)s, (BYTE *)d, 0);
        CHECK_INT("A16B16G16R16 R", d[0], 4);
        CHECK_INT("A16B16G16R16 G", d[1], 3);
        CHECK_INT("A16B16G16R16 B", d[2], 2);
        CHECK_INT("A16B16G16R16 A", d[3], 1);
        CHECK("has_conversion A16B16G16R16", d3d8_format_has_conversion(D3DFMT_A16B16G16R16));
    }

    /* A32B32G32R32 -> R32G32B32A32 (channel swap, 128 bpp, 1 px). */
    {
        UINT32 s[4] = { 1, 2, 3, 4 };  /* A=1, B=2, G=3, R=4 */
        UINT32 d[4] = { 0, 0, 0, 0 };
        d3d8_convert_linear_pixels(D3DFMT_A32B32G32R32, 1, 1, (const BYTE *)s, (BYTE *)d, 0);
        CHECK_INT("A32B32G32R32 R", d[0], 4);
        CHECK_INT("A32B32G32R32 G", d[1], 3);
        CHECK_INT("A32B32G32R32 B", d[2], 2);
        CHECK_INT("A32B32G32R32 A", d[3], 1);
        CHECK("has_conversion A32B32G32R32", d3d8_format_has_conversion(D3DFMT_A32B32G32R32));
    }

    /* A2R10G10B10 -> R10G10B10A2 (swap A and R fields).
     * Xbox: A=0x3, R=0x3FF (bits 20-29), G=0x155, B=0x0AA.
     * word = (0x3<<30)|(0x3FF<<20)|(0x155<<10)|0x0AA */
    {
        UINT32 s, d;
        s = (0x3u << 30) | (0x3FFu << 20) | (0x155u << 10) | (0x0AAu);
        d3d8_convert_linear_pixels(D3DFMT_A2R10G10B10, 1, 1, (const BYTE *)&s, (BYTE *)&d, 0);
        CHECK_INT("A2R10G10B10->R10G10B10A2", d, (0x3u << 30) | (0x3FFu) | (0x155u << 10) | (0x0AAu << 20));
        CHECK("has_conversion A2R10G10B10", d3d8_format_has_conversion(D3DFMT_A2R10G10B10));
    }

    /* L6V5U5 sign extension: V=0x10 (=-16), U=0x10, L=0x20.
     * word = (U<<11)|(L<<5)|V = (0x10<<11)|(0x20<<5)|0x10 */
    {
        UINT16 w = (UINT16)((0x10u << 11) | (0x20u << 5) | 0x10u);
        BYTE d[2] = { 0, 0 };
        d3d8_convert_linear_pixels(D3DFMT_L6V5U5, 1, 1, (const BYTE *)&w, d, 0);
        CHECK_INT("L6V5U5 V->R sign", (INT8)d[0], -16 * 8);   /* -16 << 3 */
        CHECK_INT("L6V5U5 U->G sign", (INT8)d[1], -16 * 8);
        CHECK("has_conversion L6V5U5", d3d8_format_has_conversion(D3DFMT_L6V5U5));
    }

    /* A2B10G10R10 is a direct map (no conversion). */
    CHECK("no conv A2B10G10R10", !d3d8_format_has_conversion(D3DFMT_A2B10G10R10));

    (void)src; (void)dst;
}

static void test_fvf_position(void)
{
    static const struct { DWORD position; UINT bytes; int transformed; } cases[] = {
        {D3DFVF_XYZ, 12, 0}, {D3DFVF_XYZRHW, 16, 1},
        {D3DFVF_XYZB1, 16, 0}, {D3DFVF_XYZB2, 20, 0},
        {D3DFVF_XYZB3, 24, 0}, {D3DFVF_XYZB4, 28, 0},
        {D3DFVF_XYZB5, 32, 0}, {0, 0, 0},
    };
    UINT i;
    printf("test_fvf_position\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        DWORD fvf = cases[i].position | D3DFVF_DIFFUSE | D3DFVF_TEX1;
        CHECK_INT("position span", d3d8_fvf_position_bytes(fvf), cases[i].bytes);
        CHECK_INT("transformed", d3d8_fvf_transformed(fvf), cases[i].transformed);
    }
    /* A packed last beta still occupies one four-byte slot. */
    CHECK_INT("indexed beta span", d3d8_fvf_position_bytes(0x1000u | D3DFVF_XYZB3), 24);
    CHECK_INT("weighted normal offset", d3d8_fvf_position_bytes(0x118u), 20);
}

int main(void)
{
    int i;
    printf("d3d8_smoke: running\n");
    for (i = 0; i < 256; i++) g_palette[i] = 0xFF000000u | i;

    test_to_dxgi();
    test_bpp();
    test_predicates();
    test_swizzle_classification();
    test_unswizzle_roundtrip();
    test_convert_linear();
    test_fvf_position();

    if (failures == 0) {
        printf("d3d8_smoke: ALL PASS\n");
        return 0;
    }
    printf("d3d8_smoke: %d FAILURE(S)\n", failures);
    return 1;
}
