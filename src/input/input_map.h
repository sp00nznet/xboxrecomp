/*
 * input_map -- what a host key, mouse button or pad control means to an Xbox
 * controller.
 *
 * Pure arithmetic and text: no Windows, no SDL, no mutable globals. A host
 * layer reads the devices and hands normalized values to this; keeping the
 * rules here is what lets a test check binding resolution and deadzones
 * without a pad on the desk. keyboard.c (RECOMP_KEYBOARD) is the first
 * adopter: its key map is resolved here.
 *
 * Three pieces:
 *
 *   Keyboard and mouse bindings. Each Xbox control has up to four host inputs
 *   bound to it, written as a comma separated list of names ("L, Space,
 *   Mouse1"). A bound key is a Windows virtual-key code, the same codes as
 *   the key table in keyboard.h, so the held-key array is that table and
 *   there is one key space on every host. The names are the persistence
 *   format; the codes are what the writers record.
 *
 *   Pad remapping. Each of the pad's sixteen digital and trigger controls
 *   takes its value from one named physical control (the names are SDL's
 *   positional ones: south is the bottom face button, an Xbox A, a PlayStation
 *   cross), and each stick can come from either physical stick or from none.
 *
 *   Deadzones. A radial deadzone for sticks that rescales so the stick still
 *   reaches full deflection, and a threshold for triggers.
 *
 * Every output is an XBOX_GAMEPAD (xinput_xbox.h): wButtons with the
 * XBOX_GAMEPAD_* bits, bAnalogButtons[] at the XBOX_BUTTON_* indices, and the
 * sticks with Y positive upwards.
 */
#ifndef XBOX_INPUT_MAP_H
#define XBOX_INPUT_MAP_H

#include <stdint.h>
#include "keyboard.h"     /* XBOX_VK_*, and XBOX_GAMEPAD through xinput_xbox.h */

#ifdef __cplusplus
extern "C" {
#endif

/* Pointer contract: structs, arrays and output pointers must be non-NULL unless
 * a function explicitly documents otherwise. Independent caller-owned states
 * may be used concurrently. Coordinate access to a shared mutable state. */

/* The Xbox controller's inputs, in the order settings list them. The first
 * eight are the wButtons bits in bit order (XBOX_GAMEPAD_DPAD_UP is bit 0),
 * the next eight the bAnalogButtons[] indices in order (INPUT_A is
 * XBOX_BUTTON_A); these sixteen are what a pad's physical controls can be
 * remapped onto. */
typedef enum InputControl {
    INPUT_DPAD_UP, INPUT_DPAD_DOWN, INPUT_DPAD_LEFT, INPUT_DPAD_RIGHT,
    INPUT_START, INPUT_BACK, INPUT_LEFT_THUMB, INPUT_RIGHT_THUMB,
    INPUT_A, INPUT_B, INPUT_X, INPUT_Y, INPUT_BLACK, INPUT_WHITE,
    INPUT_LEFT_TRIGGER, INPUT_RIGHT_TRIGGER,
    /* Keyboard only: the sticks as four keys each. */
    INPUT_LSTICK_UP, INPUT_LSTICK_DOWN, INPUT_LSTICK_LEFT, INPUT_LSTICK_RIGHT,
    INPUT_RSTICK_UP, INPUT_RSTICK_DOWN, INPUT_RSTICK_LEFT, INPUT_RSTICK_RIGHT,
    INPUT_CONTROL_COUNT
} InputControl;
#define INPUT_PAD_CONTROL_COUNT 16

/* Names use ASCII case-insensitive matching. The setting key for a control
 * ("dpad_up", "left_stick_left"), or NULL. */
const char *xbox_InputMap_ControlName(int control);
/* The control for a setting key, or -1. */
int xbox_InputMap_ControlFromName(const char *name);

/* Add `src` to `dst`: buttons are ORed, analog buttons take the larger, and a
 * stick takes whichever of the two is deflected further (the destination wins
 * ties). */
void xbox_InputMap_PadMerge(XBOX_GAMEPAD *dst, const XBOX_GAMEPAD *src);

/* ---- keyboard and mouse -------------------------------------------------- */

/* The held-key array KeysResolve reads: one entry per virtual-key code, the
 * same 256 as keyboard.h's table. */
#define INPUT_KEY_COUNT 256
#define INPUT_BINDINGS_PER_CONTROL 4

/* How a stick built from keys handles two directions at once. */
enum {
    /* Each axis at full deflection: a diagonal is (32767, 32767). What
     * keyboard.c has always done, and the default of a zeroed InputBindings. */
    INPUT_DIAGONAL_FULL = 0,
    /* A unit circle: a diagonal is (23169, 23169), so it is no faster than a
     * straight line. */
    INPUT_DIAGONAL_ROUND = 1,
};

typedef struct InputBindings {
    uint8_t key[INPUT_CONTROL_COUNT][INPUT_BINDINGS_PER_CONTROL];  /* VK codes, 1..255 */
    uint8_t count[INPUT_CONTROL_COUNT];
    uint8_t diagonal;                       /* INPUT_DIAGONAL_* */
} InputBindings;

/* The virtual-key code for a key name, case-insensitive ("a", "space",
 * "numpad4", "mouse1", "lshift"), or -1 if there is no such key. Aliases
 * ("return", "esc", "control") give the same code as the name they stand for. */
int xbox_InputMap_KeyFromName(const char *name);

/* Immutable canonical name for a virtual-key code, or NULL when the code has
 * no name. Safe across threads. */
const char *xbox_InputMap_KeyName(int vk);

/* Parse "L, Space" into `control`'s bindings, replacing them. "none" or an
 * empty string clears it. Names that are not keys are skipped and counted in
 * the return value; at most four keys are kept. Returns the number of names
 * rejected, or -1 for an invalid binding pointer/control. No fixed input-text
 * limit. */
int xbox_InputMap_BindingsParse(InputBindings *b, int control, const char *text);

/* The bindings of `control` as text that xbox_InputMap_BindingsParse reads back. */
const char *xbox_InputMap_BindingsFormat(const InputBindings *b, int control,
                                         char *buf, int size);

/* The pad a set of held keys makes. `down` has INPUT_KEY_COUNT entries indexed
 * by virtual-key code, non-zero for held. Digital controls set their wButtons
 * bit, analog ones read 255, and stick keys deflect fully, opposite keys
 * cancelling; two directions at once follow b->diagonal. */
void xbox_InputMap_KeysResolve(const InputBindings *b, const uint8_t *down,
                               XBOX_GAMEPAD *out);

/* ---- pad remapping and deadzones ------------------------------------------ */

/* The physical controls a pad has, SDL's positional names. */
typedef enum InputSource {
    INPUT_SRC_NONE,
    INPUT_SRC_SOUTH, INPUT_SRC_EAST, INPUT_SRC_WEST, INPUT_SRC_NORTH,
    INPUT_SRC_BACK, INPUT_SRC_GUIDE, INPUT_SRC_START,
    INPUT_SRC_LEFT_STICK_CLICK, INPUT_SRC_RIGHT_STICK_CLICK,
    INPUT_SRC_LEFT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER,
    INPUT_SRC_DPAD_UP, INPUT_SRC_DPAD_DOWN, INPUT_SRC_DPAD_LEFT, INPUT_SRC_DPAD_RIGHT,
    INPUT_SRC_MISC1, INPUT_SRC_TOUCHPAD,
    INPUT_SRC_PADDLE1, INPUT_SRC_PADDLE2, INPUT_SRC_PADDLE3, INPUT_SRC_PADDLE4,
    INPUT_SRC_LEFT_TRIGGER, INPUT_SRC_RIGHT_TRIGGER,
    /* A stick pushed one way, 0 to full. */
    INPUT_SRC_LSTICK_UP, INPUT_SRC_LSTICK_DOWN, INPUT_SRC_LSTICK_LEFT, INPUT_SRC_LSTICK_RIGHT,
    INPUT_SRC_RSTICK_UP, INPUT_SRC_RSTICK_DOWN, INPUT_SRC_RSTICK_LEFT, INPUT_SRC_RSTICK_RIGHT,
    INPUT_SRC_COUNT
} InputSource;

const char *xbox_InputMap_SourceName(int source);
int xbox_InputMap_SourceFromName(const char *name);      /* -1 if unknown */

/* The adapter must populate directional sources explicitly if they are used
 * as bindings; stick axes are not automatically copied into src[].
 * Missing sources are zero. Negative src[] values are treated as zero.
 * Digital/face-button mappings use button_threshold (clamped to 0..100%).
 */

/* What the host read from a pad before any mapping. Every source is 0 to
 * 32767 (a button is 0 or 32767); the sticks are signed, Y positive upwards. */
typedef struct InputRaw {
    int16_t src[INPUT_SRC_COUNT];
    int16_t lx, ly, rx, ry;
} InputRaw;

enum { INPUT_STICK_LEFT, INPUT_STICK_RIGHT, INPUT_STICK_NONE };

typedef struct InputPadMap {
    uint8_t source[INPUT_PAD_CONTROL_COUNT];   /* InputSource for each control */
    uint8_t left_stick;                        /* INPUT_STICK_*: which physical stick */
    uint8_t right_stick;
    uint8_t deadzone_left;                     /* percent, 0-95 */
    uint8_t deadzone_right;
    uint8_t trigger_threshold;                 /* percent, 0-95 */
    uint8_t button_threshold;                  /* percent a trigger/stick must reach to count as a button */
} InputPadMap;

/* The identity mapping: a pad as the Xbox pad it is shaped like, with the
 * shoulders as Black (left) and White (right) like xinput_device.c.
 * Deadzones 15%, trigger threshold 5%, an axis counts as a button at 50%. */
void xbox_InputMap_PadMapDefaults(InputPadMap *m);

/* Apply `m` to a raw pad. */
void xbox_InputMap_PadMap(const InputPadMap *m, const InputRaw *raw, XBOX_GAMEPAD *out);

/* A radial deadzone (percent clamped to 0..95): a stick inside `deadzone_pct` of full deflection reads as
 * centred, and the range beyond it is stretched so full deflection is still
 * full. The direction is preserved. */
void xbox_InputMap_ApplyStick(int x, int y, int deadzone_pct, SHORT *ox, SHORT *oy);

/* A trigger threshold is clamped to 0..95. A trigger value (0-255) below `threshold_pct` of the range reads 0; the rest
 * is stretched to 0-255. */
uint8_t xbox_InputMap_ApplyTrigger(int value, int threshold_pct);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_MAP_H */
