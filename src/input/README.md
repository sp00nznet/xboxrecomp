# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Files

| File | Purpose |
|------|---------|
| `xinput_xbox.h` | Public header: types, button constants, function prototypes |
| `input_map.h`, `input_map.c` | The host → Xbox mapping table, the pure map and deadzone helpers |
| `input_core.c` | The public API: connection cache, probe backoff, keyboard merge, shutdown |
| `input_backend.h` | The seam between `input_core.c` and a host backend (tests swap it) |
| `xinput_device.c` | Host backends: XInput (Windows, Proton) and SDL2 GameController |
| `keyboard.c` | The key table, the compiled-in key map, the runtime bindings and the keyboard-and-mouse composer, every host |
| `mouse.h`, `mouse.c` | The mouse: motion as a stick, buttons, the wheel, the capture state machine; its `_WIN32` half is the Win32 windows' mouse |
| `input_names.h`, `input_names.c` | The binding vocabulary: key, mouse and control names |
| `kbm_script.c` | `RECOMP_DEBUG=kbm_script=<file>`: key and mouse events injected on the poll |
| `keyboard_sdl.c` | The SDL window's key and focus events into that table (POSIX) |
| `mouse_sdl.c` | The SDL window's mouse events, and the capture as relative mode (POSIX) |

## Quick Start

```c
#include "xinput_xbox.h"

// Initialize input system
xbox_InputInit();

// Poll controller state (port 0-3)
XBOX_INPUT_STATE state;
if (xbox_InputGetState(0, &state) == 0) {
    // Digital buttons
    if (state.Gamepad.wButtons & XBOX_GAMEPAD_START)
        pause_game();

    // Analog buttons (0-255 pressure)
    uint8_t trigger_r = state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    if (trigger_r > XBOX_ANALOG_BUTTON_THRESHOLD)
        accelerate(trigger_r / 255.0f);

    // Stick axes (-32768 to +32767)
    float steer = state.Gamepad.sThumbLX / 32768.0f;
}

// Vibration feedback
XBOX_VIBRATION vib = { .wLeftMotorSpeed = 32000, .wRightMotorSpeed = 16000 };
xbox_InputSetState(0, &vib);
```

## API

```c
// Initialize (idempotent; the other calls do it lazily)
void xbox_InputInit(void);

// Stop every motor (also run at exit)
void xbox_InputShutdown(void);

// Poll controller state (returns 0 on success, non-zero if disconnected)
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

// The pad on a host slot alone: same probe rule, no keyboard/mouse merge
DWORD xbox_InputGetPadState(DWORD slot, XBOX_INPUT_STATE *pState);

// Merge a keyboard/mouse state onto a pad (the rule GetState uses on port 0)
void xbox_InputMergeKbm(XBOX_INPUT_STATE *pad, const XBOX_INPUT_STATE *kb);

// Set vibration motors
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

// Check if controller is connected
BOOL xbox_InputIsConnected(DWORD dwPort);

// Query controller capabilities
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);
```

## Types

```c
typedef struct {
    WORD  wButtons;                     // Digital button bitmask
    BYTE  bAnalogButtons[8];            // Analog button pressure (0-255)
    SHORT sThumbLX, sThumbLY;           // Left stick (-32768 to +32767)
    SHORT sThumbRX, sThumbRY;           // Right stick
} XBOX_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;               // Increments on state change
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct {
    WORD wLeftMotorSpeed;               // 0-65535
    WORD wRightMotorSpeed;              // 0-65535
} XBOX_VIBRATION;
```

## Button Constants

### Digital Buttons (wButtons bitmask)

```c
XBOX_GAMEPAD_DPAD_UP         0x0001
XBOX_GAMEPAD_DPAD_DOWN       0x0002
XBOX_GAMEPAD_DPAD_LEFT       0x0004
XBOX_GAMEPAD_DPAD_RIGHT      0x0008
XBOX_GAMEPAD_START            0x0010
XBOX_GAMEPAD_BACK             0x0020
XBOX_GAMEPAD_LEFT_THUMB       0x0040    // Left stick click
XBOX_GAMEPAD_RIGHT_THUMB      0x0080    // Right stick click
```

### Analog Buttons (bAnalogButtons[] indices)

The original Xbox had pressure-sensitive face buttons (0-255):

```c
XBOX_BUTTON_A          0    // Also used for "boost" in racing games
XBOX_BUTTON_B          1
XBOX_BUTTON_X          2
XBOX_BUTTON_Y          3
XBOX_BUTTON_BLACK      4    // Right shoulder (RB) on a modern pad
XBOX_BUTTON_WHITE      5    // Left shoulder (LB) on a modern pad
XBOX_BUTTON_LTRIGGER   6    // Left trigger
XBOX_BUTTON_RTRIGGER   7    // Right trigger

XBOX_ANALOG_BUTTON_THRESHOLD  30   // Recommended press threshold
```

### Host controller → Xbox mapping

One table in `input_map.c` drives both backends (XInput on Windows and
Proton, SDL2 GameController elsewhere); `tests/input_map` checks every row
and fails if the table below and the code disagree. Regenerate it with
`INPUT_MAP_README_OUT=<file>` set when running the test.

- White is the left shoulder (LB) and Black the right shoulder (RB):
  Microsoft's own Xbox 360 convention, and xemu's default.
- Face buttons and shoulders are digital on a modern pad, so the guest sees
  255 or 0; the Duke's pressure sensitivity is not available. A title that
  thresholds at `XBOX_ANALOG_BUTTON_THRESHOLD` (30) sees no difference.
- Sticks pass at the host's full range with no deadzone (the title applies
  its own). `RECOMP_PAD_DEADZONE=N` (0..32767, default 0) adds a radial
  deadzone of N, rescaled so full deflection still reads full, for pads
  that drift.
- SDL's stick Y points down and is inverted as `-1 - v`, so -32768 and
  32767 swap exactly (an SDL stick at rest reads -1 on Y).

<!-- input_map:begin (generated from src/input/input_map.c; tests/input_map checks it) -->
| Host control | Guest field | Guest value | XInput | SDL2 |
|---|---|---|---|---|
| A | `bAnalogButtons[A]` | 255 held, 0 released | `A` | `A` |
| B | `bAnalogButtons[B]` | 255 held, 0 released | `B` | `B` |
| X | `bAnalogButtons[X]` | 255 held, 0 released | `X` | `X` |
| Y | `bAnalogButtons[Y]` | 255 held, 0 released | `Y` | `Y` |
| Left shoulder (LB) | `bAnalogButtons[WHITE]` | 255 held, 0 released | `LEFT_SHOULDER` | `LEFTSHOULDER` |
| Right shoulder (RB) | `bAnalogButtons[BLACK]` | 255 held, 0 released | `RIGHT_SHOULDER` | `RIGHTSHOULDER` |
| Start / Menu | `wButtons START` | bit set while held | `START` | `START` |
| Back / View | `wButtons BACK` | bit set while held | `BACK` | `BACK` |
| Left stick click | `wButtons LEFT_THUMB` | bit set while held | `LEFT_THUMB` | `LEFTSTICK` |
| Right stick click | `wButtons RIGHT_THUMB` | bit set while held | `RIGHT_THUMB` | `RIGHTSTICK` |
| D-pad up | `wButtons DPAD_UP` | bit set while held | `DPAD_UP` | `DPAD_UP` |
| D-pad down | `wButtons DPAD_DOWN` | bit set while held | `DPAD_DOWN` | `DPAD_DOWN` |
| D-pad left | `wButtons DPAD_LEFT` | bit set while held | `DPAD_LEFT` | `DPAD_LEFT` |
| D-pad right | `wButtons DPAD_RIGHT` | bit set while held | `DPAD_RIGHT` | `DPAD_RIGHT` |
| Left trigger | `bAnalogButtons[LTRIGGER]` | 0..255 | `bLeftTrigger` | `TRIGGERLEFT` |
| Right trigger | `bAnalogButtons[RTRIGGER]` | 0..255 | `bRightTrigger` | `TRIGGERRIGHT` |
| Left stick X | `sThumbLX` | -32768..32767, right positive | `sThumbLX` | `LEFTX` |
| Left stick Y | `sThumbLY` | -32768..32767, up positive | `sThumbLY` | `LEFTY` as `-1 - v` |
| Right stick X | `sThumbRX` | -32768..32767, right positive | `sThumbRX` | `RIGHTX` |
| Right stick Y | `sThumbRY` | -32768..32767, up positive | `sThumbRY` | `RIGHTY` as `-1 - v` |
<!-- input_map:end -->

### Keyboard → Xbox mapping

With `RECOMP_KEYBOARD=1` the keyboard is a pad on port 0, merged on top of
a controller there: a real pad keeps working and the keys only add. The
table below is the compiled-in map: the runtime bindings start as a copy of
it, and a title's settings file replaces them (see "Keyboard and mouse"
below). One map in `keyboard.c` serves every host: the Win32
windows (GDI and D3D11, under Proton too) record Windows virtual-key codes,
and the SDL window on macOS and Linux turns SDL keycodes into the same codes
(`keyboard_sdl.c`). `tests/input_keyboard` checks every row and fails if the
table below and the code disagree; regenerate it with
`INPUT_KEYBOARD_README_OUT=<file>` set when running the test.

- Keys are read only while the game's window has the focus. Losing the
  focus (and on SDL, hiding or minimising the window) releases every key.
- Letters follow the keyboard layout (the key labelled Z), not the
  physical position. The digits differ between hosts: on AZERTY the top-row
  keys labelled 1 and 3 give SDL the keycodes for `&` and `"` (an SDL
  keycode ignores Shift), so the 1 and 3 trigger keys do nothing in the
  SDL window there, while Win32 reports the same keys as VK `1` and `3`. Auto-repeat is ignored, and on macOS Cmd+key is left to
  the system (Cmd going down releases the held keys).
- Face buttons, shoulders and triggers read 255 held, 0 released; stick keys
  give full deflection (unless `stick_ramp_ms` is set), and opposite keys
  cancel.
- `RECOMP_TRACE=key` logs every key and mouse edge by its binding name
  (`[KEY] down Return`), the first 200 and then one in 50.

<!-- keyboard_map:begin (generated from src/input/keyboard.c; tests/input_keyboard checks it) -->
| Key | Guest control |
|---|---|
| Up arrow | D-pad up |
| Down arrow | D-pad down |
| Left arrow | D-pad left |
| Right arrow | D-pad right |
| Enter | START |
| Backspace | BACK |
| Shift | Left stick click |
| Ctrl | Right stick click |
| Z | A |
| X | B |
| C | X |
| V | Y |
| Q | White |
| E | Black |
| 1 | Left trigger |
| 3 | Right trigger |
| W or keypad 8 | Left stick up |
| S or keypad 2 | Left stick down |
| A or keypad 4 | Left stick left |
| D or keypad 6 | Left stick right |
| I | Right stick up |
| K | Right stick down |
| J | Right stick left |
| L | Right stick right |
<!-- keyboard_map:end -->

### Keyboard and mouse

The keyboard and the mouse are one source on port 0 (`xbox_InputKbmPoll`,
the backend hook), switched by `RECOMP_KEYBOARD` and `RECOMP_MOUSE` or by a
title's `[input]` table (`src/enhance/enhance_input.c`, documented in
[`docs/runtime/config.md`](../../docs/runtime/config.md#input)). It composes
the pad from the runtime bindings: a control (`a`, `start`, `lstick_up`,
...) holds up to eight inputs, and any of them held drives it.

- **Vocabulary** (`input_names.c`, case-insensitive): `A`..`Z`, `0`..`9`,
  `F1`..`F12`, `Space`, `Return` (alias `Enter`), `Escape` (alias `Esc`),
  `Backspace`, `Tab`, `LShift`/`RShift`/`Shift`, `LCtrl`/`RCtrl`/`Ctrl`,
  `LAlt`/`RAlt`/`Alt`, the arrows `Up`/`Down`/`Left`/`Right`, `Insert`,
  `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `KP0`..`KP9`, `KPEnter`,
  `KPPlus`, `KPMinus`, `KPMultiply`, `KPDivide`, `KPPeriod`, the US-labelled
  punctuation (`Comma`, `Period`, `Slash`, `Semicolon`, `Quote`,
  `LBracket`, `RBracket`, `Backslash`, `Minus`, `Equals`, `Grave`), and
  `MouseLeft`, `MouseRight`, `MouseMiddle`, `Mouse4`, `Mouse5`, `WheelUp`,
  `WheelDown`. `Shift`, `Ctrl` and `Alt` hear either side; keypad Enter also
  holds `Return`, as Win32 reports it. `LGui`/`RGui` (Cmd, the Windows key)
  are the system's and have no name.
- **Controls**: `a`, `b`, `x`, `y`, `white`, `black`, `ltrigger`,
  `rtrigger`, `start`, `back`, `lthumb`, `rthumb`, `dpad_up`, `dpad_down`,
  `dpad_left`, `dpad_right`, `lstick_up`, `lstick_down`, `lstick_left`,
  `lstick_right`, `rstick_up`, `rstick_down`, `rstick_left`,
  `rstick_right`.
- **Stick keys**: with `stick_ramp_ms` above 0 a stick key starts at 0.41 of
  full deflection (just past a 0.4 deadzone) and reaches full over that
  time; two perpendicular keys give 1/√2 each, and the walk key caps the
  magnitude at `walk_scale`. With 0, every key is full deflection as the
  table above gives.
- **Mouse**: the counts since the last poll become a stick deflection
  (`min_deflection + (1 - min_deflection) · min(1, counts · sensitivity / 40)`),
  replacing the keys' value on that axis for that poll; a mouse at rest reads
  zero the next poll. A wheel notch holds its control for `wheel_pulse_ms`,
  then one poll released before the next notch.
- **Capture**: the cursor is captured (hidden, confined, relative) while the
  window has focus (`focus`, the default), after a click in it (`click`), or
  never (`never`). Focus loss, the release key (`F9` by default, a toggle)
  and `xbox_InputMouseSuspend(1)` (an overlay menu) free it, and each
  transition logs `[INPUT] mouse captured` or `[INPUT] mouse released
  (<reason>)`. A button press while free drives nothing, and in `focus` mode
  neither does the click that focused the window: Win32's
  `WM_MOUSEACTIVATE` names it, and otherwise (SDL, or Wine when the window
  manager focuses first) it is the first press within
  `XBOX_MOUSE_FOCUS_CLICK_MS` (250 ms) of the focus.
- `RECOMP_TRACE=mouse` logs the model per poll: `[MOUSE] dx= dy= -> RS(x,y)`.
- `RECOMP_DEBUG=kbm_script=<file>` injects key and mouse events on the poll
  (one per line: `<seconds>|<N>p key down|up NAME`, `mouse move DX DY`,
  `mouse down|up NAME`, `wheel N`); it captures the mouse itself and is
  refused when the title runs its own input script.

A title's own layout belongs in its settings file (BLiNX 2's is
`packaging/config.default.toml`), not in the compiled-in map, which stays
the fallback for titles without one. `tests/input_bindings` checks the
names, the composer, the mouse model, the capture rules, the injector and
the merge on every host.

## Connection state and rumble

`xbox_InputGetState` reads a connected port at every call. A port last seen
disconnected is not asked again until a second has passed since its last
probe (XInput's guidance); `xbox_InputIsConnected` answers from the same
cache and refreshes it under the same rule, so a pad plugged in later
appears within a second. Port N is XInput user index N. On SDL hosts a pad
keeps its port while it stays attached, and ports never shift: a removed pad
frees its port, and a new one takes the port the same model (joystick GUID)
last held when that is free and another pad is still connected, else the
lowest free port, so a pad that connects while no other pad is connected
takes port 0. Device events and a once-a-second rescan both feed this, run
by the thread that pumps SDL: the window's loop (`xbox_InputSdlPadSync`), or
the poll when headless, since sdl2-compat's device-index calls are not safe
beside another thread's pump; each change logs `[INPUT] host pad added
port=N "name"` or `[INPUT] host pad removed port=N`. With a window, the
first poll waits up to 250 ms for the window's loop to open the pads, so a
pad connected before launch is connected at the first read.
`tests/input_hotplug_sdl` checks it with SDL's virtual joysticks.
`xbox_InputSetState` reaches only connected ports; `xbox_InputShutdown`
(also registered with `atexit` by `xbox_InputInit`) stops every motor.

## Ports

```c
#define XBOX_MAX_CONTROLLERS  4   // Ports 0-3
```

The Xbox supports 4 controllers. Each port can have a controller with optional memory units and other accessories. This layer only handles the gamepad; memory unit emulation is not needed for recompiled games.
