/**
 * xbox_input names -- the vocabulary of key and mouse bindings.
 *
 * One table names every key and mouse input a binding may use, and one
 * names the Duke's controls. The configuration files ([input.bindings]),
 * the kbm_script injector and the key trace all speak it, so a name the
 * trace prints is a name a binding accepts.
 *
 * A key's code is its Windows virtual-key code, the index of the key table
 * in keyboard.c. Two codes are this table's own, for keys Win32 folds into
 * another: the left and right modifiers (0xA0..0xA5, as winuser.h numbers
 * them) beside the either-side Shift, Ctrl and Alt, and keypad Enter (0x0E,
 * unassigned in winuser.h) beside Return. A mouse input's code is 1..7 with
 * is_mouse set.
 *
 * No platform header, so tests/input_bindings round-trips the table on
 * every host.
 */
#ifndef XBOX_INPUT_NAMES_H
#define XBOX_INPUT_NAMES_H

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

/* Mouse inputs (is_mouse = 1). */
enum {
    XBOX_MOUSE_LEFT = 1, XBOX_MOUSE_RIGHT, XBOX_MOUSE_MIDDLE,
    XBOX_MOUSE_4, XBOX_MOUSE_5, XBOX_MOUSE_WHEEL_UP, XBOX_MOUSE_WHEEL_DOWN,
    XBOX_MOUSE_INPUTS = XBOX_MOUSE_WHEEL_DOWN
};

/* The Duke's controls, in the order [input.bindings] documents them. */
enum {
    XBOX_CTL_A, XBOX_CTL_B, XBOX_CTL_X, XBOX_CTL_Y,
    XBOX_CTL_WHITE, XBOX_CTL_BLACK, XBOX_CTL_LTRIGGER, XBOX_CTL_RTRIGGER,
    XBOX_CTL_START, XBOX_CTL_BACK, XBOX_CTL_LTHUMB, XBOX_CTL_RTHUMB,
    XBOX_CTL_DPAD_UP, XBOX_CTL_DPAD_DOWN, XBOX_CTL_DPAD_LEFT, XBOX_CTL_DPAD_RIGHT,
    XBOX_CTL_LSTICK_UP, XBOX_CTL_LSTICK_DOWN, XBOX_CTL_LSTICK_LEFT, XBOX_CTL_LSTICK_RIGHT,
    XBOX_CTL_RSTICK_UP, XBOX_CTL_RSTICK_DOWN, XBOX_CTL_RSTICK_LEFT, XBOX_CTL_RSTICK_RIGHT,
    XBOX_CTL_COUNT
};

/* A name (case-insensitive, aliases included) to its code: 1 and *code,
 * *is_mouse set, or 0 when the name is not in the table. LGui and RGui
 * (Cmd, the Windows key) are not: they belong to the system. */
int  xbox_InputNameToCode(const char *name, int *code, int *is_mouse);
/* The canonical name of a code, or NULL when no name has it. */
const char *xbox_InputCodeName(int code, int is_mouse);
/* A control name to its XBOX_CTL_* index, -1 when unknown. */
int  xbox_InputControlFromName(const char *name);
/* XBOX_CTL_* to its name, NULL out of range. */
const char *xbox_InputControlName(int control);

/* The name table, entry by entry (tests and docs): 0 past the end. Aliases
 * come back with *alias set. */
int  xbox_InputNameAt(int i, const char **name, int *code, int *is_mouse,
                      int *alias);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_NAMES_H */
