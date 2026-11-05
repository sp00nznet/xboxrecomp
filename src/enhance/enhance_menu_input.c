/*
 * The in-game menu's input rules that need no menu state
 * (enhance_menu_input.h).
 */
#include "enhance_menu_input.h"

#include <math.h>
#include <stdlib.h>

#include "enhance_menu.h"
#include "enhance_text.h"
#include "input_map.h"

int menu_focus_cycle(int focus, int dir)
{
    static const int k_order[] = { MENU_FOCUS_TOOLBAR, MENU_FOCUS_CONTENT, MENU_FOCUS_FOOTER };
    int i = 1;
    for (int k = 0; k < 3; k++)
        if (k_order[k] == focus)
            i = k;
    return k_order[(i + (dir < 0 ? 2 : 1)) % 3];
}

int menu_button_owner(int src)
{
    switch (src) {
    case INPUT_SRC_NORTH:
    case INPUT_SRC_BACK:
    case INPUT_SRC_LEFT_STICK_CLICK:
    case INPUT_SRC_RIGHT_STICK_CLICK:
    case INPUT_SRC_LEFT_SHOULDER:
    case INPUT_SRC_RIGHT_SHOULDER:
        return MENU_OWN_GLOBAL;
    case INPUT_SRC_SOUTH:
    case INPUT_SRC_EAST:
    case INPUT_SRC_WEST:
    case INPUT_SRC_DPAD_UP:
    case INPUT_SRC_DPAD_DOWN:
    case INPUT_SRC_DPAD_LEFT:
    case INPUT_SRC_DPAD_RIGHT:
    case INPUT_SRC_LSTICK_UP:
    case INPUT_SRC_LSTICK_DOWN:
    case INPUT_SRC_LSTICK_LEFT:
    case INPUT_SRC_LSTICK_RIGHT:
        return MENU_OWN_SECTION;
    default:
        return MENU_OWN_NONE;
    }
}

uint32_t menu_owner_mask(int owner)
{
    uint32_t m = 0;
    for (int src = 1; src < INPUT_SRC_COUNT && src < 32; src++)
        if (menu_button_owner(src) == owner)
            m |= 1u << src;
    return m;
}

/* The directions repeat while held and are read from the held set, not
 * from this table; they are listed so the ownership check covers them. */
static const struct menu_pad_event k_pad[] = {
    { INPUT_SRC_SOUTH, ENHANCE_EV_OK, 0 },
    { INPUT_SRC_EAST, ENHANCE_EV_BACK, 0 },
    { INPUT_SRC_WEST, ENHANCE_EV_CLEAR, 0 },
    { INPUT_SRC_NORTH, ENHANCE_EV_SAVE, 0 },
    { INPUT_SRC_LEFT_SHOULDER, ENHANCE_EV_PAGE_PREV, 0 },
    { INPUT_SRC_RIGHT_SHOULDER, ENHANCE_EV_PAGE_NEXT, 0 },
    { INPUT_SRC_BACK, ENHANCE_EV_CANCEL, 1 },
    { INPUT_SRC_LEFT_STICK_CLICK, ENHANCE_EV_HINTS, 1 },
    { INPUT_SRC_RIGHT_STICK_CLICK, ENHANCE_EV_SIZE, 1 },
    { INPUT_SRC_DPAD_UP, ENHANCE_EV_UP, 0 },
    { INPUT_SRC_DPAD_DOWN, ENHANCE_EV_DOWN, 0 },
    { INPUT_SRC_DPAD_LEFT, ENHANCE_EV_LEFT, 0 },
    { INPUT_SRC_DPAD_RIGHT, ENHANCE_EV_RIGHT, 0 },
    { INPUT_SRC_LSTICK_UP, ENHANCE_EV_UP, 0 },
    { INPUT_SRC_LSTICK_DOWN, ENHANCE_EV_DOWN, 0 },
    { INPUT_SRC_LSTICK_LEFT, ENHANCE_EV_LEFT, 0 },
    { INPUT_SRC_LSTICK_RIGHT, ENHANCE_EV_RIGHT, 0 },
};

const struct menu_pad_event *menu_pad_events(int *n)
{
    *n = (int)(sizeof k_pad / sizeof k_pad[0]);
    return k_pad;
}

int menu_pointer_threshold(float s)
{
    int t = (int)lroundf(2.0f * s);
    return t < 1 ? 1 : t;
}

int menu_pointer_moved(int ax, int ay, int x, int y, float s)
{
    int t = menu_pointer_threshold(s);
    return abs(x - ax) >= t || abs(y - ay) >= t;
}

int menu_tip_due(int device, uint64_t since, uint64_t now)
{
    uint64_t wait = device == ENHANCE_DEV_MOUSE ? MENU_TIP_HOVER_MS : MENU_TIP_SETTLE_MS;
    return since && now >= since && now - since >= wait;
}

int menu_hints_fit(const int *w, int n, int sep, int avail)
{
    int used = 0, k = 0;
    for (; k < n; k++) {
        int need = used + (k ? sep : 0) + w[k];
        if (need > avail)
            break;
        used = need;
    }
    return k;
}
