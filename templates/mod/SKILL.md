---
name: xbr-mod
description: Write or change a game mod on the xboxrecomp mod API (src/enhance/mod.h). Use when asked to add a mod, hook a game function, add a mod setting, control or status panel, or move game code in recomp_manual.c onto the API.
---

# Writing a mod on the xboxrecomp mod API

Read these from the game's toolkit first, at the toolkit the game pins (`game.toml`
`[toolkit]`), not at a newer one:

- `docs/runtime/mods.md`: the contract and the call table.
- `docs/runtime/mod-howto.md`: the walkthrough.
- `src/enhance/mod.h`: the truth, when the docs disagree.
- `templates/mod/demo_mod.c`: the file to copy. It builds in `tests/mod_demo`.

A pin older than these docs and the template has only `mod.h` and
`docs/runtime/config.md`. Read the rest from the integration branch, and let the
pin's `mod.h` decide what the mod can call.

## Decide first

1. Could it work with no game address and no game code? It's a toolkit feature,
   not a mod. Stop and say so.
2. Does it fix a crash in the stock game? It goes in `src/fixes`, on by default.
3. Otherwise it's a mod in the game's repo: `src/mods/<id>.c`, with pure logic in
   `src/mods/<id>_logic.{c,h}`, its table row in `main.c`.

## Rules

- Off by default. A stock run must print `[ENHANCE] mods=none`. Default-on only if
  the mod does nothing on the stock configuration, and then it calls
  `api->active(mod, 0)`.
- A guard to the newest member it uses (`XBOX_MOD_API_VERSION >= N`), and an accessor
  returning NULL below it.
- All config through the descriptor: an enable setting (`XBOX_SET_ENABLE`,
  `ENHANCE_LIVE`), every tunable a setting, every key a control. No `recomp_env`, no
  `getenv`, no `cfg_*`. No direct calls into `nv2a_*` or other toolkit internals: if
  the API lacks something, say so and stop, don't reach around it.
- `on_frame` tests the mod's own state first, since the toolkit keeps calling it
  while the mod is off. `on_enable` is idempotent and restores what the mod changed
  in guest memory.
- A mod with a control or a panel calls `api->actions_arm("mods.<id>.enable")` in
  init, or they work only while the menu is on.
- `api->mem` is 0 during init. Test it in `on_frame`.
- A hook runs on the caller's thread. What a frame callback reads from it is atomic.
- Keep to the short list of how to reach the game, in this order: frame + memory,
  hook slot, HUD table, `guest_call`. A wrapper in `recomp_manual.c` only when none
  fits, and say why in its comment.
- Never edit generated code. A new hook goes in `game.toml [mods] hook_functions`,
  then `<game> analyze`, then `<game> recomp`.
- Addresses go in `config/names.toml` when the game has it, or named `#define`s at
  the top of the mod otherwise, each with what it is and how it's known. `names.toml`
  needs a toolkit whose `recomp` takes `--names`, so check the pin first.
- A `sig:` entry places the slot in each release, but `api->hook` still takes a VA.
  For a second release, add a `names.toml` row and pass `XBR_VA(name)`.
- `analyze` doesn't see a wrap written with `XBR_FN`/`XBR_GEN`. Check the manual file
  before adding a hook on a function it might wrap.
- No game data: no copied assets, no dumped tables. Public text says "I".

## What you must not guess

Addresses, struct offsets and calling conventions come from the disassembly or from
a run, never from memory of the game. Look them up in the game's `symbols`/`names`
config or its analysis notes, and cite in a comment how you know. If you can't
confirm one, ask.

## Done means

1. The mod's logic has a unit test, and the mod runs in a `tests/mod_demo`-style
   ctest where it can be tested standalone.
2. The game builds with the mod off and on, and the log says `mods=none` off.
3. On and off in the running game leaves nothing behind. Use
   `RECOMP_DEBUG=mod_set_at=<frame>:mods.<id>.enable=on,...` when there is no one at
   the menu (it needs the actions tick armed; for a mod that starts off, the menu on).
4. A change that added a hook slot or a wrapper changes what the game executes, so
   the game's goldens run.

Report which of these ran, and what couldn't be.
