/* SET_COLOR_CLEAR_VALUE is in the colour surface's own format
 * (nv2a_backend_common.h): what the CPU path writes to a 16-bit surface, and
 * the A8R8G8B8 colour the GPU backends clear their 8-bit targets to. */
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* SET_SURFACE_FORMAT colour codes (bits 3:0). */
#define X1R5G5B5 0x1u
#define X1R5G5B5_O1R5G5B5 0x2u
#define R5G6B5   0x3u
#define A8R8G8B8 0x8u

static uint32_t argb(uint32_t format, uint32_t value)
{
    struct nv2a_pb_gpu g;
    memset(&g, 0, sizeof g);
    g.format = 0x00000120u | format;       /* pitch, Z24S8, as titles set it */
    g.clear_color = value;
    return nv2a_clear_argb(&g);
}

static int packed(uint32_t format, uint32_t value, uint16_t *out)
{
    struct nv2a_pb_gpu g;
    memset(&g, 0, sizeof g);
    g.format = 0x00000120u | format;
    g.clear_color = value;
    return nv2a_clear_packed16(&g, out);
}

int main(void)
{
    uint16_t v = 0;

    /* JSRF's Smilebit logo: white on R5G6B5 is 0xFFFF, not cyan. */
    CHECK(argb(R5G6B5, 0x0000FFFFu) == 0xFFFFFFFFu);
    /* Its Dolby logo: 0x20E4 is a near-black grey, not blue. */
    CHECK(argb(R5G6B5, 0x000020E4u) == 0xFF211C21u);
    CHECK(argb(R5G6B5, 0x00000000u) == 0xFF000000u);
    CHECK(argb(R5G6B5, 0x0000F800u) == 0xFFFF0000u);
    CHECK(argb(R5G6B5, 0x000007E0u) == 0xFF00FF00u);
    CHECK(argb(R5G6B5, 0x0000001Fu) == 0xFF0000FFu);
    CHECK(argb(X1R5G5B5, 0x00007C00u) == 0xFFFF0000u);
    CHECK(argb(X1R5G5B5, 0x000003E0u) == 0xFF00FF00u);
    CHECK(argb(X1R5G5B5, 0x00007FFFu) == 0xFFFFFFFFu);
    /* The O1 variant packs the same way. */
    CHECK(argb(X1R5G5B5_O1R5G5B5, 0x00007C00u) == 0xFFFF0000u);
    CHECK(argb(X1R5G5B5_O1R5G5B5, 0x0000001Fu) == 0xFF0000FFu);
    /* A 16-bit value ignores whatever sits in the high half. */
    CHECK(argb(R5G6B5, 0xFFFF20E4u) == argb(R5G6B5, 0x000020E4u));
    CHECK(argb(R5G6B5, 0xFFFF20E4u) == 0xFF211C21u);
    CHECK(argb(X1R5G5B5, 0xABCD7C00u) == 0xFFFF0000u);
    /* 32-bit surfaces take the value as it is, alpha included. */
    CHECK(argb(A8R8G8B8, 0x80204060u) == 0x80204060u);
    CHECK(argb(A8R8G8B8, 0x0000FFFFu) == 0x0000FFFFu);

    /* The CPU path writes a 16-bit RGB value as given. */
    CHECK(packed(R5G6B5, 0x000020E4u, &v) && v == 0x20E4u);
    CHECK(packed(X1R5G5B5, 0x00007FFFu, &v) && v == 0x7FFFu);
    CHECK(!packed(A8R8G8B8, 0xFFFFFFFFu, &v));
    CHECK(packed(X1R5G5B5_O1R5G5B5, 0x00007C00u, &v) && v == 0x7C00u);
    CHECK(packed(R5G6B5, 0xFFFF20E4u, &v) && v == 0x20E4u);

    /* The format at CLEAR_SURFACE time decides, not the one current when
     * SET_COLOR_CLEAR_VALUE arrived: the value is kept as sent. */
    {
        struct nv2a_pb_gpu g;
        memset(&g, 0, sizeof g);
        g.format = 0x00000120u | A8R8G8B8;
        g.clear_color = 0x000020E4u;
        g.format = 0x00000120u | R5G6B5;
        CHECK(nv2a_clear_argb(&g) == 0xFF211C21u);
        CHECK(nv2a_clear_packed16(&g, &v) && v == 0x20E4u);
        g.format = 0x00000120u | A8R8G8B8;
        CHECK(nv2a_clear_argb(&g) == 0x000020E4u);
        CHECK(!nv2a_clear_packed16(&g, &v));
    }

    printf("nv2a_clear_color: all checks passed\n");
    return 0;
}
