/*
 * enhance_text.h - menu text with input glyphs in it (overlay-revamp D8).
 *
 * Any text the menu draws (help, labels, notes, toasts, the footer) may
 * hold tokens that draw as glyphs on the text's line:
 *
 *   {pad:south} {pad:lb}    a pad source, by its source name, a Duke
 *                           control's name (a = south) or a short alias
 *                           (lb rb lt rt l3 r3 ls rs view menu home dpad
 *                           l4 r4 l5 r5); positional, so {pad:south} is
 *                           "B" on a Nintendo pad
 *   {key:F5} {key:ctrl+F6}  a key cap per key, in the layout in use
 *   {mouse:left}            left right middle wheel_up wheel_down move
 *                           mouse
 *   {act:menu.open}         the action's binding in effect for the device
 *                           (a pad binding for the pad, a key for the
 *                           keyboard and the mouse), so a hint follows
 *                           rebinding; a chord's members are joined by +
 *
 * A token that names nothing draws as its own text, braces included, and
 * a member with no glyph (the Xbox family's paddles) draws its name. Mods
 * get glyphs by writing tokens in their strings: no API call (Q5 open).
 */
#ifndef ENHANCE_TEXT_H
#define ENHANCE_TEXT_H

#include "microui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which binding {act:} shows: the device the player used last. */
enum { ENHANCE_DEV_PAD, ENHANCE_DEV_KEYBOARD, ENHANCE_DEV_MOUSE };

/* Draw text with its tokens expanded from (x, y), the top left of a line
 * of font, in color. family is an XBOX_PAD_FAMILY_* (host_pad.h), or
 * XBOX_PAD_FAMILY_AUTO for the first pad's (xbox_InputPadFamily, which
 * input.glyphs overrides). Returns the width drawn. */
int enhance_text_draw(mu_Context *ctx, mu_Font font, const char *text, int x, int y,
                      mu_Color color, int family, int device);
/* The width enhance_text_draw would draw. */
int enhance_text_width(mu_Font font, const char *text, int family, int device);

/* Install the host's key namer for key caps (nv2a_ovl_glyph_set_key_namer).
 * enhance_text_draw does it on first use; a page that draws key caps
 * itself calls this first. */
void enhance_text_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_TEXT_H */
