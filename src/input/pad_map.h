/**
 * xbox_input -- host controller -> Xbox (Duke) gamepad mapping.
 *
 * One table (xbox_pad_map[]) says which host control drives which field of
 * the guest's XBOX_GAMEPAD, and both host backends go through it: the XInput
 * backend (Windows, Proton) via xbox_PadMapXInput() and the SDL3
 * gamepad backend via xbox_PadMapSdl(). The helpers are pure (no
 * platform calls, no SDL or XInput headers), so tests/pad_map checks every
 * row on every host, and checks the table in src/input/README.md against
 * xbox_PadMapReadmeRow() so the documentation cannot drift from the code.
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
#ifndef XBOX_PAD_MAP_H
#define XBOX_PAD_MAP_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"
#include "input_map.h"

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

/* SDL3's SDL_GamepadButton and SDL_GamepadAxis values, the same numbers
 * SDL2's controller enums had; the SDL backend checks them with static
 * asserts. SDL3 names the face buttons by position (SOUTH, EAST, WEST,
 * NORTH), which are the Xbox A, B, X and Y places the Duke mapping wants, so
 * the names here keep the Xbox letters. */
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

/* A snapshot of an SDL gamepad: button[i] = SDL_GetGamepadButton(i),
 * axis[i] = SDL_GetGamepadAxis(i), indexed by the enums above. */
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

typedef struct xbox_pad_map_row {
    const char *host;        /* host control, as documented */
    uint8_t     kind;        /* enum xbox_map_kind */
    uint16_t    guest;       /* wButtons bit, bAnalogButtons index or axis */
    uint16_t    xinput;      /* XInput bit (DIGITAL/ANALOG) or XBOX_XI_SRC_* */
    int8_t      sdl;         /* XBOX_SDL_BUTTON_* or XBOX_SDL_AXIS_* */
    int8_t      sdl_invert;  /* STICK: SDL axis points the other way */
    const char *guest_name;  /* guest field, as documented */
    const char *value;       /* guest value, as documented */
} xbox_pad_map_row;

extern const xbox_pad_map_row xbox_pad_map[];
extern const size_t xbox_pad_map_count;

/* Map one host pad to the guest gamepad (every field of *out is written).
 * Sticks go through xbox_InputApplyDeadzone() with xbox_InputDeadzone(). */
void xbox_PadMapXInput(const XBOX_HOST_XINPUT_GAMEPAD *in, XBOX_GAMEPAD *out);
void xbox_PadMapSdl(const XBOX_HOST_SDL_GAMEPAD *in, XBOX_GAMEPAD *out);

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
 * header, 1 the separator, 2.. the rows of xbox_pad_map[] in order.
 * Returns the length written, or 0 past the end of the table. */
size_t xbox_PadMapReadmeRow(size_t i, char *buf, size_t size);

/* The README markers the table sits between. */
#define XBOX_PAD_MAP_README_BEGIN "<!-- pad_map:begin (generated from src/input/pad_map.c; tests/pad_map checks it) -->"
#define XBOX_PAD_MAP_README_END   "<!-- pad_map:end -->"

/* ── the InputRaw adapter (upstream's pad map, input_map.h) ──────────────
 *
 * The runtime path is host pad -> InputRaw -> xbox_InputMap_PadMap. The
 * adapter below fills an InputRaw the way #203 asks of "a port that calls
 * it": every button source 0 or 32767, the triggers 0..32767, the sticks
 * signed with up and right positive, and the stick directions as the
 * clamped magnitude of each half-axis (the opposite half reads 0). The
 * host-only buttons (host_pad.h's XBOX_HOST_* bits in `extra`) become the
 * guide, misc1, paddle and touchpad sources.
 *
 * XInput's triggers are 0..255; they are scaled as (t << 7) | (t >> 1), so
 * 255 is 32767 and PadMap's >> 7 gives back t exactly. SDL's are already
 * 0..32767 (negative reads 0). */
void xbox_PadRawXInput(const XBOX_HOST_XINPUT_GAMEPAD *in, uint32_t extra, InputRaw *out);
void xbox_PadRawSdl(const XBOX_HOST_SDL_GAMEPAD *in, uint32_t extra, InputRaw *out);

/* The inverse of the identity, for a source that only has a Duke state (a
 * test's fake backend): buttons, analog buttons past 0, triggers through the
 * XInput scaling, sticks as they are. */
void xbox_PadRawFromGamepad(const XBOX_GAMEPAD *g, uint32_t extra, InputRaw *out);

/* input.pad_invert_ly / _ry: negate a host Y axis before the map, so a
 * swapped stick keeps its own inversion; -32768 becomes 32767. The
 * direction sources follow. */
void xbox_PadRawInvert(InputRaw *raw, int invert_ly, int invert_ry);

/* The XBOX_HOST_* bits of the host-only sources in raw. */
uint32_t xbox_PadRawExtra(const InputRaw *raw);

/* ── the pad map the runtime applies ([input.pad]) ────────────────────── */

typedef struct xbox_pad_setup {
    InputPadMap map;        /* the primary binding of each control */
    InputPadMap alt;        /* the second one; every source and stick none
                               unless [input.pad] gives a control two */
    uint8_t invert_ly;      /* input.pad_invert_ly / _ry (xbox_PadRawInvert) */
    uint8_t invert_ry;
} xbox_pad_setup;

/* The fork's identity: xbox_InputMap_PadMapDefaults, then White = left
 * shoulder and Black = right shoulder (Microsoft's Xbox 360 convention and
 * xemu's; upstream's default has them the other way), deadzones 0 and
 * trigger threshold 0 (the title applies its own), and no alternate map.
 * Through xbox_PadSetupApply it gives what xbox_PadMapXInput and
 * xbox_PadMapSdl give, bit for bit (tests/pad_map). */
void xbox_PadSetupIdentity(xbox_pad_setup *s);
int  xbox_PadSetupIsIdentity(const xbox_pad_setup *s);

/* Nonzero when a map names a host-only source (guide, misc1, a paddle, the
 * touchpad), so the backend has to read them on a stock poll too. */
int  xbox_PadSetupNeedsExtras(const xbox_pad_setup *s);

/* The guest pad for raw (already inverted). Each map goes through
 * xbox_InputMap_PadMap; a guest stick whose map has deadzone 0 is then
 * overwritten with the raw physical stick that map selects (the swap
 * honoured, none centred) and RECOMP_PAD_DEADZONE's xbox_InputApplyDeadzone,
 * because PadMap's radial rescale clamps to the unit circle even at 0 and
 * real pads report corners outside it. The alternate map, when it has any
 * source, is merged with xbox_InputMap_PadMerge. Every field of *out is
 * written. */
void xbox_PadSetupApply(const xbox_pad_setup *s, const InputRaw *raw, XBOX_GAMEPAD *out);

/* Install the setup input_core.c applies to every pad read (any thread; a
 * poll in flight finishes with the one it loaded). NULL restores the
 * identity. xbox_InputPadSetup copies the current one. */
void xbox_InputSetPadSetup(const xbox_pad_setup *s);
void xbox_InputPadSetup(xbox_pad_setup *out);

/* Host rumble gate, for the SDL backend. A title asks for vibration at its
 * own rate (BLiNX, for example, sends an XID SET_REPORT about 21 times a
 * second, rumble or not), so a level that varies can change on every one.
 * Each change SDL's HIDAPI drivers turn into an output report, and their
 * rumble thread holds the device lock across the write, while the input
 * update only tries that lock and skips the pad when it is held. On macOS the write is a
 * synchronous IOHIDDeviceSetReport, which can be slow over Bluetooth. A
 * pad that loses its input (it keeps its last stick and buttons, still
 * connected, and the character runs on by itself) was seen with an 8BitDo
 * pad over Bluetooth after a rumble. The cause is not confirmed: a write
 * that blocks for long, or a pad that stops reporting while it is flooded
 * with output reports. Fewer writes lower the exposure to both, though
 * they cannot cure a write that blocks. The gate passes a change at most
 * once per XBOX_RUMBLE_MIN_MS, the latest one winning, and a stop at once,
 * so at most a change and a stop reach the pad per window. Levels are
 * compared as the pad receives them (the high byte), so a change SDL would
 * write as the same report is no change. A held non-zero level is renewed,
 * on an ask, every XBOX_RUMBLE_REFRESH_MS so the duration SDL needs does
 * not run out while the title keeps asking (SDL writes nothing for an
 * unchanged level); a title that stops asking without a stop stops
 * rumbling within SDL's 1 s duration. Pure: the caller owns the clock and
 * the lock. */
#define XBOX_RUMBLE_MIN_MS      100u
#define XBOX_RUMBLE_REFRESH_MS  500u

typedef struct xbox_rumble_gate {
    uint16_t want_l, want_r;    /* the latest level asked for */
    uint16_t sent_l, sent_r;    /* the last level handed to the host */
    uint64_t sent_ms;           /* when that level changed; 0 = never */
    uint64_t renew_ms;          /* when it was last changed or renewed */
} xbox_rumble_gate;

/* Forget everything: a new pad starts still. */
void xbox_RumbleGateReset(xbox_rumble_gate *g);
/* Record what the guest asks for; nothing reaches the host here. */
void xbox_RumbleGateSet(xbox_rumble_gate *g, uint16_t left, uint16_t right);
/* 1 with *left/*right set when the host should be given that level now
 * (and the gate counts it sent), else 0. Call it with asked = 1 right after
 * each Set, and with asked = 0 from a loop that keeps running, so a
 * held-back change still lands; only an ask repeats a held level. */
int  xbox_RumbleGatePoll(xbox_rumble_gate *g, uint64_t now_ms, int asked,
                         uint16_t *left, uint16_t *right);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PAD_MAP_H */
