/*
 * nv2a_overlay_int.h - the overlay's insides, shared by its three files
 * and the overlay ctest:
 *
 *   nv2a_overlay.c       layers, triple buffers, arming, toasts, images
 *   nv2a_overlay_font.c  the atlas bake (stb_truetype) and text metrics
 *   nv2a_overlay_ui.c    the theme, microui contexts, the converter from
 *                        microui commands to a draw list, the STATS and
 *                        TOAST pages
 *   nv2a_overlay_ref.c   the CPU reference rasteriser (ctests; F-2)
 */
#ifndef NV2A_OVERLAY_INT_H
#define NV2A_OVERLAY_INT_H

#include "nv2a_overlay.h"
#include "microui.h"

#include <stddef.h>
#include <stdint.h>

#define OVL_LOGP "[OVERLAY] "

/* ── Theme (R8): code defaults, sizes at 1080 output lines ── */

struct nv2a_ovl_theme {
    mu_Color bg, bg_row, bg_hover, accent, text, text_dim, warn, bad, good;
    int radius, pad, gap, row_h;
    int font_title, font_body, font_small;
    float shadow_alpha;
    int shadow;
};
extern const struct nv2a_ovl_theme nv2a_ovl_theme_default;

/* clamp(clamp(out_h / 1080, 0.5, 3.0) * overlay.ui_scale, 0.5, 3.0)
 * (nv2a_overlay_set_ui_scale). */
float nv2a_ovl_ui_scale(uint32_t out_h);
/* A token scaled and rounded to whole pixels (at least 1 for a non-zero
 * token). */
int nv2a_ovl_px(float s, int token);

/* ── Font ── */

/* The baked sizes, one per text token. */
enum { OVL_FONT_SMALL, OVL_FONT_BODY, OVL_FONT_TITLE, OVL_FONT_SLOTS };

/* A microui font handle: the token slot, the drawn size in pixels and
 * whether digits are tabular (each advanced by the widest digit). */
#define OVL_FONT(slot, px, tab) \
    ((mu_Font)(uintptr_t)(((unsigned)(px) & 0xfffu) | ((unsigned)(slot) << 12) \
                          | ((tab) ? 0x8000u : 0u)))
#define OVL_FONT_PX(f)   ((int)((uintptr_t)(f) & 0xfffu))
#define OVL_FONT_SLOT(f) ((int)(((uintptr_t)(f) >> 12) & 7u))
#define OVL_FONT_TAB(f)  (((uintptr_t)(f) & 0x8000u) != 0)

/* Bakes Regular at the three text sizes for out_h lines into a coverage
 * atlas, with the symbol faces when symbols (the menu layer draws them),
 * expands it to premultiplied white, and logs the atlas line. Init
 * thread only, before any layer publishes. Returns 0 or -1. */
int nv2a_ovl_font_bake(uint32_t out_h, int symbols);
/* Whether the atlas holds the symbol faces. */
int nv2a_ovl_font_symbols(void);
void nv2a_ovl_font_free(void);
const struct nv2a_ovl_atlas *nv2a_ovl_font_atlas(void);   /* NULL before a bake */

/* microui's callbacks. */
int nv2a_ovl_text_width(mu_Font font, const char *str, int len);
int nv2a_ovl_text_height(mu_Font font);

/* The UV of the atlas's white texel, for solid fills. */
void nv2a_ovl_font_white_uv(float *u, float *v);

/* One glyph's quad for code point cp drawn with font at pen (x, baseline
 * y); advances *x. Returns 0 for a glyph with no ink (a space). */
struct nv2a_ovl_quad { float x0, y0, x1, y1, u0, v0, u1, v1; };
int nv2a_ovl_font_glyph(mu_Font font, uint32_t cp, float *x, float y,
                        struct nv2a_ovl_quad *q);
/* The baseline offset below the top of a line of font. */
float nv2a_ovl_font_ascent(mu_Font font);

/* UTF-8 to a code point; advances *s, stops at end. 0xFFFD for a bad
 * sequence. */
uint32_t nv2a_ovl_utf8_next(const char **s, const char *end);

/* The symbol faces baked beside the text (overlay-revamp D7, D8): the
 * Lucide icon subset at the small, body and title sizes, and the input
 * glyphs (third_party/kenney-input) at small, body and twice body. */
enum { OVL_FACE_ICONS, OVL_FACE_INPUT, OVL_FACES };
#define OVL_SYM_SIZES 3
/* Whether face has a glyph for cp; needs no bake, and is 0 once a bake
 * left the symbol faces out. */
int nv2a_ovl_font_has_symbol(int face, uint32_t cp);
/* The quad of cp in face centred in the box of box pixels at (x, y),
 * from the nearest baked size. Returns 0 when the face lacks cp or no
 * atlas is baked. */
int nv2a_ovl_font_symbol(int face, uint32_t cp, float x, float y, float box,
                         struct nv2a_ovl_quad *q);

/* ── Custom microui commands: rounded rects, images, cursors, glyphs ── */

enum {
    OVL_CMD_RRECT = MU_COMMAND_MAX,
    OVL_CMD_IMAGE,
    OVL_CMD_CURSOR,
    OVL_CMD_GLYPH,      /* a symbol from a face of the atlas (nv2a_overlay_glyph.h) */
};
enum { OVL_CURSOR_ARROW, OVL_CURSOR_DOT };

/* A filled rounded rect. feather is the width of the alpha ramp centred
 * on the edge: 1 for a crisp antialiased edge, the shadow size for a
 * soft shadow. */
void nv2a_ovl_ui_rrect(mu_Context *ctx, mu_Rect r, int radius, float feather,
                       mu_Color c);
/* The panel's soft shadow under r (R8 shadow token). */
void nv2a_ovl_ui_shadow(mu_Context *ctx, mu_Rect r, int radius, int size,
                        float alpha);
void nv2a_ovl_ui_image(mu_Context *ctx, mu_Rect r, uint16_t tex, mu_Color tint);
/* A cursor with its hotspot at (x, y), size pixels tall. */
void nv2a_ovl_ui_cursor(mu_Context *ctx, int style, int x, int y, int size,
                        mu_Color fill, mu_Color edge);
/* The symbol cp of face (OVL_FACE_*) centred in r, a square of the
 * smaller side; nv2a_ovl_ui_glyph is the public way in. */
void nv2a_ovl_ui_symbol(mu_Context *ctx, int face, uint32_t cp, mu_Rect r, mu_Color c);

/* A finished microui frame into a draw list. Returns 1 when the list was
 * cut at a command boundary for want of room, else 0. */
int nv2a_ovl_convert(mu_Context *ctx, struct nv2a_ovl_build *b);

/* ── UI contexts and the presenter's pages (nv2a_overlay_ui.c) ── */

mu_Context *nv2a_ovl_ui_new(void);         /* mu_init and the callbacks */
/* Compose the STATS strip / the TOAST stack into their layers. */
struct nv2a_ovl_toast_view { const char *text; };
void nv2a_ovl_compose_stats(mu_Context *ctx, uint32_t out_w, uint32_t out_h,
                            const char *line, int spike);
void nv2a_ovl_compose_toasts(mu_Context *ctx, uint32_t out_w, uint32_t out_h,
                             const char *const *lines, int n);
/* The strip's text, from the stats counters. *spike is set when the
 * worst frame of the last second is a spike. */
void nv2a_ovl_stats_line(char *out, size_t n, const char *presenter, int *spike);

/* ── Core (nv2a_overlay.c), for the ui file ── */

mu_Context *nv2a_ovl_ctx(enum nv2a_overlay_layer l);
uint64_t nv2a_ovl_now_ns(void);             /* monotonic */

/* ── The CPU reference rasteriser (nv2a_overlay_ref.c; ctests only) ──
 *
 * Draws lists into a premultiplied RGBA8 picture of w x h as a presenter
 * would: scaled from the list's placement to w x h, scissored, bilinear
 * atlas samples, premultiplied "over". The GPU presenters are checked
 * against it by eye in window shots; the ctests check its checksum. */
void nv2a_ovl_ref_draw(uint32_t *dst, int w, int h, const struct nv2a_ovl_list *l,
                       const struct nv2a_ovl_atlas *a);

#endif /* NV2A_OVERLAY_INT_H */
