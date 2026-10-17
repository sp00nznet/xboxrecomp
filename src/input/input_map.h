/**
 * xbox_input -- host controller -> Xbox (Duke) gamepad mapping.
 *
 * One table (xbox_input_map[]) says which host control drives which field of
 * the guest's XBOX_GAMEPAD, and both host backends go through it: the XInput
 * backend (Windows, Proton) via xbox_InputMapXInput() and the SDL2
 * GameController backend via xbox_InputMapSdl(). The helpers are pure (no
 * platform calls, no SDL or XInput headers), so tests/input_map checks every
 * row on every host, and checks the table in src/input/README.md against
 * xbox_InputMapReadmeRow() so the documentation cannot drift from the code.
 *
 * Conventions (the Duke's, which the title was tuned for):
 *   - digital face buttons and shoulders become 255 (held) or 0 in
 *     bAnalogButtons[]; the console's pressure sensitivity is not available
 *     from a modern pad;
 *   - White = left shoulder (LB), Black = right shoulder (RB), Microsoft's own
 *     Xbox 360 convention and xemu's default;
 *   - triggers are 0..255 (XInput natively; SDL's 0..32767 shifted right 7);
 *   - sticks are signed 16-bit, up and right positive; SDL's Y axis points
 *     down and is inverted as (-1 - v), so -32768 cannot overflow;
 *   - no deadzone unless RECOMP_PAD_DEADZONE=N is set (the title applies its
 *     own); see xbox_InputApplyDeadzone().
 */
#ifndef XBOX_INPUT_MAP_H
#define XBOX_INPUT_MAP_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XInput's XINPUT_GAMEPAD, byte for byte, so the helper builds without
 * <xinput.h>. The Windows backend checks the layout with a static assert. */
typedef struct XBOX_HOST_XINPUT_GAMEPAD {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
} XBOX_HOST_XINPUT_GAMEPAD;

/* XINPUT_GAMEPAD_* button bits (xinput.h). */
#define XBOX_XI_DPAD_UP        0x0001
#define XBOX_XI_DPAD_DOWN      0x0002
#define XBOX_XI_DPAD_LEFT      0x0004
#define XBOX_XI_DPAD_RIGHT     0x0008
#define XBOX_XI_START          0x0010
#define XBOX_XI_BACK           0x0020
#define XBOX_XI_LEFT_THUMB     0x0040
#define XBOX_XI_RIGHT_THUMB    0x0080
#define XBOX_XI_LEFT_SHOULDER  0x0100
#define XBOX_XI_RIGHT_SHOULDER 0x0200
#define XBOX_XI_A              0x1000
#define XBOX_XI_B              0x2000
#define XBOX_XI_X              0x4000
#define XBOX_XI_Y              0x8000

/* SDL2's SDL_GameControllerButton and SDL_GameControllerAxis values (stable
 * ABI since SDL 2.0.2); the SDL backend checks them with static asserts. */
enum {
    XBOX_SDL_BUTTON_A = 0, XBOX_SDL_BUTTON_B, XBOX_SDL_BUTTON_X,
    XBOX_SDL_BUTTON_Y, XBOX_SDL_BUTTON_BACK, XBOX_SDL_BUTTON_GUIDE,
    XBOX_SDL_BUTTON_START, XBOX_SDL_BUTTON_LEFTSTICK,
    XBOX_SDL_BUTTON_RIGHTSTICK, XBOX_SDL_BUTTON_LEFTSHOULDER,
    XBOX_SDL_BUTTON_RIGHTSHOULDER, XBOX_SDL_BUTTON_DPAD_UP,
    XBOX_SDL_BUTTON_DPAD_DOWN, XBOX_SDL_BUTTON_DPAD_LEFT,
    XBOX_SDL_BUTTON_DPAD_RIGHT,
    XBOX_SDL_BUTTON_COUNT
};
enum {
    XBOX_SDL_AXIS_LEFTX = 0, XBOX_SDL_AXIS_LEFTY, XBOX_SDL_AXIS_RIGHTX,
    XBOX_SDL_AXIS_RIGHTY, XBOX_SDL_AXIS_TRIGGERLEFT,
    XBOX_SDL_AXIS_TRIGGERRIGHT,
    XBOX_SDL_AXIS_COUNT
};

/* A snapshot of an SDL GameController: button[i] = SDL_GameControllerGetButton
 * (i), axis[i] = SDL_GameControllerGetAxis(i), indexed by the enums above. */
typedef struct XBOX_HOST_SDL_GAMEPAD {
    uint8_t button[XBOX_SDL_BUTTON_COUNT];
    int16_t axis[XBOX_SDL_AXIS_COUNT];
} XBOX_HOST_SDL_GAMEPAD;

/* What a row writes in the guest's XBOX_GAMEPAD. */
enum xbox_map_kind {
    XBOX_MAP_DIGITAL,   /* wButtons |= guest */
    XBOX_MAP_ANALOG,    /* bAnalogButtons[guest] = 255 while held */
    XBOX_MAP_TRIGGER,   /* bAnalogButtons[guest] = trigger, 0..255 */
    XBOX_MAP_STICK      /* stick axis guest (0 LX, 1 LY, 2 RX, 3 RY) */
};

/* XInput analog sources (for TRIGGER and STICK rows). */
enum { XBOX_XI_SRC_LT, XBOX_XI_SRC_RT, XBOX_XI_SRC_LX, XBOX_XI_SRC_LY,
       XBOX_XI_SRC_RX, XBOX_XI_SRC_RY };

typedef struct xbox_input_map_row {
    const char *host;        /* host control, as documented */
    uint8_t     kind;        /* enum xbox_map_kind */
    uint16_t    guest;       /* wButtons bit, bAnalogButtons index or axis */
    uint16_t    xinput;      /* XInput bit (DIGITAL/ANALOG) or XBOX_XI_SRC_* */
    int8_t      sdl;         /* XBOX_SDL_BUTTON_* or XBOX_SDL_AXIS_* */
    int8_t      sdl_invert;  /* STICK: SDL axis points the other way */
    const char *guest_name;  /* guest field, as documented */
    const char *value;       /* guest value, as documented */
} xbox_input_map_row;

extern const xbox_input_map_row xbox_input_map[];
extern const size_t xbox_input_map_count;

/* Map one host pad to the guest gamepad (every field of *out is written).
 * Sticks go through xbox_InputApplyDeadzone() with xbox_InputDeadzone(). */
void xbox_InputMapXInput(const XBOX_HOST_XINPUT_GAMEPAD *in, XBOX_GAMEPAD *out);
void xbox_InputMapSdl(const XBOX_HOST_SDL_GAMEPAD *in, XBOX_GAMEPAD *out);

/* Radial deadzone of dz (0..32767) on one stick, rescaled so the edge of the
 * zone reads 0 and full deflection still reads full: |v| <= dz -> (0, 0),
 * otherwise the magnitude m becomes (m - dz) / (32767 - dz) * 32767 along the
 * same direction (m capped at 32767 for the scale, so corners and -32768 are
 * unchanged). dz <= 0 is the identity. */
void xbox_InputApplyDeadzone(SHORT *x, SHORT *y, int dz);

/* The deadzone the helpers apply: RECOMP_PAD_DEADZONE (0..32767, default 0),
 * read once. xbox_InputSetDeadzone overrides it (tests; -1 = back to env). */
int  xbox_InputDeadzone(void);
void xbox_InputSetDeadzone(int dz);

/* Line i of the README's mapping table, as Markdown (no newline): 0 is the
 * header, 1 the separator, 2.. the rows of xbox_input_map[] in order.
 * Returns the length written, or 0 past the end of the table. */
size_t xbox_InputMapReadmeRow(size_t i, char *buf, size_t size);

/* The README markers the table sits between. */
#define XBOX_INPUT_MAP_README_BEGIN "<!-- input_map:begin (generated from src/input/input_map.c; tests/input_map checks it) -->"
#define XBOX_INPUT_MAP_README_END   "<!-- input_map:end -->"

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_MAP_H */
