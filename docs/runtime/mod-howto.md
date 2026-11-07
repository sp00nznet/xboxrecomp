# Write a mod

This walks through `templates/mod/demo_mod.c`, a mod that counts frames and shows
them on a status panel. It needs no game address, so it builds into any game.
[The reference](mods.md) has the contract and every call.
`tests/mod_demo` builds the same file on every toolkit test run, so this page
stays code that works.

## Check it should be a mod

A feature that needs no game address goes in the toolkit, for every game. A stock
game's crash fix goes in the game's `src/fixes`.
[What goes where](mods.md#what-goes-where).

## Add it to the game

Copy `templates/mod/demo_mod.{c,h}` to the game's `src/mods/`, add the `.c` to the
game's sources, and list it in `main.c`:

```c
#include "mods/demo_mod.h"

/* Before xbox_enhance_init. A NULL ends the table, so add only what's built. */
static const struct xbox_mod *mods[2];
int nm = 0;

if (demo_mod())
    mods[nm++] = demo_mod();
xbox_mod_register_table(mods);
xbox_enhance_init(".", NULL);
```

`demo_mod()` returns NULL in a build with no enhancements layer. Guard the
table the same way as the mod, since `mod.h` isn't there either.

The game must also call the frame entry once per guest frame. If it has no mods
yet, wrap a function the game runs once a frame, usually the D3D swap, in its
`recomp_manual.c`:

```c
/* D3DDevice_Swap: every draw of a frame comes before it. */
#if defined(RECOMP_ENV_HAVE_ENHANCE_KEYS) && defined(__has_include)
#  if __has_include("mod.h")
#    include "mod.h"
#    define GAME_MOD_FRAME 1
#  endif
#endif

extern void sub_00123450_gen(void);

void sub_00123450(void)
{
    sub_00123450_gen();
#ifdef GAME_MOD_FRAME
    xbox_mod_frame();
#endif
}
```

`xbr recomp` sees the `_gen` name and emits the game's body under it. It reads
those two lines as text, so keep them as they are here, with no comment on the
`void sub_...` line. The guard keeps the file building with no enhancements layer.

To see the mod, build with `-DXBOXRECOMP_ENHANCE=ON` and add to
`config/config.toml`:

```toml
[mods]
"demo.enable" = true
```

The log says `[MOD demo] on` and `[ENHANCE] mods=demo`, and the panel appears at
the top left.

## The minimal mod

The guard, an id, an `init`, and an accessor. The rest of this page adds to it.

```c
static const struct xbox_mod_api *s_api;

static int init(const struct xbox_mod_api *api)
{
    s_api = api;
    api->log(&s_mod, "hello");
    return 0;                   /* non-zero leaves the mod out */
}

static const struct xbox_mod s_mod = { "demo", init, NULL, &k_desc, 0, NULL };

const void *demo_mod(void) { return &s_mod; }
```

Below the version in the guard, the accessor returns NULL. See the end of the
template for the `#else`.

The template declares `static const struct xbox_mod s_mod;` above `init`, so `init`
can name it.

## Settings

Declare them as data. The menu builds the page from this and reads the values
before the mod runs.

```c
enum { SET_ENABLE, SET_INTERVAL, SET_N };

static const struct xbox_mod_setting k_set[SET_N] = {
    /* name, key, env, aliases, label, help, unit, kind, live, flags,
     * def, lo, hi, step, choices */
    [SET_ENABLE] = { "enable", NULL, NULL, NULL, "Frame counter", "...",
      NULL, XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 1, 1, NULL },
    [SET_INTERVAL] = { "interval", NULL, NULL, NULL, "Refresh every", "...",
      "frames", XBOX_SET_INT, ENHANCE_LIVE, 0, 30, 1, 600, 1, NULL },
};
```

Read them in `init`, and take changes from `on_setting`:

```c
s_interval = (uint32_t)api->setting(&k_desc, SET_INTERVAL);
s_on = api->setting(&k_desc, SET_ENABLE) != 0;
api->on_enable(on_enable, NULL);
api->on_setting(&k_desc, on_setting, NULL);
```

Three rules cover most mistakes:

- **The enable setting's default is off.** A stock run must print `mods=none`.
- **Gate your own callbacks.** `on_frame` keeps being called while the mod is off,
  so test `s_on` first. Hooks do stop.
- **`on_enable` may be told the state it's in.** Make it idempotent.

The enable setting makes the mod inited *dormant* when the menu is on, so the menu
can switch it without a restart. That needs `on_enable`. Without it, switching
applies at the next start, with one log line.

## Controls

A control is a binding the player can change in the menu. Declare the defaults;
the toolkit registers the action before `init`.

```c
static const char *const k_reset_def[] = { "F8", NULL };

static const struct xbox_mod_control k_ctl[CTL_N] = {
    /* id, label, help, when, kind, defaults, aliases, env */
    [CTL_RESET] = { "reset", "Reset the count", NULL, NULL, XBOX_CTL_ACTION,
                    k_reset_def, NULL, NULL },
};

/* In init. */
s_reset_id = api->control_action(&k_desc, CTL_RESET);
api->actions_arm("mods.demo.enable");

/* In on_frame. */
if (api->action_pressed(s_reset_id))
    s_frames = 0;
```

`actions_arm` names the mod's enable key. An action fires, and a panel draws, only
while a feature key arms the actions tick. The menu arms it when it's on, so a mod
without this call works until someone runs with `RECOMP_MENU=0`.

A key the toolkit keeps for itself is refused with one log line, so read the log
the first time. `XBOX_CTL_BUTTON` and `XBOX_CTL_STICK` are for a mod that captures
the pad, with `input_capture` and `control_value`.

## Status panel

One card at the top left, for a readout or a key hint. The first line is the
title.

```c
snprintf(text, sizeof text, "Frames\n%u\n{ctl:reset} resets", s_frames);
if (strcmp(text, s_shown) != 0) {
    memcpy(s_shown, text, sizeof s_shown);
    s_api->status(&s_mod, text);
}
```

- `{ctl:reset}` is kept as written and expanded at each draw, so the panel follows
  a rebinding and the device in use. Never paste a key's name in the text.
- Set it when it changes. A panel is up to 6 lines of 120 bytes.
- The toolkit drops the panel when the mod goes off, so forget what you showed in
  `on_enable`, or the next panel never comes.
- Panels draw only while the actions tick is armed, as for controls above.
  [Details](config.md#glyphs-and-the-status-panel-v7).

## Reach the game

In order of preference.

### Read and write memory once a frame

```c
if (!s_on || !s_api->mem)
    return;
float speed = XBOX_MOD_MEMF(s_api, MY_PLAYER_VA + 0x154u);
```

`api->mem` is 0 during init. Nothing in the game's code changes, so no regeneration
is needed.

### Hook a function

Declare it in the game's `game.toml`, by address, or by signature so the slot
lands on the function in another release:

```toml
[mods]
hook_functions = ["0x00234560", "sig:hud_draw"]

[mods.signatures.hud_draw]
section = ".text"
pattern = "55 8B EC 83 E4 F8 ?? ?? ?? ?? 56"
```

Then `<game> analyze` and `<game> recomp`. `analyze` refuses a function the
disassembly doesn't start at, one inside another function, one whose `ret n` pops
differ, and one the game's manual file defines or wraps by address (`sub_X`,
`sub_X_gen`). It doesn't see the name forms (`XBR_FN`, `XBR_GEN`), so in a file
that uses them, check the function isn't wrapped there: the link would then see
two definitions of `sub_X`. It writes `config/hook_functions.json`; commit it. The
CLI's `docs/manifest.md` has all of it.

In the mod:

```c
static void on_call(struct xbox_mod_call *c, void *user)
{
    uint32_t first = XBOX_MOD_ARG32(s_api, c, 0);
    /* ... */
}

/* In init. A VA with no slot is one log line and a 0. */
if (!api->hook(0x00234560u, on_call, NULL, 0, NULL))
    return 1;
```

`api->hook` takes a VA, and a signature only places the slot. A mod for more than
one release also needs the function in `config/names.toml`, placed for each
release, and passes the name ([Addresses](#addresses)):

```c
if (!api->hook(XBR_VA(hud_draw), on_call, NULL, 0, NULL))
    return 1;
```

- A pre runs before the function, a post after. A post can change `c->eax`.
- `XBOX_MOD_HOOK_MAY_SKIP` lets a pre set `c->skip`, and the slot does the body's
  `ret n`.
- Several mods can hook one function. Pres run in table order, posts in reverse.
- The hook is silent while the mod is off, so off means the game's own code path.
- It runs on the calling thread: use atomics for what a frame callback reads.
- Declare it: `XBOX_MOD_NEED_HOOK` in `s_mod.needs`.

**Regeneration.** The list is part of the generation key, so editing it stales
`gen/` until the next `analyze` and `recomp`. That's the point: the slot is
regenerated with the function, and the mod's source doesn't change. Adding a slot
does change the generated code, so run the game's goldens.

### Call a game function

```c
uint32_t stack[1] = { arg }, ret;
struct xbox_guest_args a = { .stack = stack, .n_stack = 1 };
if (s_api->guest_call(0x00345670u, &a, &ret)) { /* ... */ }
```

Only from a hook or a frame callback, and only what the game itself calls from a
place like that. [The rules](config.md#guest-calls-v6).

### A wrapper, when nothing else fits

A wrapper in `recomp_manual.c` is plain C around the game's function, as in the
frame entry above. It's the escape hatch, for what a hook can't express. It has
costs:

- It isn't in the mod's file, and it runs while the mod is off, so it needs its own
  test of the mod's state.
- A function has one owner: it can't have a hook slot, and mods can't share it.

Keep the wrapper to a single call into the mod.

### Addresses

Put them in the game's `config/names.toml` when it has one, per release, and read
them with `XBR_VA(name)`. Guard what the mod can go without:

```c
#if XBR_HAVE(player_speed)
    float speed = XBOX_MOD_MEMF(s_api, XBR_VA(player_speed));
#endif
```

Only a plain `#if XBR_HAVE(name)`, one per line. Nest them for two names. See the
CLI's `docs/names.md`. These forms come from `gen/recomp_names.h`, so they need a
newer CLI and toolkit than some pins; [the reference](mods.md#reaching-the-game)
says which.

## Test it

**Pure logic.** Keep what can be computed without the game in a `<id>_logic.[ch]`
with no guest access, and give it a unit test. That's where most bugs are.

**The mod, standalone.** `tests/mod_demo` runs the mod through the real
`xbox_enhance_init` with no game: off and silent, on from a user file, dormant then
switched, a setting clamped and delivered, the panel's text, the control's
binding and its key, the dump, a hook through a slot, and the build with no API. `xbox_mod_status_text`,
`xbox_mod_setting_set`, `xbox_mod_control_bindings` and `xbox_mod_describe_write`
are there for tests like this one. Copy it for your mod.

**The dump.** It runs the descriptors with no mod init:

```sh
RECOMP_MODS_DESCRIBE=json RECOMP_MODS_DESCRIBE_OUT=mods.json ./game
```

**In the game.**

- With the mod off, the log prints `mods=none`, and the game's goldens pass
  unchanged.
- `RECOMP_DEBUG=mod_set_at=900:mods.demo.enable=on,1200:mods.demo.enable=off`
  toggles it at a frame, with nobody at the menu. It runs from the actions tick, so
  something must arm that. To turn on a mod that starts off, that's the menu, since
  the mod inits dormant only with the menu on.
- Toggle it on and off in the menu, and check it leaves nothing behind.

## Checklist

- [ ] Off by default, and `mods=none` with it off.
- [ ] A guard at the newest API member it uses, and a NULL accessor below it.
- [ ] Every value the player might change is a setting, and every key a control.
- [ ] `on_enable` is idempotent, and `on_frame` tests the mod's state first.
- [ ] Off restores what the mod changed in guest memory.
- [ ] No `recomp_env`, no direct calls into the toolkit.
- [ ] Addresses named, not scattered, with `needs` declared.
- [ ] No game data in the repo, no assets the player doesn't generate from their
  own copy.
