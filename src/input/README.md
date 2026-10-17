# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `xinput_xbox.h` | 189 | Public header — types, button constants, function prototypes |
| `xinput_device.c` | 23 | Implementation (maps Xbox calls to Windows XInput) |
| `keyboard.c` | | The key table and the one key map, every host |
| `keyboard_sdl.c` | | The SDL window's key and focus events into that table (POSIX) |
| `input_map.c` | | Named bindings over that table, pad remapping, deadzones, merging (`xbox_input_map`) |

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
// Initialize (call once at startup)
void xbox_InputInit(void);

// Poll controller state (returns 0 on success, non-zero if disconnected)
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

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
XBOX_BUTTON_BLACK      4    // No equivalent on modern controllers
XBOX_BUTTON_WHITE      5    // No equivalent on modern controllers
XBOX_BUTTON_LTRIGGER   6    // Left trigger
XBOX_BUTTON_RTRIGGER   7    // Right trigger

XBOX_ANALOG_BUTTON_THRESHOLD  30   // Recommended press threshold
```

### Xbox → Modern Controller Mapping

| Xbox Button | XInput Equivalent | Notes |
|-------------|------------------|-------|
| A | A | Green button |
| B | B | Red button |
| X | X | Blue button |
| Y | Y | Yellow button |
| Black | Right Bumper | Mapped to RB |
| White | Left Bumper | Mapped to LB |
| L Trigger | Left Trigger | Analog 0-255 |
| R Trigger | Right Trigger | Analog 0-255 |
| Start | Start/Menu | |
| Back | Back/View | |
| D-pad | D-pad | Digital only |
| L Stick | L Stick | Click = L3 |
| R Stick | R Stick | Click = R3 |

## Ports

```c
#define XBOX_MAX_CONTROLLERS  4   // Ports 0-3
```

The Xbox supports 4 controllers. Each port can have a controller with optional memory units and other accessories. This layer only handles the gamepad; memory unit emulation is not needed for recompiled games.

## Keyboard → Xbox mapping

With `RECOMP_KEYBOARD=1` the keyboard is a pad on port 0, merged on top of a
controller there: a real pad keeps working and the keys only add. One map in
`keyboard.c` serves every host: the Win32 framebuffer window records Windows
virtual-key codes, and the SDL window on macOS and Linux turns SDL keycodes
into the same codes (`keyboard_sdl.c`). `tests/input_keyboard` checks every
row against an independent copy of this table.

The map is resolved by the mapping rules below (`input_map.c`), as the
bindings `xbox_InputKeyboardBindings` builds from it, so a named binding and
the map read the same table. The pad is the same as before the map used them:
`tests/input_keyboard_map` compares it, byte for byte, with a frozen copy of
v0.14.0's resolver.

- Keys are read only while the game's window has the focus. Losing the
  focus (and on SDL, hiding or minimising the window) releases every key.
- Letters follow the keyboard layout (the key labelled Z), not the physical
  position. Auto-repeat is ignored, and on macOS Cmd+key is left to the
  system (Cmd going down releases the held keys).
- Face buttons, shoulders and triggers read 255 held, 0 released; stick keys
  give full deflection, and opposite keys cancel.
- Every key a binding can name is recorded, not only the map's (Space,
  F-keys, the keypad, punctuation, Alt), and so are the mouse buttons the
  window receives. A left or right Shift, Ctrl or Alt is recorded both as
  its side's code (`VK_LSHIFT` ...) and as the either-side one (`VK_SHIFT`
  ...), which, as before, goes up when either side does. None of these
  extra keys is in the map, so the pad does not change.
- `RECOMP_KEY_TRACE=1` logs the first 40 key-downs as `[KEY] down vk=0x..`.

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

As binding text (`xbox_InputMap_BindingsFormat`), the same map is
`dpad_up = up`, `start = enter`, `back = backspace`, `left_thumb = shift`,
`right_thumb = ctrl`, `a = Z` ... `white = Q`, `left_trigger = 1`,
`left_stick_up = W, Numpad8` and so on: a port that lets the player rebind
starts from that.

## Bindings, remapping and deadzones

`input_map.h` (target `xbox_input_map`) holds the rules a port needs to let a
player choose their controls, on the same key table as the keyboard map above:

- **Named bindings.** Up to four keys or mouse buttons per Xbox control,
  written as text: `xbox_InputMap_BindingsParse(&b, INPUT_A, "L, Space, Mouse1")`
  and `xbox_InputMap_BindingsFormat` back. The names are the persistence
  format; a binding holds the Windows VK code from `keyboard.h`, so the
  held-key array is that table and there is one key space on every host.
  Names are ASCII case-insensitive. Unknown or oversized names and a fifth
  key are skipped and counted in the return value; duplicates are dropped;
  `none` or empty clears a control. Long text is read in full, not truncated.
- **Key resolution.** `xbox_InputMap_KeysResolve` turns held keys into an
  `XBOX_GAMEPAD`: the eight digital controls set their `wButtons` bit, the
  eight analog ones read 255, stick keys give full deflection and opposite
  keys cancel. Two directions at once give (32767, 32767), as the keyboard
  map always has; `b.diagonal = INPUT_DIAGONAL_ROUND` makes the stick a unit
  circle instead, (23169, 23169), so a diagonal is no faster than a straight
  line.
- **Pad remapping.** `xbox_InputMap_PadMap` takes each of the sixteen digital
  and trigger controls from one named physical control (SDL's positional
  names: `south` is the bottom face button), and each stick from either
  physical stick or none. `InputRaw.src[]` is 0..32767 (a button is 0 or
  32767; missing and negative read as 0); sticks are signed with Y positive
  upwards. Face buttons and shoulders come out as 0/255, triggers keep their
  travel. `xbox_InputMap_PadMapDefaults` is the identity (shoulders as Black
  and White, as `xinput_device.c` maps them) with 15% stick deadzones, a 5%
  trigger threshold and a 50% press threshold; it is an initializer, applied
  only by a port that calls it.
- **Deadzones.** `xbox_InputMap_ApplyStick` is radial: inside the deadzone the
  stick is centred, beyond it the range is stretched so full deflection stays
  full and the direction is kept. `xbox_InputMap_ApplyTrigger` zeroes a
  trigger below its threshold and stretches the rest to 0..255. Percentages
  are clamped to 0..95 (the press threshold to 0..100).
- **Merging.** `xbox_InputMap_PadMerge` ORs buttons, takes the larger analog
  value, and takes whichever whole stick is deflected further (the
  destination wins ties).

Key names: letters `A`..`Z`, digits `0`..`9`, `F1`..`F12`, `Numpad0`..`Numpad9`,
`numpadmultiply` `numpadadd` `numpadsubtract` `numpaddecimal` `numpaddivide`,
`space` `enter` (`return`) `backspace` `tab` `escape` (`esc`) `pause` `capslock`,
`up` `down` `left` `right` `home` `end` `pageup` `pagedown` `insert` `delete`,
`shift` `ctrl` (`control`) `alt` and the sides `lshift` `rshift` `lctrl` `rctrl`
`lalt` `ralt`, punctuation `semicolon` `equals` `comma` `minus` `period` `slash`
`grave` `leftbracket` `backslash` `rightbracket` `quote` (the US layout's keys,
as Windows reports them), and `mouse1`..`mouse5`. The mouse wheel has no VK
code and is not a key here.

Letters and punctuation are logical keys (the key labelled Z), not text or
physical positions; layouts and IME are the writer's business. `shift` and
`lshift` are different keys: a binding to `shift` is pressed by either side,
one to `lshift` only by the left, so the writers record both.

The module is pure arithmetic and text: no device, window or SDL, no mutable
globals. States belong to the caller; independent states can be used from
different threads. Required pointers must be non-NULL unless a function says
otherwise.

```c
#include "input_map.h"

InputBindings bindings = {0};
uint8_t held[INPUT_KEY_COUNT] = {0};
XBOX_GAMEPAD pad;

xbox_InputMap_BindingsParse(&bindings, INPUT_A, "space, mouse1");
held[XBOX_VK_SPACE] = 1;          /* or the key table, as keyboard.c reads it */
xbox_InputMap_KeysResolve(&bindings, held, &pad);
/* pad.bAnalogButtons[XBOX_BUTTON_A] == 255 */
```

A port links `xbox_input_map` (or `xbox_input`, which links it); a port
that wants only the rules can `add_subdirectory(src/input/mapping)` without
SDL or the rest of the runtime. It needs the C math library (`m` on non-MSVC
toolchains) for the radial deadzone. `tests/input_map` checks it with no
controller, game data or SDL:

```sh
cmake -S tests/input_map -B build/input-map -DCMAKE_BUILD_TYPE=Release
cmake --build build/input-map --config Release
ctest --test-dir build/input-map -C Release --output-on-failure
```
