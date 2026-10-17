/**
 * Xbox Input Compatibility Layer - Type Definitions
 *
 * Defines the Xbox XPP (Xbox Peripheral Port) input types and
 * translates them to Windows XInput. The Xbox controller API
 * differs from XInput in structure layout and polling model.
 *
 * The game uses 9 XPP entry points with 12 total calls:
 * - Controller state polling
 * - Vibration/force feedback
 * - Device enumeration
 */

#ifndef BURNOUT3_XINPUT_XBOX_H
#define BURNOUT3_XINPUT_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox input types
 * ================================================================ */

/* Xbox controller port count */
#define XBOX_MAX_CONTROLLERS 4

/* Xbox gamepad button flags */
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

/* Xbox analog button thresholds */
#define XBOX_ANALOG_BUTTON_THRESHOLD 30

typedef struct XBOX_GAMEPAD {
    WORD  wButtons;             /* Digital button bitmask */
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig (0-255) */
    SHORT sThumbLX;             /* Left stick X (-32768 to 32767) */
    SHORT sThumbLY;             /* Left stick Y */
    SHORT sThumbRX;             /* Right stick X */
    SHORT sThumbRY;             /* Right stick Y */
} XBOX_GAMEPAD;

typedef struct XBOX_INPUT_STATE {
    DWORD dwPacketNumber;
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct XBOX_VIBRATION {
    WORD wLeftMotorSpeed;       /* Low-frequency rumble (0-65535) */
    WORD wRightMotorSpeed;      /* High-frequency rumble (0-65535) */
} XBOX_VIBRATION;

typedef struct XBOX_INPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XBOX_GAMEPAD Gamepad;
    XBOX_VIBRATION Vibration;
} XBOX_INPUT_CAPABILITIES;

/* Analog button indices */
#define XBOX_BUTTON_A           0
#define XBOX_BUTTON_B           1
#define XBOX_BUTTON_X           2
#define XBOX_BUTTON_Y           3
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the input system (idempotent; the other calls do it lazily).
 * Port N is XInput user index N, or SDL GameController slot N. Registers
 * xbox_InputShutdown() with atexit().
 */
void xbox_InputInit(void);

/**
 * Stop every motor the title started. Safe to call more than once.
 */
void xbox_InputShutdown(void);

/**
 * Get the state of a controller, mapped to the Duke layout (input_map.h).
 * Port: 0-3 (Xbox controller ports). A port last seen disconnected returns
 * ERROR_DEVICE_NOT_CONNECTED without asking the host until a second has
 * passed since it was last probed.
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/**
 * The host pad on `slot` alone: the same probe and cache rule as
 * xbox_InputGetState, without the keyboard and mouse merge on 0. For a
 * game that maps host slots onto its ports itself and merges the keyboard
 * on its own port 0 (once a poll: the keyboard read consumes mouse motion).
 */
DWORD xbox_InputGetPadState(DWORD slot, XBOX_INPUT_STATE *pState);

/**
 * The keyboard-and-mouse state `kb` merged on top of the pad state `pad`,
 * as xbox_InputGetState does on port 0: buttons OR-ed, analog values the
 * larger, a non-zero key or mouse axis replacing the stick's, and the
 * keyboard's packet number (the title's input layer looks for edges).
 */
void xbox_InputMergeKbm(XBOX_INPUT_STATE *pad, const XBOX_INPUT_STATE *kb);

/**
 * Set controller vibration.
 */
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

/**
 * Check if a controller is connected (the cache; refreshed at most once a
 * second for a port the title does not read).
 */
BOOL xbox_InputIsConnected(DWORD dwPort);

/**
 * Get controller capabilities.
 */
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */
