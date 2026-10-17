/**
 * xbox_input keyboard -- the host keyboard as a port-0 pad.
 *
 * One key table and one key map for every host. The table is indexed by
 * Windows virtual-key codes, because that is what the Win32 windows already
 * receive; the SDL window (keyboard_sdl.c) turns SDL keycodes into the same
 * codes. Writers: the GDI window (fb_present.c) and the SDL main loop
 * (fb_present_sdl.c). Reader: xbox_InputGetState() for port 0, through
 * xbox_InputKeyboardPoll().
 *
 * The map is resolved by the mapping rules of input_map.h, which read this
 * same table, so named bindings and the map share one key space.
 *
 * No window or SDL header here, so tests/input_keyboard checks the map on
 * every host.
 */
#ifndef XBOX_INPUT_KEYBOARD_H
#define XBOX_INPUT_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Windows VK codes the map and the binding names use (winuser.h).
 * Letters and digits are their uppercase ASCII codes, as in Win32. The
 * OEM punctuation codes are the US layout's, as Windows reports them. */
#define XBOX_VK_LBUTTON   0x01      /* mouse buttons, in the same table */
#define XBOX_VK_RBUTTON   0x02
#define XBOX_VK_MBUTTON   0x04
#define XBOX_VK_XBUTTON1  0x05
#define XBOX_VK_XBUTTON2  0x06
#define XBOX_VK_BACK      0x08
#define XBOX_VK_TAB       0x09
#define XBOX_VK_RETURN    0x0D
#define XBOX_VK_SHIFT     0x10      /* either side, as well as the side's own */
#define XBOX_VK_CONTROL   0x11
#define XBOX_VK_MENU      0x12      /* Alt */
#define XBOX_VK_PAUSE     0x13
#define XBOX_VK_CAPITAL   0x14      /* Caps Lock */
#define XBOX_VK_ESCAPE    0x1B
#define XBOX_VK_SPACE     0x20
#define XBOX_VK_PRIOR     0x21      /* Page Up */
#define XBOX_VK_NEXT      0x22      /* Page Down */
#define XBOX_VK_END       0x23
#define XBOX_VK_HOME      0x24
#define XBOX_VK_LEFT      0x25
#define XBOX_VK_UP        0x26
#define XBOX_VK_RIGHT     0x27
#define XBOX_VK_DOWN      0x28
#define XBOX_VK_INSERT    0x2D
#define XBOX_VK_DELETE    0x2E
#define XBOX_VK_NUMPAD0   0x60      /* through XBOX_VK_NUMPAD9, 0x69 */
#define XBOX_VK_NUMPAD2   0x62
#define XBOX_VK_NUMPAD4   0x64
#define XBOX_VK_NUMPAD6   0x66
#define XBOX_VK_NUMPAD8   0x68
#define XBOX_VK_NUMPAD9   0x69
#define XBOX_VK_MULTIPLY  0x6A      /* keypad * + - . / */
#define XBOX_VK_ADD       0x6B
#define XBOX_VK_SUBTRACT  0x6D
#define XBOX_VK_DECIMAL   0x6E
#define XBOX_VK_DIVIDE    0x6F
#define XBOX_VK_F1        0x70      /* through XBOX_VK_F12, 0x7B */
#define XBOX_VK_F12       0x7B
#define XBOX_VK_LSHIFT    0xA0
#define XBOX_VK_RSHIFT    0xA1
#define XBOX_VK_LCONTROL  0xA2
#define XBOX_VK_RCONTROL  0xA3
#define XBOX_VK_LMENU     0xA4
#define XBOX_VK_RMENU     0xA5
#define XBOX_VK_OEM_1     0xBA      /* ; */
#define XBOX_VK_OEM_PLUS  0xBB      /* = */
#define XBOX_VK_OEM_COMMA 0xBC
#define XBOX_VK_OEM_MINUS 0xBD
#define XBOX_VK_OEM_PERIOD 0xBE
#define XBOX_VK_OEM_2     0xBF      /* / */
#define XBOX_VK_OEM_3     0xC0      /* ` */
#define XBOX_VK_OEM_4     0xDB      /* [ */
#define XBOX_VK_OEM_5     0xDC      /* \ */
#define XBOX_VK_OEM_6     0xDD      /* ] */
#define XBOX_VK_OEM_7     0xDE      /* ' */

/* Record a key as held (down != 0) or released. vk outside 0..255 is
 * ignored. */
void xbox_InputKeySet(int vk, int down);
/* Release every key (focus lost). */
void xbox_InputKeysClear(void);

/* For the Win32 window (fb_present.c), here so every host tests them.
 *
 * WM_KEYDOWN reports both Shifts as VK_SHIFT, both Ctrls as VK_CONTROL and
 * both Alts as VK_MENU. The side-specific code (VK_LSHIFT ...) for such a
 * message, from its lParam (scan code in bits 16-23, extended-key flag in
 * bit 24), or 0 for any other key. The window records it beside wParam, so
 * a binding to "lshift" can be held. */
int  xbox_InputWin32SideVk(int vk, uint32_t lparam);
/* A WM_[LRMX]BUTTONDOWN/UP message as its VK (VK_LBUTTON ...), with *down
 * set, or 0 for any other message. wparam is the message's wParam, whose
 * high word says which X button. */
int  xbox_InputWin32MouseVk(unsigned msg, uint32_t wparam, int *down);
/* Non-zero while that virtual key is held in the game's window. Always zero
 * when there is no window, which is the right answer: with nothing to focus
 * there is nothing to type into. Kept under its old name for usb_gamepad.c's
 * diagnostic. */
int  xbox_FramebufferKeyDown(int vk);

/* The pad the held keys make, through the one map, resolved by the mapping
 * rules (input_map.h). Moves the packet number on every call, as the title
 * looks for edges. */
void xbox_InputKeyboardState(XBOX_INPUT_STATE *out);
/* The map below as bindings (input_map.h): what xbox_InputKeyboardState
 * resolves, and what xbox_InputMap_BindingsFormat writes as text. */
struct InputBindings;
void xbox_InputKeyboardBindings(struct InputBindings *b);
/* 0 when RECOMP_KEYBOARD is off, else 1 with *out filled by
 * xbox_InputKeyboardState(). */
int  xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out);

/* The map, row by row (tests, and the table in README.md). */
enum { XBOX_KEY_BUTTON, XBOX_KEY_ANALOG, XBOX_KEY_AXIS };
enum { XBOX_KEY_LX, XBOX_KEY_LY, XBOX_KEY_RX, XBOX_KEY_RY };
typedef struct xbox_key_row {
    const char *keys;          /* as the README names them */
    uint8_t     vk[2];         /* 0 = no second key */
    uint8_t     kind;          /* XBOX_KEY_* */
    uint16_t    target;        /* wButtons bit, bAnalogButtons index, or axis */
    int8_t      sign;          /* XBOX_KEY_AXIS: -1 or +1 */
    const char *guest;         /* the guest control, as the README names it */
} xbox_key_row;

extern const xbox_key_row xbox_key_map[];
extern const size_t       xbox_key_map_count;

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_H */
