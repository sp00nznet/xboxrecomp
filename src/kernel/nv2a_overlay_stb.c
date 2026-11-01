/*
 * nv2a_overlay_stb.c - stb_truetype's implementation, in a file of its own
 * so the vendored header builds without the kernel's warnings.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* stbrp_rect is complete only here, beside the implementation; the font
 * bake (nv2a_overlay_font.c) sizes, steps and checks its boxes through
 * these. */
stbrp_rect *nv2a_ovl_stb_rects(int n)
{
    return (stbrp_rect *)malloc(sizeof(stbrp_rect) * (size_t)(n > 0 ? n : 1));
}

stbrp_rect *nv2a_ovl_stb_rect_at(stbrp_rect *base, int i)
{
    return base + i;
}

int nv2a_ovl_stb_all_packed(const stbrp_rect *r, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!r[i].was_packed)
            return 0;
    return 1;
}
