/**
 * xbox_input keyboard -- the host keyboard as a port-0 pad.
 *
 * One key table and one key map for every host. The table is indexed by
 * Windows virtual-key codes, because that is what the Win32 windows already
 * receive; the SDL window (keyboard_sdl.c) turns SDL keycodes into the same
 * codes. Writers: the GDI window (fb_present.c), the D3D11 window
 * (nv2a_pb_d3d11.c) and the SDL main loop. Reader: the game's poll, through
 * xbox_InputKeyboardPoll() as a backend's keyboard hook.
 *
 * No platform header here, so tests/input_keyboard checks the map on every
 * host and checks the table in src/input/README.md against
 * xbox_InputKeyMapReadmeRow().
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

/* App shortcuts that never reach the key table (macOS Cmd+S, which the SDL
 * path keeps from the game): the host posts the key, the menu takes it
 * once. Lock-free; a second post before the take is one press. */
void xbox_InputShortcutPost(int vk);
int  xbox_InputShortcutTake(int vk);

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

/* The pad the held keys make, through the compiled-in map below (not the
 * runtime bindings). Moves the packet number on every call, as the title
 * looks for edges. */
void xbox_InputKeyboardState(XBOX_INPUT_STATE *out);
/* The compiled-in map as input_map.h bindings, what
 * xbox_InputMap_BindingsFormat writes as text. */
struct InputBindings;
void xbox_InputKeyboardBindings(struct InputBindings *b);
/* A keyboard hook: 0 when the keyboard is off (xbox_InputKeyboardOn), else
 * 1 with *out filled by xbox_InputKeyboardState(). The host backends use
 * xbox_InputKbmPoll() below; this stays for callers of the old map. */
int  xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out);

/* ---- keyboard and mouse as one source (KBM) ----------------------------
 *
 * The compiled-in map above is the fallback and what the legacy calls use.
 * A runtime binding table starts as a copy of it and is what the backend
 * hook, xbox_InputKbmPoll(), composes from; the configuration layer
 * (src/enhance/enhance_input.c) or a title rebinds it before the first
 * poll. Controls and codes are input_names.h's. */

/* A host key edge from a window or the injector: records code (a VK, or
 * input_names.h's side-specific modifiers and keypad Enter, which also hold
 * the either-side Shift/Ctrl/Alt and Return), tells the mouse about the
 * release key, and traces the edge by name (RECOMP_TRACE=key). */
void xbox_InputKeyEvent(int code, int down);
/* The same from a Win32 WM_KEY* message: wparam's VK and lparam, whose
 * scancode and extended bit say which Shift, Ctrl, Alt or Enter it was. */
void xbox_InputKeyEventWin32(int vk, unsigned long lparam, int down);
/* The Win32 window's keyboard layout (an HKL; 0 when no Win32 window
 * posted one). Layouts are per thread: GetKeyboardLayout on the overlay's
 * thread names keys in that thread's layout, not the one the player types
 * in. So the window's thread posts its own at start and on each
 * WM_INPUTLANGCHANGE, and key names read it here. */
void      xbox_InputKeyboardSetLayout(uintptr_t hkl);
uintptr_t xbox_InputKeyboardLayout(void);
/* One line of the key trace: "[KEY] " + the text, the first 200 edges and
 * then one in 50, so a held key or a busy wheel cannot flood the log. */
void xbox_InputTraceEdge(const char *fmt, ...);

/* The runtime table is edited as a staging copy. Until the first Commit
 * the poll composes from the staging copy itself (a title or a test that
 * binds before its first poll); Commit publishes a snapshot of it and of
 * xbox_InputKeyboardOpts, which the poll thread takes at its next poll by
 * one pointer swap, so a live rebuild (the menu, a reload) never changes
 * the table under a poll in flight. Commit is any thread; Reset also drops
 * the committed table at the next poll (tests). Stage copies the
 * compiled-in map into the staging copy only: the poll keeps the committed
 * table until the next Commit, so a live rebuild never shows it a half-built
 * one. */
void xbox_InputBindingsCommit(void);
void xbox_InputBindingsStage(void);
/* Reset copies the compiled-in map back. Bind adds one
 * input to a control: 0, or -1 when the control already has
 * XBOX_BIND_MAX inputs or an argument is out of range. BindingsOf copies
 * up to max of a control's inputs out and returns how many it has. */
#define XBOX_BIND_MAX 8
void xbox_InputBindingsReset(void);
void xbox_InputBindingsClear(int control);
int  xbox_InputBind(int control, int code, int is_mouse);
int  xbox_InputBindingsOf(int control, int *codes, int *is_mouse, int max);
/* Stick keys: reach full deflection over stick_ramp_ms (0: at once, each
 * key full deflection as the compiled-in map always gave), from a floor of
 * XBOX_KBM_STICK_FLOOR; while walk_code (a key, 0: none) is held, at most
 * walk_scale of full. Code defaults: 0, 0, 1.0. */
#define XBOX_KBM_STICK_FLOOR 0.41
void xbox_InputKeyboardOpts(int stick_ramp_ms, int walk_code, double walk_scale);
/* The walk key the last KeyboardOpts set (0: none), for the host actions'
 * check that an action key is not one the guest receives. */
int  xbox_InputKeyboardWalkKey(void);
/* The poll's clock (input_core.c installs the backend's now_ms). Without
 * one a ramp is instant. */
void xbox_InputKbmSetClock(uint64_t (*now_ms)(void));

/* Whether the keys are a source: -1 goes back to RECOMP_KEYBOARD. The mouse
 * has the same pair in mouse.h. */
void xbox_InputKeyboardEnable(int on);
int  xbox_InputKeyboardOn(void);

/* The pad the runtime table makes from the held keys (keys != 0) and the
 * mouse (mouse != 0). Moves the packet number on every call. */
void xbox_InputKbmState(XBOX_INPUT_STATE *out, int keys, int mouse);
/* The backend hook: plays the kbm_script injector, then 0 when neither the
 * keys nor the mouse is on, else 1 with *out from xbox_InputKbmState(). */
int  xbox_InputKbmPoll(XBOX_INPUT_STATE *out);

/* kbm_script (RECOMP_DEBUG=kbm_script=<file>, kbm_script.c). A title that
 * runs its own input script passes 0 before the first poll so the two
 * never race on port 0. */
void xbox_InputKbmScriptAllow(int allow);
/* Parse a script from text (tests; the poll loads the file itself).
 * Returns the number of events, bad lines reported and skipped. */
int  xbox_InputKbmScriptParse(const char *text, const char *name);
/* Once per poll from xbox_InputKbmPoll: applies the events now due. */
void xbox_InputKbmScriptPoll(uint64_t now_ms, int have_clock);
/* Forget the script (tests). */
void xbox_InputKbmScriptReset(void);

/* The map, row by row (tests and the README). */
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

/* README line i of the generated table (header lines first); 0 past the
 * end, else the length written. */
size_t xbox_InputKeyMapReadmeRow(size_t i, char *buf, size_t size);

#define XBOX_KEY_MAP_README_BEGIN "<!-- keyboard_map:begin (generated from src/input/keyboard.c; tests/input_keyboard checks it) -->"
#define XBOX_KEY_MAP_README_END   "<!-- keyboard_map:end -->"

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_H */
