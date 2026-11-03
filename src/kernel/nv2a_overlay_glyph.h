/*
 * nv2a_overlay_glyph.h - input prompts and icons for the overlay's pages
 * (overlay-revamp D7, D8).
 *
 * A glyph is one value the menu can hold and draw: a section icon (a
 * Lucide code point), a pad or mouse button (a glyph of the input font,
 * third_party/kenney-input), or a keyboard key, drawn as a key cap with
 * the key's name in the layout in use. Values are 0 when there is no
 * glyph, and the caller then writes the input's text name.
 *
 * Pad glyphs are chosen by input source, which is positional: on the
 * Nintendo family SOUTH is the "B" glyph, the button the player presses.
 * The family is the one xbox_InputPadFamily reports (input/host_pad.h,
 * XBOX_PAD_FAMILY_*), which input.glyphs overrides. The guide button is
 * a plain home glyph on every family: no brand mark is drawn.
 */
#ifndef NV2A_OVERLAY_GLYPH_H
#define NV2A_OVERLAY_GLYPH_H

#include "microui.h"

#include <stdint.h>

typedef uint32_t nv2a_ovl_glyph;

/* The kind in the top byte, the code point or key below it. */
enum {
    NV2A_OVL_GLYPH_NONE,
    NV2A_OVL_GLYPH_ICON,    /* a code point of the icon face */
    NV2A_OVL_GLYPH_INPUT,   /* a code point of the input face */
    NV2A_OVL_GLYPH_KEY,     /* a virtual-key code (XBOX_VK_*), as a key cap */
};
#define NV2A_OVL_GLYPH(kind, code) ((nv2a_ovl_glyph)(((uint32_t)(kind) << 24) | ((code) & 0xffffffu)))
#define NV2A_OVL_GLYPH_KIND(g)     ((int)((g) >> 24))
#define NV2A_OVL_GLYPH_CODE(g)     ((uint32_t)(g) & 0xffffffu)

/* The menu's icons (D7): Lucide 1.57.0's own code points, the subset in
 * third_party/lucide/VERSION. */
enum {
    NV2A_OVL_ICON_SLIDERS = 0xE29A,
    NV2A_OVL_ICON_SPARKLES = 0xE412,
    NV2A_OVL_ICON_PACKAGE = 0xE129,
    NV2A_OVL_ICON_GAMEPAD = 0xE0DF,
    NV2A_OVL_ICON_PUZZLE = 0xE29C,
    NV2A_OVL_ICON_ACTIVITY = 0xE038,
    NV2A_OVL_ICON_KEYBOARD = 0xE284,
    NV2A_OVL_ICON_BUG = 0xE20C,
    NV2A_OVL_ICON_FOLDER = 0xE0D7,
    NV2A_OVL_ICON_HELP = 0xE082,
    NV2A_OVL_ICON_MAXIMIZE = 0xE113,
    NV2A_OVL_ICON_MINIMIZE = 0xE11B,
    NV2A_OVL_ICON_CLOSE = 0xE1B2,
    NV2A_OVL_ICON_CHECK = 0xE06C,
    NV2A_OVL_ICON_CHEVRON_LEFT = 0xE06E,
    NV2A_OVL_ICON_CHEVRON_RIGHT = 0xE06F,
    NV2A_OVL_ICON_RESET = 0xE144,
    NV2A_OVL_ICON_UPLOAD = 0xE19E,
    NV2A_OVL_ICON_DOWNLOAD = 0xE0B2,
};

/* Whole controls, past the InputSource values (input/input_map.h), for
 * prompts such as "{pad:dpad} Move". */
enum {
    NV2A_OVL_SRC_DPAD = 32,
    NV2A_OVL_SRC_LSTICK,
    NV2A_OVL_SRC_RSTICK,
};

/* Mouse glyphs. */
enum {
    NV2A_OVL_MOUSE_LEFT,
    NV2A_OVL_MOUSE_RIGHT,
    NV2A_OVL_MOUSE_MIDDLE,
    NV2A_OVL_MOUSE_WHEEL_UP,
    NV2A_OVL_MOUSE_WHEEL_DOWN,
    NV2A_OVL_MOUSE_MOVE,
    NV2A_OVL_MOUSE_ANY,     /* the mouse itself */
    NV2A_OVL_MOUSE_COUNT
};

nv2a_ovl_glyph nv2a_ovl_glyph_icon(uint32_t cp);
/* The glyph of src (an InputSource or NV2A_OVL_SRC_*) on family; 0 when
 * the family has none (the Xbox paddles, the Switch capture button). The
 * generic family has its own lettered back, start and stick clicks, and
 * the Xbox glyphs for the rest. */
nv2a_ovl_glyph nv2a_ovl_glyph_src(int family, int src);
nv2a_ovl_glyph nv2a_ovl_glyph_mouse(int button);
/* A key cap for vk; the mouse buttons' codes (1, 2, 4) give their mouse
 * glyphs instead. */
nv2a_ovl_glyph nv2a_ovl_glyph_key(int vk);

/* The key cap's label: the host names the key in the layout in use. The
 * kernel does not link the input layer, so the enhance layer installs
 * this; without it a cap shows the code in hex. fn writes at most n bytes
 * of UTF-8 into buf and returns buf, or NULL for no name. */
typedef const char *(*nv2a_ovl_key_namer)(int vk, char *buf, int n);
void nv2a_ovl_glyph_set_key_namer(nv2a_ovl_key_namer fn);

/* The side of g's square in a line of font: the line's height, and for
 * a pad glyph in small text the body size's (a legible minimum),
 * centred on the line. */
int nv2a_ovl_glyph_box(nv2a_ovl_glyph g, mu_Font font);
/* The width of g drawn in a line of font: a symbol is its square, a key
 * cap fits its label. */
int nv2a_ovl_glyph_width(nv2a_ovl_glyph g, mu_Font font);
/* Draw g in r (a symbol centred in r's square of side r.h, a key cap
 * filling r) in color. A key cap is the colour with its label cut out in
 * the panel colour. */
void nv2a_ovl_ui_glyph(mu_Context *ctx, nv2a_ovl_glyph g, mu_Rect r, mu_Color color);

#endif /* NV2A_OVERLAY_GLYPH_H */
