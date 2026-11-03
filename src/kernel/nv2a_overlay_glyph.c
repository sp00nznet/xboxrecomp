/*
 * nv2a_overlay_glyph.c - which glyph names an input, and drawing one
 * (overlay-revamp D7, D8; the API is nv2a_overlay_glyph.h).
 *
 * The input font's code points are computed, not tabled: the generator
 * (tools/overlay/glyphs_to_ttf.py) puts family f's glyph for source s at
 * U+E000 + 0x40 b + s, where b is f for the first five families, the mouse
 * is block 5, the shared home glyph block 6 and the GameCube block 7. Whether a glyph exists is asked of the font itself, so the
 * table lives in one place, the script, and glyphs.txt lists it.
 */
#include "nv2a_overlay_glyph.h"
#include "nv2a_overlay_int.h"

#include <stdio.h>
#include <string.h>

#define IN_PUA 0xE000u
#define IN_BLOCK 0x40u
/* The blocks: XBOX_PAD_FAMILY_* (input/host_pad.h; the kernel does not
 * include the input layer, and enhance_text.c checks the two agree). */
#define IN_XBOX 0
#define IN_NINTENDO 2
#define IN_GENERIC 4
#define IN_GAMECUBE 5
#define IN_FAMILIES 6
#define IN_MOUSE 5
#define IN_SHARED 6
#define IN_HOME (IN_PUA + IN_BLOCK * IN_SHARED)
#define SRC_GUIDE 6       /* INPUT_SRC_GUIDE */

static nv2a_ovl_key_namer s_namer;

/* A family's block: the GameCube came after the mouse and shared blocks. */
static uint32_t in_block(int family)
{
    return family == IN_GAMECUBE ? 7u : (uint32_t)family;
}

nv2a_ovl_glyph nv2a_ovl_glyph_icon(uint32_t cp)
{
    return nv2a_ovl_font_has_symbol(OVL_FACE_ICONS, cp) ? NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_ICON, cp)
                                                        : 0;
}

static nv2a_ovl_glyph input(uint32_t cp)
{
    return nv2a_ovl_font_has_symbol(OVL_FACE_INPUT, cp) ? NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_INPUT, cp)
                                                        : 0;
}

nv2a_ovl_glyph nv2a_ovl_glyph_src(int family, int src)
{
    nv2a_ovl_glyph g;
    if (src < 1 || src > NV2A_OVL_SRC_RSTICK)
        return 0;
    /* One home glyph for every family: the guide buttons carry brand
     * marks, which the overlay does not draw. */
    if (src == SRC_GUIDE)
        return input(IN_HOME);
    if (family < 0 || family >= IN_FAMILIES)
        family = IN_XBOX;
    g = input(IN_PUA + IN_BLOCK * in_block(family) + (uint32_t)src);
    /* The generic pack letters only the buttons whose names differ by
     * maker; its face buttons and the rest are the Xbox shapes, which an
     * unknown pad most often copies. The GameCube pack has no back, left
     * shoulder or stick clicks: what SDL maps there on a GameCube-style
     * pad is a Nintendo layout, so the Nintendo shapes. */
    if (!g && family == IN_GENERIC)
        g = input(IN_PUA + IN_BLOCK * IN_XBOX + (uint32_t)src);
    else if (!g && family == IN_GAMECUBE)
        g = input(IN_PUA + IN_BLOCK * IN_NINTENDO + (uint32_t)src);
    return g;
}

nv2a_ovl_glyph nv2a_ovl_glyph_mouse(int button)
{
    if (button < 0 || button >= NV2A_OVL_MOUSE_COUNT)
        return 0;
    return input(IN_PUA + IN_BLOCK * IN_MOUSE + (uint32_t)button);
}

nv2a_ovl_glyph nv2a_ovl_glyph_key(int vk)
{
    /* XBOX_VK_LBUTTON, RBUTTON and MBUTTON share the key table's codes. */
    switch (vk) {
    case 0x01: return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_LEFT);
    case 0x02: return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_RIGHT);
    case 0x04: return nv2a_ovl_glyph_mouse(NV2A_OVL_MOUSE_MIDDLE);
    }
    return vk > 0 && vk <= 0xFF ? NV2A_OVL_GLYPH(NV2A_OVL_GLYPH_KEY, (uint32_t)vk) : 0;
}

void nv2a_ovl_glyph_set_key_namer(nv2a_ovl_key_namer fn)
{
    s_namer = fn;
}

static const char *key_label(int vk, char *buf, int n)
{
    const char *s = s_namer ? s_namer(vk, buf, n) : NULL;
    if (!s || !*s) {
        snprintf(buf, (size_t)n, "%02X", vk);
        s = buf;
    }
    return s;
}

/* A cap's label is set a size below the line it sits in, so the cap's
 * edge shows around it. */
static mu_Font cap_font(int h)
{
    int px = (h * 5 + 4) / 8;
    return OVL_FONT(OVL_FONT_SMALL, px < 1 ? 1 : px, 0);
}

static int cap_width(const char *label, int h)
{
    int w = nv2a_ovl_text_width(cap_font(h), label, -1) + h / 2;
    return w > h ? w : h;
}

int nv2a_ovl_glyph_box(nv2a_ovl_glyph g, mu_Font font)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    int h = nv2a_ovl_text_height(font);
    /* A pad glyph is a letter or a shape inside a button's outline, so at
     * the small text's size Guide, Back and Start, or White and Black,
     * blur into one: it never goes under the body size. */
    if (NV2A_OVL_GLYPH_KIND(g) == NV2A_OVL_GLYPH_INPUT && OVL_FONT_SLOT(font) == OVL_FONT_SMALL
        && t->font_small > 0 && t->font_body > t->font_small)
        return (h * t->font_body + t->font_small / 2) / t->font_small;
    return h;
}

int nv2a_ovl_glyph_width(nv2a_ovl_glyph g, mu_Font font)
{
    int h = nv2a_ovl_text_height(font);
    char buf[64];
    switch (NV2A_OVL_GLYPH_KIND(g)) {
    case NV2A_OVL_GLYPH_ICON:
        return h;
    case NV2A_OVL_GLYPH_INPUT:
        return nv2a_ovl_glyph_box(g, font);
    case NV2A_OVL_GLYPH_KEY:
        return cap_width(key_label((int)NV2A_OVL_GLYPH_CODE(g), buf, sizeof buf), h);
    }
    return 0;
}

void nv2a_ovl_ui_glyph(mu_Context *ctx, nv2a_ovl_glyph g, mu_Rect r, mu_Color color)
{
    switch (NV2A_OVL_GLYPH_KIND(g)) {
    case NV2A_OVL_GLYPH_ICON:
        nv2a_ovl_ui_symbol(ctx, OVL_FACE_ICONS, NV2A_OVL_GLYPH_CODE(g), r, color);
        break;
    case NV2A_OVL_GLYPH_INPUT:
        nv2a_ovl_ui_symbol(ctx, OVL_FACE_INPUT, NV2A_OVL_GLYPH_CODE(g), r, color);
        break;
    case NV2A_OVL_GLYPH_KEY: {
        /* The cap filled in the glyph colour with the label in the panel
         * colour, as the pad glyphs are a light shape with a dark letter. */
        char buf[64];
        const char *label = key_label((int)NV2A_OVL_GLYPH_CODE(g), buf, sizeof buf);
        mu_Font f = cap_font(r.h);
        mu_Color ink = nv2a_ovl_theme_default.bg;
        int tw = nv2a_ovl_text_width(f, label, -1), th = nv2a_ovl_text_height(f);
        ink.a = color.a;
        nv2a_ovl_ui_rrect(ctx, r, r.h / 4, 1.0f, color);
        mu_draw_text(ctx, f, label, -1, mu_vec2(r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2), ink);
        break;
    }
    }
}
