# Mods: reference

A mod is C code built into a game's executable. It reads and steers the guest
through one small API, `src/enhance/mod.h`. It exists only with
`XBOXRECOMP_ENHANCE=ON`.

This page is the map: what belongs in a mod, the contract, and every call with a
link to its full rules in [config.md](config.md#mods). For a first mod, read
[Write a mod](mod-howto.md).

## What goes where

| It is | It goes |
| --- | --- |
| A feature that needs no game address or game code (scaling, filters, shaders, texture packs, pacing, overlay) | The toolkit, on for every game with no per-game code. Not a mod. |
| A feature that needs game addresses or game code | A mod in the game's repo, over this API. Off by default. |
| A fix for a crash in the stock game | The game's `src/fixes`, on by default. Not a mod. |
| Part of a mod that any game could use | The toolkit, called through this API. The mod keeps the game's addresses. |

I keep game facts out of the toolkit: every address lives in the game's repo,
and every toolkit layer (the HUD table, for one) takes them as data.

## Contract

- **Source contract, not a plugin ABI.** A mod is linked into the executable and
  built against the toolkit its game pins. There is no loader and nothing to
  stay compatible with across builds.
- **`XBOX_MOD_API_VERSION`** rises when `struct xbox_mod_api` gains members.
  Members are only added, never moved or changed in meaning.
- **`XBOX_MOD_DESC_VERSION`** rises when the descriptor gains a field or a kind.
  The toolkit reads only what `desc.size` covers.
- **A mod guards to the newest member it uses.** It compiles to a NULL accessor
  below that, so a game on an older toolkit still builds and runs without it:

  ```c
  #if defined(RECOMP_ENV_HAVE_ENHANCE_KEYS) && defined(__has_include)
  #  if __has_include("mod.h")
  #    include "mod.h"
  #    if XBOX_MOD_API_VERSION >= 7   /* the newest thing this mod calls */
  #      define MY_MOD 1
  #    endif
  #  endif
  #endif
  ```

- **Stock stays stock.** With every mod off a run prints `[ENHANCE] mods=none`,
  and the goldens depend on it. A mod defaults off, unless it does nothing on the
  stock configuration (a widescreen HUD mod at 4:3).
- **A mod only talks to the toolkit through `api`.** Its settings go through the
  descriptor, not `recomp_env` or `api->cfg_*`.
- **The descriptor is data.** The menu and the dump read it without running the
  mod's code, so a mod off or never inited still shows on the Mods page.

## What the game gives a mod

| The game does | So that | Where |
| --- | --- | --- |
| `xbox_mod_register_table(table)` before `xbox_enhance_init` | The toolkit knows the mods. The table is NULL-terminated and fixes their order. | `main.c` |
| `xbox_mod_frame()` once per guest frame, on the guest's thread | `on_frame`, `on_enable`, `on_setting` and the HUD layer run. Nothing else drives them. | A wrapper around the game's per-frame function, in `recomp_manual.c` |
| In its own pad glue, after its merge: `xbox_enhance_pad_filter`, `xbox_enhance_pad_seen` and `xbox_mod_input_captured()`. A game on `xbox_InputGetState` gets all of this from it. | Actions see the host pad, consumed chords leave the state, and `input_capture` and `control_value` work. | The pad glue |
| `game.toml [mods] hook_functions` | `api->hook` has slots. | `game.toml`, then `analyze` and `recomp` |

A game with none of these still runs. It prints `mods=none`.

## The mod

```c
struct xbox_mod {
    const char *name;                          /* the id: the keys, the log, the Mods page */
    int (*init)(const struct xbox_mod_api *);  /* non-zero: left out, its registrations dropped */
    void (*shutdown)(void);                    /* may be NULL */
    const struct xbox_mod_desc *desc;          /* what it is and reads; NULL for a v2 mod */
    uint32_t needs;                            /* XBOX_MOD_NEED_*; a declaration for now */
    const char *follows;                       /* a key that turns it on; the mods= line only */
};
```

**When it inits.** `mods."<id>"` is true (the default) and either it has no enable
setting or that setting is on. With the setting off, `menu.enabled` on and the
setting's live class `ENHANCE_LIVE`, it inits *dormant*: its hooks stay silent
until the menu turns it on. Otherwise it does not init.
[Full rule](config.md#the-descriptor-v3).

**Threads.** `on_frame`, `on_enable` and `on_setting` run on the guest's frame
thread. Hooks run on whichever guest thread made the call. `batch_filter` runs on
the render thread. `setting`, `status`, `log` and `active` are safe from any
thread.

## The API

`v` is the API version that added it. All of it is in
[config.md](config.md#mods); the links go to the long rules.

| Member | v | When | What |
| --- | --- | --- | --- |
| `mem`, `XBOX_MOD_MEM32`, `XBOX_MOD_MEMF` | 1 | From the first frame or hook | Guest memory. 0 during init. |
| `cfg_*`, `cfg_bind_env` | 1 | Any | Config getters. A mod with a descriptor uses `setting` instead. |
| `log` | 1 | Any | One `[MOD <name>]` line. |
| `on_frame` | 1 | Init | Called from `xbox_mod_frame`. Up to 16. Keeps being called while the mod is off. |
| `input_state`, `input_capture` | 1 | Frame | The title's last port 0 read; take port 0. |
| `batch_filter` | 1 | Init | Skip a draw, by program hash and state. Up to 8. |
| `action_register`, `action_pressed`, `action_down`, `actions_arm` | 2 | Init, then frame | Host actions. A descriptor control registers its own. `actions_arm(enable key)` keeps them and the panel working with the menu off. |
| `menu_add` | 2 | Init | An extra item on the mod's list. Up to 32. Static. |
| `on_enable` | 2 | Init | The Mods page's toggle, at the next frame. Must be idempotent. |
| `toast` | 2 | Any | A short message on screen. |
| `setting`, `on_setting` | 3 | Any, init | A descriptor setting's value, and its changes. |
| `control_action` | 3 | Init, frame | A control's action id. The toolkit registers it before `init`. |
| `control_value` | 3 | Frame | A control's value while captured. |
| [`hook`](config.md#hooks-v4) | 4 | Init | Callbacks around a guest function with a slot. |
| `active` | 4 | Any | "Doing something now", for the `mods=` line. |
| [`hud_table`, `hud_apply`, `hud_restore`, `hud_capturing`](config.md#hud-table-v5) | 5 | Init, hook | The game's HUD table, and moving its elements on wide displays. |
| [`guest_call`, `guest_alloc`](config.md#guest-calls-v6) | 6 | Hook, frame | Run a lifted function; a block of guest memory. |
| [`glyph_name`, `status`](config.md#glyphs-and-the-status-panel-v7) | 7 | Any | Name a binding; the mod's status panel. |
| [`draw3d_*`](config.md#3d-debug-draw-v8) | 8 | Frame | World-space triangles and lines over the frame. |

`mod.h` heads each version's members "check size before using these", and
`config.md` asks it of `glyph_name` and `status`. At or below a mod's guard
version, the guard settles it, because the game's pin fixes the toolkit, so the
check is optional there; `templates/mod/demo_mod.c` shows the form on `status`. A
member above the guard always needs one.

## The descriptor

```c
struct xbox_mod_desc {
    uint32_t version, size;                     /* XBOX_MOD_DESC_VERSION, sizeof */
    const char *title, *summary, *author, *version_text;
    const struct xbox_mod_setting *settings;  uint32_t n_settings;
    const struct xbox_mod_control *controls;  uint32_t n_controls;
};
```

**Settings.** `{ name, key, env, aliases, label, help, unit, kind, live, flags,
def, lo, hi, step, choices }`.

- `name` is the local name. The config key is `mods."<id>.<name>"`, flat under
  `[mods]`, unless `key` names a legacy one.
- `kind`: `XBOX_SET_BOOL`, `XBOX_SET_INT`, `XBOX_SET_FLOAT`, `XBOX_SET_ENUM`, and
  `XBOX_SET_LINK` for a toolkit item shown on the mod's page (descriptor 2).
- `live`: `ENHANCE_LIVE` reaches the mod through `on_setting`; a restart class
  only records the value.
- `flags`: `XBOX_SET_ENABLE` makes it the mod's Enabled switch. One per mod.
- Order: `env` > user file key > user file aliases > defaults file key > defaults
  aliases > `def`. Clamped to `lo..hi` and snapped to `step`, with one log line
  per change.

**Controls.** `{ id, label, help, when, kind, defaults, aliases, env }`.

- `kind`: `XBOX_CTL_ACTION` (a host action: `control_action`), `XBOX_CTL_BUTTON`
  and `XBOX_CTL_STICK` (read with `control_value` while captured), `XBOX_CTL_INFO`
  (a line of help on the page).
- `defaults`: a NULL-terminated list of bindings. A key the toolkit keeps for
  itself is refused with one log line (`F10` opens the menu, `F9` releases the
  mouse).

A bad descriptor refuses the mod with one line, and it never inits.
[Rules](config.md#the-descriptor-v3).

## Reaching the game

| Need | Use | Costs |
| --- | --- | --- |
| Read or write a value once a frame | `on_frame` + `XBOX_MOD_MEM32` | Nothing in the game's code |
| Run around a function, read its arguments, change its return, skip it | `hook` + a `[mods] hook_functions` entry | Regenerate; one load and a branch per call |
| Move HUD elements on wide displays | `hud_table` + a [HUD table](config.md#hud-table-v5) | Hook slots for the table's wrappers |
| Call a game function | `guest_call` | Only from a hook or a frame |
| Something a hook can't express: a frame's start, a read of the caller's frame | A wrapper in the game's `recomp_manual.c` | That function has one owner: no hook slot on it, and a mod off still goes through it |

Prefer the top of the table. [Write a mod](mod-howto.md#reach-the-game) has
how to do each.

An address belongs to one release. A game with `[pipeline] names` keeps them in
`config/names.toml` (per release, read through `XBR_VA` and friends), and a mod
that can go without one guards it with `#if XBR_HAVE(name)`. See the CLI's
`docs/names.md`.

This needs CLI v0.21.0 or newer and a toolkit whose `recomp` takes `--names`. The
public toolkit tags up to `blinx2-v0.14.0` don't, and `recomp` stops and says so.
On those pins a mod uses plain addresses, in named `#define`s.

## Config and tools

| | |
| --- | --- |
| `mods."<id>" = false` | Leaves a mod out at start. |
| `mods."<id>.<name>"` | A setting. |
| `RECOMP_MODS_DESCRIBE=json` | Writes every descriptor [as JSON](config.md#the-descriptor-dump) and exits. No mod inits. |
| `RECOMP_DEBUG=mod_set_at=<frame>:<key>=<value>,...` | Changes a setting at a frame, to test without the menu. Needs the actions tick armed, by the menu or by a key on. |
| `[ENHANCE] mods=` | [What runs now.](config.md#the-mods-line) |
