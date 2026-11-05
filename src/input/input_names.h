/**
 * xbox_input names -- the vocabulary of key and mouse bindings.
 *
 * The names are upstream's (input_map.h): its control names
 * (xbox_InputMap_ControlName), its key names (xbox_InputMap_KeyName) and its
 * pad source names for the host-only buttons. This file maps them onto the
 * fork's code space and adds the three inputs upstream has no virtual-key
 * code for. The configuration files ([input.bindings], [input.actions]), the
 * kbm_script injector and the key trace all speak it, so a name the trace
 * prints is a name a binding accepts.
 *
 * A key's code is its Windows virtual-key code, the index of the key table
 * in keyboard.c. Two codes are the fork's own, for keys Win32 folds into
 * another: the left and right modifiers (0xA0..0xA5, as winuser.h numbers
 * them) beside the either-side Shift, Ctrl and Alt, and keypad Enter (0x0E,
 * unassigned in winuser.h, named "numpadenter") beside Enter. A mouse input's
 * code is 1..7 with is_mouse set: upstream's mouse1..mouse5, then the wheel
 * as "wheelup" and "wheeldown".
 *
 * No platform header, so tests/input_bindings round-trips the table on
 * every host.
 */
#ifndef XBOX_INPUT_NAMES_H
#define XBOX_INPUT_NAMES_H

#include "input_map.h"    /* InputControl, the upstream names */

#ifdef __cplusplus
extern "C" {
#endif

/* The codes this table adds to winuser.h's. */
#define XBOX_VK_KP_ENTER  0x0E
#define XBOX_VK_MENU      0x12      /* Alt, either side */
#define XBOX_VK_LSHIFT    0xA0
#define XBOX_VK_RSHIFT    0xA1
#define XBOX_VK_LCONTROL  0xA2
#define XBOX_VK_RCONTROL  0xA3
#define XBOX_VK_LMENU     0xA4
#define XBOX_VK_RMENU     0xA5

/* Mouse inputs (is_mouse = 1): mouse1..mouse5, wheelup, wheeldown. */
enum {
    XBOX_MOUSE_LEFT = 1, XBOX_MOUSE_RIGHT, XBOX_MOUSE_MIDDLE,
    XBOX_MOUSE_4, XBOX_MOUSE_5, XBOX_MOUSE_WHEEL_UP, XBOX_MOUSE_WHEEL_DOWN,
    XBOX_MOUSE_INPUTS = XBOX_MOUSE_WHEEL_DOWN
};

/* A name (case-insensitive, upstream's aliases included) to its code: 1 and
 * *code, *is_mouse set, or 0 when the name is not a key or mouse input.
 * LGui and RGui (Cmd, the Windows key) have none: they belong to the
 * system. */
int  xbox_InputNameToCode(const char *name, int *code, int *is_mouse);
/* The canonical name of a code, or NULL when no name has it. */
const char *xbox_InputCodeName(int code, int is_mouse);

/* The host-only buttons a host action may use (host_pad.h): no guest
 * control has them. Index i is the XBOX_HOST_* bit 1 << i: guide, misc1
 * (share, capture), paddle1..4 (upper right, upper left, lower right,
 * lower left) and touchpad, named as upstream's pad sources. */
#define XBOX_HOST_BUTTONS 7
/* A host-only button name (case-insensitive) to its index, -1 when unknown. */
int  xbox_InputHostButtonFromName(const char *name);
/* Index to name, NULL out of range. */
const char *xbox_InputHostButtonName(int index);

/* Every canonical name, one by one (tests and docs): keys in code order,
 * then the mouse inputs. 0 past the end. */
int  xbox_InputNameAt(int i, const char **name, int *code, int *is_mouse);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_NAMES_H */
