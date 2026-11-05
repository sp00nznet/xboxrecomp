# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Files

| File | Purpose |
|------|---------|
| `xinput_xbox.h` | Public header: types, button constants, function prototypes |
| `pad_map.h`, `pad_map.c` | The host → Xbox mapping table, the pure map and deadzone helpers |
| `input_map.h`, `input_map.c` | Named bindings over the key table, pad remapping, deadzones, merging (`xbox_input_map`) |
| `input_core.c` | The public API: connection cache, probe backoff, keyboard merge, shutdown |
| `input_backend.h` | The seam between `input_core.c` and a host backend (tests swap it) |
| `host_pad.h` | The host pad before any merge (`struct xbox_host_pad`, the host-only buttons) and the pad filter seam |
| `xinput_device.c` | Host backends: XInput (Windows, Proton) and the SDL3 gamepad API |
| `keyboard.c` | The key table, the compiled-in key map, the runtime bindings and the keyboard-and-mouse composer, every host |
| `mouse.h`, `mouse.c` | The mouse: motion as a stick, buttons, the wheel, the capture state machine; its `_WIN32` half is the Win32 windows' mouse |
| `input_names.h`, `input_names.c` | The binding vocabulary: key, mouse and control names |
| `kbm_script.c` | `RECOMP_DEBUG=kbm_script=<file>`: key and mouse events injected on the poll |
| `keyboard_sdl.c` | The SDL window's key and focus events into that table (POSIX) |
| `mouse_sdl.c` | The SDL window's mouse events, and the capture as relative mode (POSIX) |
| `text_input.h`, `text_input.c` | Typed text for the menu's fields: the lock-free ring and the Begin/End request, every host; its `_WIN32` half reads `WM_CHAR` |
| `text_input_sdl.c` | `SDL_EVENT_TEXT_INPUT` into the ring, and the request as SDL's text input and its area (POSIX) |

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

// Record port 0's state as the title got it, from a pad source (below)
void xbox_InputKeepLastState(DWORD dwPort, DWORD rc, const XBOX_INPUT_STATE *pState);
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

One table in `pad_map.c` drives both backends (XInput on Windows and
Proton, the SDL3 gamepad API elsewhere); `tests/pad_map` checks every row
and fails if the table below and the code disagree. Regenerate it with
`PAD_MAP_README_OUT=<file>` set when running the test.

- White is the left shoulder (LB) and Black the right shoulder (RB):
  Microsoft's own Xbox 360 convention, and xemu's default.
- Face buttons and shoulders are digital on a modern pad, so the guest sees
  255 or 0; the Duke's pressure sensitivity is not available. A title that
  thresholds at `XBOX_ANALOG_BUTTON_THRESHOLD` (30) sees no difference.
- Sticks pass at the host's full range with no deadzone (the title applies
  its own). `RECOMP_PAD_DEADZONE=N` (0..32767, default 0) adds a radial
  deadzone of N, rescaled so full deflection still reads full, for pads
  that drift.
- A modern pad's gate is round, the Duke's was square: its diagonal read
  about (32767, 32767). `input.pad_square_left` / `_right` (enhancements,
  default off) stretch a host stick along its angle by
  `max(1, min(|v|, 32767) / max(|x|, |y|))`, so a round diagonal reaches
  the corner again; cardinals, the deadzone and -32768 are unchanged.
  `_left` also keeps ramped left-stick keys' diagonal at the corner
  (not with the walk key held).
- SDL's stick Y points down and is inverted as `-1 - v`, so -32768 and
  32767 swap exactly (an SDL stick at rest reads -1 on Y).
- SDL3 names the face buttons by position: `SOUTH`, `EAST`, `WEST` and
  `NORTH` are where A, B, X and Y sit on an Xbox pad, so a pad with other
  labels (Nintendo's swapped A/B, say) maps by place, not by letter.

<!-- pad_map:begin (generated from src/input/pad_map.c; tests/pad_map checks it) -->
| Host control | Control | Guest field | Guest value | XInput | SDL3 |
|---|---|---|---|---|---|
| `south` | `a` | `bAnalogButtons[A]` | 255 held, 0 released | `A` | `SOUTH` |
| `east` | `b` | `bAnalogButtons[B]` | 255 held, 0 released | `B` | `EAST` |
| `west` | `x` | `bAnalogButtons[X]` | 255 held, 0 released | `X` | `WEST` |
| `north` | `y` | `bAnalogButtons[Y]` | 255 held, 0 released | `Y` | `NORTH` |
| `left_shoulder` | `white` | `bAnalogButtons[WHITE]` | 255 held, 0 released | `LEFT_SHOULDER` | `LEFT_SHOULDER` |
| `right_shoulder` | `black` | `bAnalogButtons[BLACK]` | 255 held, 0 released | `RIGHT_SHOULDER` | `RIGHT_SHOULDER` |
| `start` | `start` | `wButtons START` | bit set while held | `START` | `START` |
| `back` | `back` | `wButtons BACK` | bit set while held | `BACK` | `BACK` |
| `left_stick_click` | `left_thumb` | `wButtons LEFT_THUMB` | bit set while held | `LEFT_THUMB` | `LEFT_STICK` |
| `right_stick_click` | `right_thumb` | `wButtons RIGHT_THUMB` | bit set while held | `RIGHT_THUMB` | `RIGHT_STICK` |
| `dpad_up` | `dpad_up` | `wButtons DPAD_UP` | bit set while held | `DPAD_UP` | `DPAD_UP` |
| `dpad_down` | `dpad_down` | `wButtons DPAD_DOWN` | bit set while held | `DPAD_DOWN` | `DPAD_DOWN` |
| `dpad_left` | `dpad_left` | `wButtons DPAD_LEFT` | bit set while held | `DPAD_LEFT` | `DPAD_LEFT` |
| `dpad_right` | `dpad_right` | `wButtons DPAD_RIGHT` | bit set while held | `DPAD_RIGHT` | `DPAD_RIGHT` |
| `left_trigger` | `left_trigger` | `bAnalogButtons[LTRIGGER]` | 0..255 | `bLeftTrigger` | `LEFT_TRIGGER` |
| `right_trigger` | `right_trigger` | `bAnalogButtons[RTRIGGER]` | 0..255 | `bRightTrigger` | `RIGHT_TRIGGER` |
| `left_stick_left / left_stick_right` | `left_stick_left` / `left_stick_right` | `sThumbLX` | -32768..32767, right positive | `sThumbLX` | `LEFTX` |
| `left_stick_down / left_stick_up` | `left_stick_down` / `left_stick_up` | `sThumbLY` | -32768..32767, up positive | `sThumbLY` | `LEFTY` as `-1 - v` |
| `right_stick_left / right_stick_right` | `right_stick_left` / `right_stick_right` | `sThumbRX` | -32768..32767, right positive | `sThumbRX` | `RIGHTX` |
| `right_stick_down / right_stick_up` | `right_stick_down` / `right_stick_up` | `sThumbRY` | -32768..32767, up positive | `sThumbRY` | `RIGHTY` as `-1 - v` |
<!-- pad_map:end -->

**Host-only buttons.** A host pad has buttons the Duke never had, and the
table above maps none of them to the guest. The backends report them
beside the guest state as `host_extra` (`host_pad.h`: `XBOX_HOST_GUIDE`,
`MISC1`, `PADDLE1`..`PADDLE4`, `TOUCHPAD`). SDL3 reads Guide and, in a
second loop past the guest map, `MISC1`, the four paddles and the touchpad
(`XBOX_SDL_BUTTON_COUNT` stays 15). XInput has Guide only, through
`XInputGetStateEx` (xinput1_4 ordinal 100, resolved at run time; without
it Guide reads 0). `xbox_InputHostExtra(slot)` answers what the last read
of a slot saw. `xbox_input_backend_read_host(slot, &host)` reads a slot
off the poll thread (the flip tick below) and clears the bits when no pad
is open, so a Guide held as the pad went away does not stay held. Under
Steam Input, and on some hosts by default, the system keeps Guide; the
per-host table is in `docs/runtime/config.md` ("Host actions").

**The pad filter.** `xbox_InputSetPadFilter(fn)` installs one function
that `xbox_InputGetState` calls last, with the port, the host pad behind
it (`struct xbox_host_pad`: the backend's state before the keyboard and
script merge, `extra`, `connected`, and the backend slot) and the state
the title is about to get, which it may change. With none set nothing
changes. The enhancements layer sets `xbox_enhance_pad_filter` (`mod.h`)
for its host actions (`src/enhance/enhance_actions.c`, `[input.actions]`
in `docs/runtime/config.md`): it fires actions from keys, host pad
sources the map leaves to the host and chords of sources (`raw` in
`struct xbox_host_pad`), and only takes presses out of the state (a
consumed chord, `guide+a`). It stays disarmed, returning at its first
test, unless a feature key arms it (`[ENHANCE] actions hook=off` in a
golden run). A title that reads the pad through its own glue rather than
`xbox_InputGetState` calls `xbox_enhance_pad_filter` itself after its
merge (cat's `pad_input.c`). When the title stops polling, the flip reads
the host pad through `xbox_input_backend_read_host` so the actions still
fire on a loading screen. `tests/input_actions` covers the filter seam
and the actions.

**The capture record.** `xbox_InputSetPadSeen(fn)` installs the
enhancements layer's record of port 0 (`xbox_enhance_pad_seen`, `mod.h`),
which `xbox_InputGetState` calls under `XBOXRECOMP_ENHANCE` after the
filter, with the state the title is about to get (zeros and
`connected` 0 for a failed poll), whether or not the filter is armed. It
returns nonzero while a mod captures port 0: the title then reads the
pad connected and released, with `dwPacketNumber` moved each time that
starts or stops, and a failed poll is left as it is. The pointer keeps
`xbox_input` from linking against the enhance library. A title that
reads the pad through its own glue calls `xbox_enhance_pad_seen` itself
after `xbox_enhance_pad_filter` (cat's `pad_input.c`,
`XBOX_MOD_PAD_SEEN`). With no mod capturing it costs one load.

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

The map is resolved by the mapping rules below (`input_map.c`), as the
bindings `xbox_InputKeyboardBindings` builds from it, so a named binding and
the map read the same table. The pad is the same as before the map used them:
`tests/input_keyboard_map` compares it, byte for byte, with a frozen copy of
v0.14.0's resolver.

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
  (`[KEY] down enter`), the first 200 and then one in 50.

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

As binding text (`xbox_InputMap_BindingsFormat`), the same map is
`dpad_up = up`, `start = enter`, `back = backspace`, `left_thumb = shift`,
`right_thumb = ctrl`, `a = Z` ... `white = Q`, `left_trigger = 1`,
`left_stick_up = W, Numpad8` and so on: a port that lets the player rebind
starts from that.

### Keyboard and mouse

The keyboard and the mouse are one source on port 0 (`xbox_InputKbmPoll`,
the backend hook), switched by `RECOMP_KEYBOARD` and `RECOMP_MOUSE` or by a
title's `[input]` table (`src/enhance/enhance_input.c`, documented in
[`docs/runtime/config.md`](../../docs/runtime/config.md#input)). It composes
the pad from the runtime bindings: a control (`a`, `start`,
`left_stick_up`, ...) holds up to eight inputs, and any of them held drives it.

- **Vocabulary**: upstream's key names (`input_map.c`, "Key names" under
  the mapping rules below), case-insensitive, aliases included, mapped onto
  the fork's codes by `input_names.c`. The fork adds three inputs upstream
  has no virtual-key code for, named in its style: `numpadenter`, `wheelup`
  and `wheeldown`. `shift`, `ctrl` and `alt` hear either side; keypad Enter
  also holds `enter`, as Win32 reports it. `LGui`/`RGui` (Cmd, the Windows
  key) are the system's and have no name. The fork's older names (`KP8`,
  `KPEnter`, `LBracket`, `MouseLeft`, ...) were dropped with no alias.
- **Controls**: upstream's `InputControl` names, in its order: `dpad_up`,
  `dpad_down`, `dpad_left`, `dpad_right`, `start`, `back`, `left_thumb`,
  `right_thumb`, `a`, `b`, `x`, `y`, `black`, `white`, `left_trigger`,
  `right_trigger`, `left_stick_up`, `left_stick_down`, `left_stick_left`,
  `left_stick_right`, `right_stick_up`, `right_stick_down`,
  `right_stick_left`, `right_stick_right`.
- **Stick keys**: with `stick_ramp_ms` above 0 a stick key starts at 0.41 of
  full deflection (just past a 0.4 deadzone) and reaches full over that
  time; two perpendicular keys give 1/√2 each (the left pair stays full
  under `input.pad_square_left`, as the squared pad does, except while
  walking), and the walk key caps the magnitude at `walk_scale`. With 0, every key is full deflection as the
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
frees its port, and a new one takes the port the same model (gamepad GUID)
last held when that is free and another pad is still connected, else the
lowest free port, so a pad that connects while no other pad is connected
takes port 0. Device events and a once-a-second rescan both feed this, run
by the thread that pumps SDL: the window's loop (`xbox_InputSdlPadSync`), or
the poll when headless, so one thread owns every open and close; each change logs `[INPUT] host pad added
port=N "name"` or `[INPUT] host pad removed port=N`. With a window, the
first poll waits up to 250 ms for the window's loop to open the pads, so a
pad connected before launch is connected at the first read.
`tests/input_hotplug_sdl` checks it with SDL's virtual joysticks.
`xbox_InputSetState` reaches only connected ports; `xbox_InputShutdown`
(also registered with `atexit` by `xbox_InputInit`) stops every motor.
On SDL hosts a rumble gate (`xbox_RumbleGate*` in `pad_map.h`) passes a
change of level to the pad at most every 100 ms, the latest one winning,
and a stop at once, so the shutdown stop lands with no loop after it.
Levels compare by their high byte, the part SDL's HIDAPI drivers send (the
8BitDo one sends only that), so a change SDL would write as the same report
is no change. A held level is renewed every 500 ms while the title keeps
asking; a title that stops asking without a stop stops rumbling within
SDL's 1 s duration. Titles ask far more often (BLiNX sends about 21 output
reports a second), and each change is an output report that SDL's HIDAPI
drivers write while holding the lock the pad's input update needs. An
8BitDo pad over Bluetooth on macOS was seen to lose its input after a
rumble; that this is the cause is not confirmed, and fewer writes lower the
exposure without curing a write that blocks. A held-back change lands from
the window's loop, or from the next poll when headless. `tests/pad_map`
checks the gate. `tests/input_hotplug_sdl` checks its rate, the stop and
the shutdown through a virtual pad, which has no HIDAPI rumble thread, so it
does not reproduce the starvation. Native Linux SDL builds take the gate
too, which costs a change at most 100 ms. The XInput backend (native Windows, and
Proton, where Wine's XInput sits on winebus) is not gated: `XInputSetState`
goes to the XInput driver and shares no lock with the input reads in the
game's process, and the macOS synchronous write the gate guards against
is not on that path.

## A pad source behind the emulated USB pad

A title that links its own XAPI reads the pad through the emulated OHCI
controller (`src/usb/ohci.c`), not `xbox_InputGetState`, so a game has no
call to override there. `usb_gamepad_set_source(&src)` (`src/usb/usb_gamepad.h`)
gives it one place to stand instead. Install it once, before `xbox_OhciInit`;
with none installed, host pad N drives pad N on a fixed set of ports, as before.

```c
typedef struct {
    int      (*get_state)(int pad, struct XBOX_GAMEPAD *out);  // 0: not driven
    uint32_t (*present)(void);                // bit N: pad N should be plugged
    void     (*rumble)(int pad, uint16_t left, uint16_t right);  // NULL: host pad
} UsbPadSource;
```

- `get_state` is called for every input report, and `present` once per
  controller pass (about 4 ms). Both run on the controller thread, not the
  title's, and must not call back into the USB model.
- `present` decides what is on the bus. A pad joins when the source names
  it, one at a time, about 200 ms after the last one configured. It leaves
  after it has been missing for about 250 ms, so a source that drops a pad
  for a poll or two causes no pull. A pad that leaves enumerates from
  address 0 when it comes back, and only after the bus settles, so the
  driver sees the disconnect first.
- A transfer to an address no pad holds completes with DEVICE NOT RESPONDING
  while a source is installed. That is how XAPI learns that a pad is gone.
- `RECOMP_PAD_PRESS`, `RECOMP_PAD_SCRIPT` and `RECOMP_PAD_LIVE` still press
  on top of `get_state`, and the pads they name stay plugged in.
  `RECOMP_USB_PADS` keeps that many plugged even without host pads.
- The enhancements layer's `xbox_InputLastState` answers from what
  `xbox_InputGetState` returned, and a source bypasses that call. A source
  therefore passes port 0's state on with `xbox_InputKeepLastState(0, rc, &st)`.
  Without the enhancements layer this is a no-op.

`tests/usb_pad_source` covers the report, rumble, the plug and unplug
timing, the port's change bits and the no-device answer, all without a title.
BLiNX 1's `src/pad_input.c` is a source.

A game that answers the title's `XInputGetState` itself never reaches the
report, so `usb_gamepad.h` also has a tap for it:
`usb_gamepad_game_press(pad, &g)` presses the script's, the live file's and
the synthetic press's buttons on top of the game's state,
`usb_gamepad_game_record(pad, &g)` writes the state the title gets to
`RECOMP_DEBUG=pad_record` (on every poll, a disconnected port's zeros too,
under the flip that poll's press used), and `usb_gamepad_game_pads()` names the pads
those inputs drive. They use the walker's flip count and the same step
format, so a record replays with `RECOMP_PAD_SCRIPT=@file` through either
path. BLiNX 2's `src/pad_input.c` uses it; `tests/usb_pad_game` covers it.

A flip-timed step can miss when a replay runs slower or faster than the
record: a press that waited on a game event (a movie that takes A only some
seconds in) lands before the event. An anchor fixes the step to the event:
`f<flip>:@open:<text>[:<buttons>/<period>]` says that in the record a file
whose name starts with `<text>` (any case; a text holding `\` or `/` may
match anywhere in the guest path) opened at that flip. Steps that start at
or after it count their flips from the replay's open of that file and press
nothing before it; while the open is late, the replay taps the retry
`<buttons>` for half of every `<period>` flips (2 to 300), eight times, then
only waits. Anchors match in order, and the kernel's `NtCreateFile` reports
each successful open through `usb_gamepad_file_open`. An open that matches
a later anchor gives up the ones between and is taken: one on the armed
anchor's record flip at any time, one up to 1800 record flips on once the
armed anchor is late. A re-open of the last matched file is not a later
anchor while its clock is nearer the matched one than the later (before the
midpoint of their record flips); inside one record flip it always is. An
open 1800 flips late is given
up, along with later anchors that clock has passed: the replay goes on on
the clock it was due by, and steps already past on it are skipped. Each
give-up logs how many steps it skipped. Live lines (`RECOMP_PAD_LIVE`) take
no anchors.
`RECOMP_DEBUG=pad_record=<file>,pad_anchor=<text>|...` writes an anchor for
each open whose path holds one of the texts. It writes a retry only for a
tap: the last three presses before the open press the same buttons at an
even spacing, and the open comes within one spacing of the last. A `# retry`
comment line before the anchor names those presses, so a wrong guess can be
deleted; three same-button presses that miss the rule, the last within
three spacings of the open, get a `# no retry` line, so one can be added
by hand. An open in the same flip as a press is written a flip later, so
that press stays before it; the steps under that anchor then replay one
flip early against the open.
`tests/pad_script` covers the grammar and the clock, `tests/usb_pad_anchor`
the tap with opens.

A flip-timed record is not exact. A title's pad polls do not keep step with
the walker's flips: two polls can share a flip, and the next flip then gets
none. The record keeps one of the two polls' sticks, and the replay gives
both polls that value, so a stick that moves on every poll replays a poll
off. The record also keeps a stick within 256 of the step's first value, and
an analog button only as pressed or not. That is enough to navigate menus,
but not for a frame-exact move (BLiNX 2's 2-1 wall clip missed).
`RECOMP_DEBUG=pad_clock=poll` records on the title's port-0 polls instead:
`g<poll>:<buttons>:<hold polls>`, every stick change a new step, and an
analog button's depth as `lt=<1..255>` (any analog button takes `=<v>` in a
script; 255 is a full press). Anchors on that clock are `g<poll>:@open:...`.
A script uses one clock: an entry on the other one is refused with a log
line. The poll clock counts the polls that pass through
`usb_gamepad_report` or `usb_gamepad_game_record` for port 0, so a replay
gives poll N what the record's poll N got, however the flips fall. On the
OHCI path (a title that links its own XAPI) `usb_gamepad_report` answers the
model's USB interrupt transfers, so the clock counts those reports, which
come at the bus's pace, not the title's reads of the pad: there a poll step
is only as exact as the reports are steady against the title's frame. A
title that answers XInputGetState itself (BLiNX 2) counts its own reads.
An anchor's retry period and its 1800-count give-up are polls on this clock.

A record ends with `g<poll>:@end` (`f<flip>:@end` on the flip clock), the
count at which it stopped. The step presses nothing.

`RECOMP_DEBUG=pad_quit` ends a scripted run once every step of
`RECOMP_PAD_SCRIPT` has ended, the `@end` included, so a replay runs through
the idle time after the record's last release and stops where the record
stopped (a record without `@end` stops at its last release). The quit is
posted to the window, which closes as its close button closes it, on its
own thread: the check runs on the title's pad thread, under the game's pad
lock, where exit hooks and atexit work must not run. Without a window (a
headless run) a thread of its own runs the exit hooks, then
`ExitProcess(0)` (`exit(0)` on POSIX). It needs steps from
`RECOMP_PAD_SCRIPT`; live lines added meanwhile must end too.
`tests/usb_pad_poll` covers the poll clock end to end, a flip-clock control
that misses, `@end` and both quits.

## Ports

```c
#define XBOX_MAX_CONTROLLERS  4   // Ports 0-3
```

The Xbox supports 4 controllers. Each port can have a controller with optional memory units and other accessories. This layer only handles the gamepad; memory unit emulation is not needed for recompiled games.

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
  only by a port that calls it. This fork's backends fill an `InputRaw`
  from the host pad (`xbox_PadRawFromGamepad`, the table above) and
  `input_core.c` maps it through the pad setup (`xbox_InputSetPadSetup`,
  `pad_map.h`): a `PadMap`, an alternate map merged in with `PadMerge`,
  and Y inversion. With no `[input.pad]` that setup is
  `xbox_PadSetupIdentity`: `PadMapDefaults`, then White on LB, Black on
  RB, deadzones and the trigger threshold 0, which `tests/pad_map` checks
  gives what the old fixed table gave. `xbox_InputHostRaw(slot, &raw)` is
  the raw pad before the map, which the host actions read.
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
