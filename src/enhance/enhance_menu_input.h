/*
 * enhance_menu_input.h - the in-game menu's input rules that need no menu
 * state (overlay-revamp D3, D9, D11): which pad button belongs to whom, the
 * pointer's motion threshold, the focus regions' Tab order, the tooltip's
 * timers and how many footer hint pairs fit. enhance_menu.c holds the
 * state and calls these; tests/enhance_menu checks them on their own.
 *
 * Pure C11: no overlay, clock or input reads.
 */
#ifndef ENHANCE_MENU_INPUT_H
#define ENHANCE_MENU_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The focus regions (D3). HEADER is the mouse's only: the pad has L3 and R3
 * for its buttons, and Tab skips it. */
enum menu_focus { MENU_FOCUS_CONTENT, MENU_FOCUS_TOOLBAR, MENU_FOCUS_FOOTER, MENU_FOCUS_HEADER };

/* Tab (dir 1) and Shift+Tab (-1): TOOLBAR, CONTENT, FOOTER, round. */
int menu_focus_cycle(int focus, int dir);

/* Who a pad button (INPUT_SRC_*) belongs to. GLOBAL buttons do the same on
 * every section (Y Save, View Cancel, L3 hints, R3 size, LB/RB section), so
 * no section, row or mod page binds one; SECTION buttons (A, B, X, the
 * d-pad, the left stick) are the focused region's. A captured-control row
 * takes every button only while it captures, which is no binding. */
enum menu_owner { MENU_OWN_NONE, MENU_OWN_GLOBAL, MENU_OWN_SECTION };
int      menu_button_owner(int src);
uint32_t menu_owner_mask(int owner);    /* bit 1 << src per button */

/* The pad's menu events: each button, the event it sends (ENHANCE_EV_*) and
 * whether on release (View, L3, R3: a game's menu chord may use them,
 * A.6). input_step reads this table, and the ctest checks every event a
 * section owns sits on a SECTION button and every global one on a GLOBAL
 * button. */
struct menu_pad_event {
    int src;
    int ev;
    int on_release;
};
const struct menu_pad_event *menu_pad_events(int *n);

/* Real pointer motion (D3): at least 2 px at ui scale s since the anchor,
 * so a mouse resting on a row never takes the selection from a pad or key
 * press. */
int menu_pointer_threshold(float s);
int menu_pointer_moved(int ax, int ay, int x, int y, float s);

/* D11's tooltip: due MENU_TIP_SETTLE_MS after a pad or key selection
 * settles, MENU_TIP_HOVER_MS after the pointer rests on a row. */
#define MENU_TIP_SETTLE_MS 350u
#define MENU_TIP_HOVER_MS  500u
int menu_tip_due(int device, uint64_t since, uint64_t now);

/* overlay.menu_hints (D11). */
enum menu_hints { MENU_HINTS_TOOLTIP, MENU_HINTS_INLINE, MENU_HINTS_OFF };

/* The footer's hint pairs (D9): of n pairs with widths w[], how many from
 * the left fit in avail with sep between two; the rest drop from the
 * right. */
int menu_hints_fit(const int *w, int n, int sep, int avail);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_MENU_INPUT_H */
