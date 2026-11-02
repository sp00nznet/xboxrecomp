/*
 * enhance_input.h - the [input] and [input.bindings] keys (the port-0
 * keyboard and mouse) read from the configuration files into src/input,
 * and the input a mod reads and captures (mod.h).
 */
#ifndef ENHANCE_INPUT_H
#define ENHANCE_INPUT_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by xbox_enhance_init after enhance_cfg_init: binds input.keyboard
 * and input.mouse to RECOMP_KEYBOARD and RECOMP_MOUSE, reads the knobs and
 * the bindings, applies them through src/input, and logs one
 * "[INPUT] config:" line. Errors are [INPUT] lines and never fatal; returns
 * 0. A second call re-reads (tests). */
int enhance_input_init(void);

/* The same again, live, from any thread (the menu): the mouse options go
 * through their ring, the key table and the stick options through one
 * committed snapshot the poll swaps in, then [input.actions] is rebuilt
 * against the new key map. The window re-grabs for a new mouse_capture at
 * its next event. Returns 0. */
int enhance_input_reload(void);

/* ---- a mod's input (src/enhance/mod.h) --------------------------------- */

/* What a mod reads each frame. */
struct enhance_input_state {
    XBOX_INPUT_STATE pad;       /* port 0 as the title last read it, with
                                 * the keyboard merged; all zero when none */
    int connected;              /* whether port 0 answered that read */
    int mouse_dx, mouse_dy;     /* raw counts since the last call, while
                                 * captured (mouse forward is negative dy);
                                 * 0, 0 otherwise */
    unsigned mouse_buttons;     /* bit (XBOX_MOUSE_x - 1) per held button */
    int captured;               /* enhance_input_captured() */
};

/* Takes port 0 for a mod, or gives it back. While captured the mouse's
 * motion goes to the mod (enhance_input_state) instead of the pad's look
 * stick, and the title's pad glue, which asks enhance_input_captured(),
 * gives the title a released pad: the title's own read is never touched
 * here, so the pad's state stays readable for the mod. Any thread. */
void enhance_input_capture(int on);
int  enhance_input_captured(void);
/* The enhancements menu holds the mouse while it is open: the cursor is
 * freed (xbox_InputMouseSuspend) and a mod's divert is off; a mod's capture
 * meanwhile is only recorded. 0 gives it back: the divert follows
 * enhance_input_captured() as it is then. */
void enhance_input_menu_hold(int on);
void enhance_input_state(struct enhance_input_state *out);

#ifdef __cplusplus
}
#endif

#endif /* ENHANCE_INPUT_H */
