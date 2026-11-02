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
 *   {bind:guide+y}          one binding as the config writes it
 *   {bind:ctrl+F6}          ([input.actions], [input.controls]): a chord
 *                           of pad names as pad glyphs, anything else as
 *                           key caps (mouse1 the click); the hold and a
 *                           padN: prefix are dropped. The menu's binding
 *                           rows draw their values with it
 *   {ctl:move}              a control of the mod that owns the string, as
 *                           bound now (a mod's labels, help and status
 *                           panel); names nothing in the menu's own text.
 *                           Off the pad, a captured control bound only on
 *                           the pad shows the keys that drive it, a stick
 *                           its direction keys joined by / (the right
 *                           stick also the mouse)
 *
 * A token that names nothing draws as its own text, braces included, and
 * a member with no glyph (the Xbox family's paddles) draws its name. Mods
 * get glyphs by writing tokens in their strings, and outside the overlay
 * through mod API version 7 (Q5, D12a): glyph_name writes a ref's binding
 * as tokens or plain names (enhance_text_glyph_name), and the status
 * panel expands its tokens at every draw.
 */
#ifndef ENHANCE_TEXT_H
#define ENHANCE_TEXT_H

#include <stddef.h>
#include <stdint.h>

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

/* The same, with {ctl:} read against mod (an xbox_mod_count index; -1
 * for none). */
int enhance_text_draw_owned(mu_Context *ctx, mu_Font font, const char *text, int x, int y,
                            mu_Color color, int family, int device, int mod);
int enhance_text_width_owned(mu_Font font, const char *text, int family, int device, int mod);

/* glyph_name (mod.h, version 7) for mod: ref ("pad:south", "act:x",
 * "ctl:move", braces allowed) as its binding for device, written into out
 * as tokens, or with XBOX_GLYPH_TEXT as plain names ("Left stick",
 * "Guide+Y"; a stick's keys "W/A/S/D"). Returns the length; -1 when ref
 * names nothing (out is then untouched); XBOX_MOD_GLYPH_CUT when cap is
 * short, with out holding the whole members that fit, never part of one.
 * Any thread: the tables are read under their locks and the names are
 * the canonical ones, not the layout cache's. */
int enhance_text_glyph_name(int mod, const char *ref, char *out, size_t cap, uint32_t flags,
                            int family, int device);

/* Install the host's key namer for key caps (nv2a_ovl_glyph_set_key_namer).
 * enhance_text_draw does it on first use; a page that draws key caps
 * itself calls this first. */
void enhance_text_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_TEXT_H */
