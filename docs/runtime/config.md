# Configuration (`config.toml`, `config.default.toml`)

The enhancements layer (`src/enhance/`) is opt-in. It is built only with
`-DXBOXRECOMP_ENHANCE=ON` (off by default), and a title that turns it on calls
it. This page describes its configuration: the two files and how they layer,
where they are read from, how the environment overrides them, the format and
the key names.

## Two files and the environment

A title calls `xbox_enhance_init(root, title)` (`src/enhance/enhance.h`) once
at startup, after `recomp_env_init` and before the guest starts. `root` is
the user data root, the directory that holds the player's files; a title
passes `"."`, the working directory, which a bundle's launcher sets to the
bundle. A title with one executable passes `title = NULL`.

There are two kinds of file:

- **`config.default.toml`**, the defaults that ship with the game. It sits
  beside the executable, `recomp_exe_dir()` (`src/platform/recomp_exe_dir.h`:
  `_NSGetExecutablePath` on macOS, `GetModuleFileNameA` on Windows,
  `/proc/self/exe` on Linux, the working directory if none works); on macOS
  a `.app` keeps it in `Contents/Resources`, which the layer looks in when
  the executable's directory has none. It is read, never copied, so a new
  version's keys and values reach every player. Nobody edits it: an update
  replaces it.
- **`config.toml`**, the player's file. It holds only the keys the player
  changed. An update never writes it.

For a key, the first of these that has a value wins:

| Tier | Source | Log source |
|---|---|---|
| 1 | the environment variable bound to the key, for example `RECOMP_RENDER_SCALE` for `render.scale` (see [Environment](#environment)) | `env` |
| 2 | `<root>/<title>/config.toml`, the per-title file | `file` |
| 3 | `<root>/config/config.toml`, the file shared by all titles | `file` |
| 4 | `config.default.toml` beside the executable | `defaults` |
| 5 | the game's code default for the key (`enhance_cfg_set_code_default`), else the default that the module passes in | `default` |

A game sets a code default with `enhance_cfg_set_code_default(key, value)`
(`enhance_cfg.h`) before `xbox_enhance_init`, for a default that differs
from the toolkit's, for example one per build:

```c
#ifdef _WIN32
    enhance_cfg_set_code_default("present.pacing", "spin");
#endif
```

It is spelt as the file would spell it. It answers whenever tier 5 does,
including for an env or file value that does not convert, and it is the
stock value of the in-game menu, so its Save, row reset and Reset to
defaults compare against it.

`RECOMP_CONFIG=<path>` replaces tiers 2 and 3 with that one file; the
defaults file is still read under it. `RECOMP_CONFIG=none` reads no file at
all, the defaults file included, so only the environment and the code
defaults apply: a golden run pins it, so a shipped default never changes a
golden. `RECOMP_ENHANCE_CONFIG`, the variable's earlier name, is still read,
with one `[ENV] RECOMP_ENHANCE_CONFIG is deprecated; use RECOMP_CONFIG` line.

When the player's file is absent and its directory exists, the layer writes
it as a short comment header that says what goes there (the per-title file
when the title's directory exists, else the shared one; a `RECOMP_CONFIG`
file whose directory exists gets one too). A `RECOMP_CONFIG` file whose
directory is missing is an error. Under `none` nothing is written.

### Migration from `enhance.toml`

An earlier version named the player's file `enhance.toml`, with the same
keys. When a location the layer reads as `config.toml` is absent and an
`enhance.toml` is beside it, the layer renames it once and says so:

```
[ENHANCE] config: renamed ./config/enhance.toml to config.toml (one-time migration)
```

Only a file named `config.toml` is a candidate (so a `RECOMP_CONFIG` file
with another name never is), `<root>/enhance.toml` itself is not, and
nothing is renamed under `none`. A rename that fails is reported once; the old file is left alone and
not read, and the next run tries again.

### Errors and the log

If a file does not parse, the layer reports it with its line number and
ignores the whole file; the other files still apply. A typo therefore never
applies half a file:

```
[ENHANCE] config error: ./config/config.toml:3: duplicate key 'render.scale' (first set on line 2) (file ignored)
```

At startup the layer logs one line that names the files, lowest layer first,
each with its own key count, and one line for each environment override:

```
[ENHANCE] config: /opt/game/config.default.toml (6 keys), ./config/config.toml (1 key)
[ENHANCE] render.scale = 2 from RECOMP_RENDER_SCALE
```

With `RECOMP_CONFIG=none` the line is `[ENHANCE] config: none (RECOMP_CONFIG)`,
and with no file found it is `[ENHANCE] config: none`.

When a value has the wrong type (`scale = "2"`), the layer reports it once,
names the file and line, and uses the next tier. The layer can also list keys
that nothing read (`enhance_cfg_report_unused`), in either file, so that a
misspelled key such as `scael = 2` is visible instead of being ignored
silently.

## File format: a subset of TOML

Every file that this reader accepts is valid [TOML](https://toml.io) 1.0. The file must be UTF-8.

```toml
# Comments start with '#' and run to the end of the line.

[render]                # a table; dotted names work too: [input.bindings]
scale = 2               # integer: 42, -7, 1_000, 0xFF, 0o17, 0b101
sharpness = 0.5         # float: 0.5, 1e3, -2.5E-2, inf, nan
filter = "linear"       # string: "..." with escapes \t \n \" \\ \uXXXX
shader_dir = 'C:\mods'  # literal string: '...' with no escapes
vsync = true            # bool: true or false (lower case)
sizes = [1, 2, 3]       # array of scalars; it can span lines,
modes = [               # hold comments, mix types and end with a comma
  "a",   # first
  "b",
]

[input]
bindings.a = "Space"    # dotted key: the same as [input.bindings] a = "Space"
```

The reader rejects the following. Each is reported as an error with a line
number:

- multi-line strings (`"""` and `'''`)
- inline tables (`{ a = 1 }`)
- arrays of tables (`[[x]]`)
- nested arrays
- dates and times
- unquoted strings (`filter = linear`; the error message says that strings
  need quotes)
- duplicate keys, a table defined twice, and a key that is used both as a
  value and as a table
- reopening a table that a dotted key defined (`a.b = 1`, then `[a]`), and
  using a dotted key to add to a table that a header defined
- `_` anywhere except between two digits (`1_000` and `0xFF_FF` are fine;
  `0x_FF` and `1_e5` are not), and a second base prefix (`0x0x1F`)
- the `\e` escape (TOML 1.1 only)
- invalid UTF-8, and control characters (DEL included, tab excepted) in
  strings and comments

Floats are parsed the same way in every locale: a process locale with a comma
decimal point does not change how `0.5` is read, either in the file or in the
environment.

Keys are bare (`A-Z a-z 0-9 _ -`) or quoted, with no `.` inside a quoted part.
The one exception is `[input.actions]` (below), whose keys are action names.
A lookup uses the full dotted name, such as `render.scale`. A UTF-8 BOM and
CRLF line endings are accepted.

## Keys

The current keys are listed below. Every key defaults to the stock behaviour
except `present.pacing`, which defaults to `"sleep"` (the frames and pace
match the console's spin, and a host core is freed), so an empty file (or no
file) runs the game as it was apart from that wait. Golden runs always use
the stock settings, `present.pacing = "spin"` included.

| Key | Type | Values | Default | Env override |
|---|---|---|---|---|
| `render.scale` | int | internal resolution factor: 1, 2, 3, ... | `1` | `RECOMP_RENDER_SCALE` |
| `render.aniso` | int | anisotropic filtering on mipmapped textures: `0` (off), `2`..`16` | `0` | `RECOMP_RENDER_ANISO` |
| `render.snap_2d` | bool | above `render.scale` 1, 2D batches sample their textures on the guest grid, as at 1x (no seams between tiles, blockier 2D art) | `false` | `RECOMP_RENDER_SNAP_2D` |
| `display.aspect` | string | `"W:H"`: `"4:3"` (stock) or a wider ratio such as `"16:9"`, `"16:10"`, `"21:9"`: Hor+ on Metal and D3D11 ([Hor+](#hor-widescreen-displayaspect)); the CPU rasteriser ignores it with a log line | `"4:3"` | `RECOMP_DISPLAY_ASPECT` |
| `display.movie` | string | `"black"`, `"fit"`, `"fill"`: with a wide aspect, a full-screen movie or still is pillarboxed, stretched to the width, or scaled to cover it (top and bottom cropped) | `"black"` | `RECOMP_DISPLAY_MOVIE` |
| `display.overscan` | int | guest pixels cropped from each left and right edge at present, top and bottom in proportion: `0` (stock) to `64` ([Overscan](#display-overscan-displayoverscan)); ignored with a wide `display.aspect` and by the CPU rasteriser, each with a log line | `0` | `RECOMP_DISPLAY_OVERSCAN` |
| `display.hud_aspect` | string | how far a HUD mod moves the HUD ([HUD table](#hud-table-v5)): `"display"` (the frame's edges, as wide as `display.aspect`) or `"W:H"` from 4:3 up, capped at `display.aspect`. Narrower than 4:3 reads as 4:3 and an invalid value as `"display"`, each with a log line. Without a HUD mod, or at 4:3, nothing reads it | `"display"` | `RECOMP_DISPLAY_HUD_ASPECT` |
| `display.hud_margin` | int | guest pixels a HUD mod's edge elements are pulled in from the HUD's edge: `0` to `64`; out of range is clamped with a line | `0` | `RECOMP_DISPLAY_HUD_MARGIN` |
| `present.filter` | string | `"nearest"`, `"linear"`, `"integer"` | `"nearest"` | `RECOMP_PRESENT_FILTER` |
| `present.fullscreen` | bool | borderless fullscreen at start | `false` | `RECOMP_PRESENT_FULLSCREEN` |
| `present.pacing` | string | `"sleep"`, `"spin"` (stock, the console-original timing): how a lowered guest busy-wait waits | `"sleep"` | `RECOMP_PRESENT_PACING` |
| `present.profile` | string | `"none"` or a shader profile's slug, its file `config/shader-profiles/<slug>.toml`: a chain of slang presets run in place of the final present, on Metal and D3D11 ([shader profiles](#shaderprofiles)); in the layer that sets it, it wins over `present.shader` | `"none"` | `RECOMP_PRESENT_PROFILE` |
| `present.shader` | string | `"none"`, `"crt-lottes"`, `"crt-lottes-fast"`, `"crt-hyllian"`, `"fsr"`, or a `.slangp` path: a RetroArch slang preset run by librashader in place of the final present, on Metal and D3D11 ([present.shader](#presentshader)) | `"none"` | `RECOMP_PRESENT_SHADER` |
| `present.shader_params.<preset>.<param>` | number | a slang preset's runtime parameter, per preset: `<preset>` is the bundled name or a path's file stem, `<param>` a `#pragma parameter` name. Clamped to the parameter's range ([shader parameters](#shaderparams)) | the preset's own value | none |
| `present.shader_lib` | string | the librashader library; relative to the executable's directory. Loaded as given, with no hash check (the sha256 pin covers only the build's own copy) | `librashader.dll` beside the executable; macOS `librashader.dylib` beside it, then `../Frameworks/librashader.dylib` | `RECOMP_PRESENT_SHADER_LIB` |
| `present.shader_lines` | string | `"auto"`, `"guest"`, `"host"`: whether the shader chain gets the guest's line count or the frame at `render.scale` ([pass order](#presentshader)); `auto` decides by the preset's name. Over a profile's `lines` | `"auto"` | none |
| `present.surface` | string | `"auto"`, `"window"`, `"internal"`: who scales the frame to the screen, the game or the compositor | `"auto"` | `RECOMP_PRESENT_SURFACE` |
| `textures.pack` | string | a texture pack file ([texture-pack.md](../formats/texture-pack.md)); relative to the executable's directory | none | `RECOMP_TEXTURES_PACK` |
| `textures.font` | string | `"on"`, `"off"`: the pack's font-atlas entries | `"on"` | `RECOMP_TEXTURES_FONT` |
| `textures.text` | string | `"on"`, `"off"`: the pack's baked-text entries | `"on"` | `RECOMP_TEXTURES_TEXT` |
| `textures.world` | string | `"on"`, `"off"`: the pack's world entries (version 3) | `"on"` | `RECOMP_TEXTURES_WORLD` |
| `overlay.stats` | bool | the stats strip in the window's top right corner: fps, frame ms (average and worst of the last second), pacing, scale and backend, at 2 Hz; never in a frame dump | `false` | `RECOMP_OVERLAY_STATS` |
| `overlay.ui_scale` | number | the menu's and the overlays' size over the automatic scale (`overlay.ui_fit`), 0.75 to 2 in quarter steps: larger for a TV across the room. Live from the menu: the font atlas is baked again between frames | `1` | `RECOMP_OVERLAY_UI_SCALE` |
| `overlay.ui_fit` | string | what the automatic scale follows: `"display"` (the larger of the output height over 1080 and the display's floor: the OS's UI scale, and on a Steam Deck's own 1280x800 panel (`SteamDeck=1` or its SMBIOS names) its 216 ppi over 160, held to the output height over 600; [Overlay scale](#overlay-scale)) or `"output"` (the output height over 1080 alone, the rule before the display floor; the height shown, so under gamescope the window's lines after the compositor's fit to the screen). Live from the menu, like `overlay.ui_scale` | `"display"` | `RECOMP_OVERLAY_UI_FIT` |
| `overlay.menu_size` | string | `"compact"` (a panel at the left, the game beside it) or `"expanded"` (the whole window over a dimmed game). R3 in the menu flips it and writes this key at once, alone; not with `RECOMP_CONFIG=none` | `"compact"` | none |
| `overlay.menu_hints` | string | where the menu shows the selected row's help: `"tooltip"` (a card beside the row, 350 ms after a pad or key selection settles, 500 ms after the mouse rests), `"inline"` (a line under every row) or `"off"`. L3 (on release), F1 and the header's `?` flip between tooltip and inline (from off: inline) and write this key at once, alone; not with `RECOMP_CONFIG=none` | `"tooltip"` | none |
| `menu.enabled` | bool | the in-game menu, opened by the `menu.open` action ([In-game menu](#in-game-menu-menuenabled)) | `false` | `RECOMP_MENU` |
| `storage.ram_cache` | bool | the title's cache partition (`Z:`, `Partition3`-`5`) lives in memory: capped at the stock 750 MB (a full one answers `STATUS_DISK_FULL`, as FATX does), seeded read-only from `<hdd>/Cache` at the first file call, never written back. For a title that rewrites its cache all the time (BLiNX 1: about 7 GB an hour); the console clears these partitions anyway. On by default for every title; `false` puts the files in `<hdd>/Cache` as stock. Read before the guest starts | `true` | `RECOMP_STORAGE_RAM_CACHE` |
| `system.language` | string | the dashboard language the title is told (`XC_LANGUAGE`, EEPROM index 7, and `+0x30` of the `XC_MAX_OS` block): `en`, `ja`, `de`, `fr`, `es`, `it`, `ko`, `zh` (traditional Chinese) or `pt` (1 to 9), lower case only. A game that names its languages (game.toml `[system] languages`, built in as `XBOXRECOMP_LANGUAGES`) gets a Language item on the menu's Game page with those only, and any other code is logged and gives English. Read before the guest starts; titles read it once at boot, so a change applies at the next start (kernel_language.h) | `"en"` | `RECOMP_SYSTEM_LANGUAGE` |
| `mods.<name>` | bool | a mod ([Mods](#mods)) runs; `false` leaves its init out at start. The menu's Mods page writes it | `true` | none |

A title adds its own tables (`[fps]`, `[fx]`, ...) in the same files, and
lists them with their stock values in its `config.default.toml`. The
title reads them through the same layering and binds each to one of its own
environment variables, `enhance_cfg_bind_env("game.mode", RENV_GAME_MODE)`,
where `RENV_GAME_MODE` is a row of its `recomp_env_game.h`. A title that
offers only some of a key's values reads the key all the same and says in
one line which value it runs instead. Such keys change only that game's
code and leave the toolkit's backends alone, so every backend sees the same
guest values. The title documents its keys with its environment variables.

The keyboard and mouse keys (`[input]`, `[input.bindings]`) have their own
section, [Input](#input), below.

### Hor+ widescreen (display.aspect)

A ratio wider than 4:3 widens the picture instead of stretching it. Every
4:3-shaped colour and depth target is allocated wider by a pad a side
(`nv2a_host_pad`: 107 guest pixels for 640x480 at 16:9) and the title's
columns are mapped centered in it, so the game's own perspective projection
fills the pads with more of the world. The projection is not patched: what
the title culls at the old frustum's edge pops in at the screen's edge.
Guest memory and the write-back keep the 640x480 centre columns, so the
guest and the goldens see the stock frame. A window (not fullscreen) opens
at the padded frame's shape, `nv2a_host_window_w`: 854x480 at 16:9 and
scale 1, so the pads show 1:1; fullscreen keeps the display's size and
letterboxes. A square target (BLiNX 2's screen-space reflections) is
widened by the same ratio plus an eighth of its width a side, so a
reflection read the water bumps past the frame's edge still finds the
mirrored view instead of the target's clamped edge column.

Each batch is placed by one rule table (`nv2a_aspect_place`,
`src/kernel/nv2a_backend_common.h`), after a 2D test that runs the batch's
vertex program on the CPU and checks that every vertex shares one `oPos.w`,
an exact whole number from 1 to 1024 (`nv2a_aspect_layer_w`,
`nv2a_aspect_scan`, gated per program). `oPos.xy` are pixels whatever `w`
is, and BLiNX 2 draws parts of its stage HUD at a fixed view depth of 2 or 3
and its title movie at 100:

- 3D: centered; a clip box at the frame's edges reaches the target's edges.
- 2D, not full width (HUD, menus, text): centered and scissored to the 4:3
  frame, so pieces the title parks off-screen never show in the pads. The
  HUD keeps the title's 4:3 layout unless a HUD mod moves it: a 2D batch's
  triangles say nothing of which HUD element they belong to, so moving
  pieces by their position would split groups the title draws as one
  (BLiNX 2's item counter from its items, a dialogue face from its box).
  A HUD mod moves them in guest memory instead, per element, where the
  title hands their vertices over ([HUD table](#hud-table-v5)); while one
  is active such batches on the frame get the 3D box (the lift), so the
  moved elements show in the pads.
- 2D, full width, sampling only render targets, nothing, or a guest
  texture no wider than a quarter of the frame (fades, post passes):
  stretched over the whole target. On a square target that is the ratio's
  pad only (`nv2a_host_pad_ratio`), not the overscan, so a full-width copy
  between the frame and a square lines up, and reads of a padded square
  by such a pass take the same columns.
- 2D, full width over a wider guest texture (a movie, a still):
  `display.movie`.
- A quad of at most 6 vertices sharing one positive `w` that is not a
  screen layer (`nv2a_aspect_layer_w`; `nv2a_aspect_screen_quad`) is not 2D by the test above, but it is
  stretched in either of two cases:
  - it samples only padded render targets and spans exactly the frame's
    width: a pass laid over the frame (JSRF's Can boost). Such a quad
    wholly in the top or bottom third also marks that third as holding a
    bar, for this flip and the next;
  - it is drawn with the depth test off, over no guest picture, within the
    frame's width and height, reaching its left or right edge, and wholly
    in a third that holds a bar (`nv2a_aspect_band`): a piece of a
    letterbox band. Burnout 3's crash cam draws its bars, their edge lines
    (as half-width quads) and the list backing that way at one `w`, so they
    all reach the target's edges together while the text inside stays in
    the 4:3 frame. Its front-end menu frame has the same shape but no bar,
    so it keeps its 4:3 place.
- A batch drawn with the depth test off whose vertices (all of them under
  64, about 32 evenly spaced beyond) share one `w` under 1 that is the reciprocal of a whole number from 2 to
  1024 (`nv2a_aspect_flat_w`, `nv2a_aspect_flat`) is a flat near layer: it
  keeps its centred place but is scissored to the 4:3 frame like 2D.
  Burnout 3 draws its whole front end at rhw 10 (`w` 0.1); its menu grid
  overhangs the frame to x 656 and text slides in from off screen, which
  stock hides past the screen's edge. Its race's place tags over the rival
  cars use the same program at the car's computed depth, between 0.54 and
  1, where no reciprocal lies, so they still follow their car into the
  pads. The known limit: camera-locked geometry drawn without depth at a
  round view distance (`w` 0.5, 0.25, 0.1), such as a full-screen flash
  or tint quad or a lens overlay, is taken too. If it overhangs the 4:3
  frame on purpose, it is cut at the old 4:3 edge: a pad tinted only up
  to the 4:3 frame is this case.

`RECOMP_TRACE=aspect` prints one `[aspect.program]` line per program,
texture and placement, and the target size per flip, and one
`[aspect.flat]` line per program and `w` the first time a flat near layer
is clipped.

### Display overscan (display.overscan)

Some titles leave a dark border at the left and right edges of their 4:3
frame, because the televisions they were made for hid the outer few percent
of the picture. `display.overscan = n` does what such a TV did: it cuts `n`
guest columns from each side and `n * h / w` guest rows from the top and
bottom, then zooms the rest uniformly into the place the whole frame would
take. 640x480 at 9 shows guest (9, 6.75) to (631, 473.25). The value is in
guest pixels, so it reads the same at every `render.scale`. Above 64 is
clamped to 64 and below 0 to 0, each with a warning.

- **Present only.** The crop is a source rectangle on the Metal and D3D11
  present. The guest frame, the host targets, the write-back, `fb_dump`,
  `metal_host_dump` and the D3D11 dump are unchanged; `window_shot` reads
  the window, so it shows the crop.
- **Filter.** With `nearest`, a zoom of 622 columns into the window draws
  uneven columns; `present.filter = "linear"` hides that. Under `integer`
  the vertical cut is rounded to whole host rows (7 at scale 1), so the
  picture's aspect is off by up to one row.
- **Not with Hor+.** With a `display.aspect` wider than 4:3 the 4:3 frame's
  edges sit inside the padded frame, where no source rectangle can cut them,
  so a non-zero value is reported once and 0 is used.
- **Every frame.** The crop applies to every screen, so a title whose border
  shows only on some screens loses those columns everywhere.
- Ignored by the CPU rasteriser (one log line) and by the Metal readback
  debug present (`metal_present=readback`). A non-zero value adds
  ` display.overscan=N` to the `[ENHANCE]` line, after `display.aspect`.

### What the render and present keys do

`xbox_enhance_init` reads them once and hands them to the backends and the
presenter as `nv2a_host_opts` (`src/kernel/nv2a_backend_common.h`). Those
libraries never link the layer: without it the options are all zero, which
is stock. One line shows the values in force:

```
[ENHANCE] render.scale=2 render.aniso=0 present.filter=integer present.fullscreen=0 present.pacing=sleep present.surface=window(auto) (display.aspect=4:3)
```

A change while the game runs (the in-game menu) goes through
`nv2a_host_opts_request(o, mask)`, from any thread: it is queued, and the
Metal and D3D11 backends apply it with `nv2a_host_opts_apply()` at the
next flip, right after its present: the one point where no target is bound
and no upload is in flight, and the frame shown is still sized by the scale
it was drawn at. A new `render.scale` then drains the pending occlusion
queries at the old scale and drops every render target, depth
buffer and texture (Metal writes a target back
first, so the next one seeds from it), with one line
(`render.scale=N from this flip: targets and textures dropped`); a pack
change makes every texture upload again. The CPU and null backends apply
too and repeat their "ignored" line on a scale or overscan change.

The other live keys need nothing at the flip but the stored value:
`render.aniso` is part of the sampler key at each draw, `display.movie`
is read at each movie draw, and `display.overscan` at each present (a
non-zero overscan under a wide `display.aspect` is refused, as at start,
and the aspect itself is start-only). `present.filter` and
`present.fullscreen` are the presenter's: the SDL window's thread sees the
options generation move and sets the texture's scale mode or calls
`SDL_SetWindowFullscreen`; the D3D11 window, which the walker thread owns,
swaps its style with `SetWindowPos` in the same on_flip, and the next
flip resizes the swap chain. `present.pacing` changes through
`xbox_enhance_set_pacing(mode)` (`enhance.h`), from any thread. Each shows
within two flips. `RECOMP_DEBUG=opts_at=<flip>:<key>=<value>,...` makes the
same requests at given flips, to check a switch without the menu.

- **`render.scale = N`** (1..4; outside that it is clamped, with a line).
  Metal and D3D11 allocate every colour and depth target at N times its
  guest size and draw with an N-times viewport. The pixel-to-NDC mapping
  stays on the guest size, so every vertex program, the pre-transformed
  HUD ones included, lands where it did, at N times the pixels.
  Render-to-texture samples the scaled target (supersampled). A bilinear
  linear-format (rect) texture drawn 1:1 to guest pixels, such as a movie
  frame, is sampled where the 1x render samples it: each N x N block of
  host pixels reads its guest pixel's 1x sample point, so the padding past
  the picture does not bleed into the last row and column. A bilinear
  texture on a 2D batch (a pre-transformed one, always; a program one
  whose vertices share one whole w and one depth; on an unpadded target or
  centred on a padded one), swizzled and compressed ones too, with u along x and v along y
  (mirrored or not; a mapping turned 90 degrees is left alone), is
  sampled the same way: a WRAP tile's edge pixels would otherwise pull
  part of the opposite edge's texel and draw a thin light line between
  loading-screen tiles. This 2D form is behind `render.snap_2d` (default
  off, so stock scaling blends 2D art at the host rate as before); when on,
  such textures look as at 1x, in N x N blocks, not with host-rate
  bilinear smoothing, and keep the 1x mip level and min/mag filter through
  explicit gradients. 3D batches and render-target textures are left
  alone. With it on, the walker does not merge back-to-back batches (as
  under Hor+), since each batch is classed from its own vertices. The frame the
  guest sees is still guest-sized: Metal box-filters its write-back to
  guest memory, so `RECOMP_FB_DUMP` and `fb_dump_at` frames stay 640x480.
  The box filter averages the stored (gamma-encoded) values, not linear
  light, the same as xemu's downscale.
  Visibility-test counts are divided by N squared before the title reads
  them, a non-zero count staying non-zero. The window gets the full-size
  frame. The CPU rasteriser draws into guest memory at the guest pitch, so
  it ignores the key and says so once
  (`[ENHANCE] render.scale=N ignored: ...`).
  - Known differences from the console: post passes that sample with
    guest-texel offsets (blurs) cover N times fewer host pixels and look
    sharper; lines and points stay one host pixel wide.
  - D3D11 `d3d11_dump` frames of a scaled run are the render target at host
    size (1280x960 at 2x). Golden frames are compared at the stock size
    only, and a golden run pins `RECOMP_RENDER_SCALE=1`.
- **`render.aniso = N`** (2..16; 0 or 1 is off, the stock filtering;
  outside 0..16 it is clamped, with a line). D3D11 and Metal sample a
  texture anisotropically when it has mip levels, its minification filters
  and it is not a cube map: the world's surfaces, sharp at grazing angles. A
  texture without mips is left alone, since those are render targets read
  back (bloom, blur and composite passes), linear-format movie frames and
  lookup tables: screen-aligned or data, and they shimmer under it. Both
  backends filter min, mag and mip linearly under it. Live; the CPU
  backend ignores it.
- **`present.filter`.** `nearest` is the stock letterbox. `linear` is the
  same letterbox with bilinear filtering. `integer` shows the frame at the
  largest whole multiple that fits, centered on black; a frame larger than
  the window (2x in a small window) is fitted with linear filtering instead
  of cropped. The SDL window (macOS, Linux, every backend) and the D3D11
  window follow it. The D3D11 window also follows resizes now; at stock it
  still copies the frame into its 640x480 back buffer, and anything else
  goes through a scaling blit. The Win32 GDI window of the CPU backend does
  not follow these keys yet.
- **`present.fullscreen`.** A borderless window covering the display at
  start, with no mode change (SDL `SDL_WINDOW_FULLSCREEN` at the desktop mode; D3D11 a `WS_POPUP`
  the primary monitor's size). Live: on and off while the game runs (the
  D3D11 window goes back to the size it had, or to the starting size). The
  CPU backend's Win32 GDI window does not follow it.
  - *macOS.* A borderless window over the whole display (menu bar row
    included), with no Space and no slide animation, when fullscreen is
    configured, the in-game menu is armed or `opts_at` is set: the presenter
    sets `SDL_VIDEO_MAC_FULLSCREEN_SPACES=0` before `SDL_Init` (a Spaces
    switch in an app that is not frontmost does nothing). The window drops
    to the normal level while it has no focus, so Cmd-Tab uncovers the
    desktop, and the green button zooms. A player config arms the menu, so
    in practice only stock and golden runs keep Spaces. The variable in the
    environment overrides either way.
  - *Linux (native SDL).* SDL's desktop fullscreen, a borderless window at
    the display mode. Under gamescope (Steam Game Mode) every window already
    fills the output, so the key makes little difference there; under
    Proton the D3D11 popup applies.
  - The mouse capture (`input.mouse_capture`) is SDL's relative mode on the
    window, which follows the window through a switch; the Win32 cursor
    clip is re-applied on `WM_SIZE` and `WM_MOVE`.
- **`present.surface`.** Who scales the frame to the screen. `window` is
  the stock behaviour: the swap chain follows the window, and the game
  scales the frame into it as `present.filter` says. `internal` makes the
  surface the frame itself, 640x480 times `render.scale` (1280x960 at 2x):
  D3D11 creates the window's client area and the swap chain at that size
  (a `WS_POPUP` at the top-left corner under `present.fullscreen`, without
  asking the screen size), resizes the window and the swap chain only when
  the menu changes `render.scale`, and copies the frame into it under every
  filter. Dragging the edge of a desktop window stretches the swap chain to
  the window without keeping the aspect (DXGI's stretch), where Metal
  letterboxes. A scaling compositor then fits it to
  the output with its own filter. That is the point under gamescope (Steam
  Game Mode): Steam's *Scaling filter* (FSR, linear, nearest, ...) applies
  to the surface gamescope receives, and FSR upscales only a surface
  smaller than its output, so with `window` (a surface the screen's size)
  it has nothing to do. On the Steam Deck use `render.scale = 1` for FSR:
  at 2x the 1280x960 surface is taller than the 800-line output and is
  downsampled instead (a sharp picture, but not FSR). On a desktop without
  a scaling compositor `internal` shows the frame unscaled. `auto` (the
  default) means `internal` when the launcher sets `RECOMP_HOST_GAMESCOPE`
  (the SteamOS bundle's `launch.sh` does under gamescope) and `window`
  otherwise; the runtime does not look for gamescope itself. A bare `.exe`
  started under gamescope without the launcher sets
  `RECOMP_PRESENT_SURFACE=internal` (or `surface = "internal"`) by hand.
  The `[ENHANCE]` line names the result and its reason:
  `present.surface=internal(auto: gamescope)`, `window(auto)`, `internal`.
  The key is read once, when the window is made; the menu cannot change it.
  The Metal window (macOS) keeps its drawable at the frame's size under
  `internal` and lets the layer scale it, letterboxed, nearest for
  `nearest` and linear otherwise; the macOS launcher sets no hint, so
  `auto` gives `window` there unless `RECOMP_HOST_GAMESCOPE` is set by hand. The
  SDL renderer window (the CPU backend) ignores the key with one line.
- **`present.shader`.** <a id="presentshader"></a>A RetroArch slang
  preset, run by [librashader](https://github.com/SnowflakePowered/librashader)
  0.12, replaces the draw that puts the frame on the screen. Its input is
  the frame at `render.scale` (the whole frame, its guard band included;
  a CRT preset gets it at the guest's line count, see the pass order
  below), and its output fills the same letterboxed rectangle the stock
  present would, so the aspect, `present.surface` and the window size
  behave as before; `present.filter` and `display.overscan` do not apply
  while a shader runs (overscan says so once). The bundled presets sit in
  `shaders/` beside the executable (`assets/shaders`, public domain, MIT
  and Unlicense only, listed in its README): `crt-lottes` and
  `crt-lottes-fast` (shadow mask, scanlines, curvature), `crt-hyllian`
  (the one-pass fast variant) and `fsr` (AMD FidelityFX Super Resolution
  1, an upscaler, not a CRT). Any other value with a `/` or ending in
  `.slangp` is a preset path, relative to the executable's directory; a
  bundled name or path that is not there says so once and the present
  stays stock. The library is loaded on first use, never linked:
  `cmake/librashader.cmake` stages a prebuilt release (pinned by
  sha256; upstream's on macOS, our D3D11-only build of the same source on
  x86-64 Windows, which needs no DirectX 9 or Visual C++ runtime) beside
  the executable when a game calls
  `xboxrecomp_present_shaders(<target>)`; a configure that cannot
  download it warns and builds without it. `present.shader_lib` points at
  another copy, loaded as given with no hash check: it runs code from the
  path the player names, as any library they put there would. A missing
  library, one with another ABI, or (Windows) a library whose import is
  missing (named in the line, error 126) gives one `[SHADER]` line and
  the stock present. The menu's *Profile* row
  (Shaders page, see [Shader profiles](#shaderprofiles)) switches it live: the next present builds the new chain,
  and a preset that fails to build is not retried until the next change.
  The menu leaves the Shaders page out where no shader can run: no chain
  presenter (the SDL renderer, or the CPU rasteriser's window on Windows),
  or a library that is missing or does not load. It says why once
  (`[SHADER] the menu's Shaders page is hidden: ...`). With a working
  library and `none`, the page stays, for its picker.
  `none` costs one atomic load per present and loads nothing. D3D11 frame
  dumps add `present_NNNNN.bmp`, the back buffer after the shader, beside
  the usual render-target dumps, which stay the game's own frame. The
  CPU rasteriser ignores the key.

  While a shader runs, `present.surface = internal` draws at the output
  size, as `window` does: the chain's mask and scanlines are drawn per
  output pixel and per source line, and a compositor that scaled a
  frame-sized result (gamescope's 640x480 to 1280x800) resampled them
  into colour fringes and moiré. D3D11 under gamescope or fullscreen turns
  the window into a borderless one over the monitor, with the swap chain
  at that size; otherwise the swap chain follows the window's client
  area. Metal's drawable follows the window. With the shader set back to
  `none` the frame-sized surface comes back at the next present.

  The pass order, the same on D3D11 and Metal:
  1. The game draws its frame at `render.scale` (1280x960 at 2x).
  2. When the chain is fed the guest's lines (below), the frame is
     resolved to the guest's line count at full host width
     (1280x480 at 2x, 1920x480 at 3x), each row the mean of its
     `render.scale` host rows. A CRT preset draws one scanline per
     source row, so a scaled frame gave it 960 or 1440 lines for an
     output of 800 rows, under one pixel per scanline, and the beat
     between the two showed as wide horizontal bands. Fed the guest's
     480 lines the scanlines match scale 1, and the extra width keeps
     the sharper picture across each line. Any other preset (`fsr`, a
     path named otherwise) gets the scaled frame as it is, as does a
     frame whose height is no multiple of `render.scale`. The first frame
     logs which it got (`fed the guest's lines (480 of 960 rows)` or `fed
     the host's 961 rows (not a multiple of render.scale)`).

     Which presets get the guest's lines: `present.shader_lines = "guest"`
     or `"host"` decides for every preset. Under `"auto"` (the default) a
     profile's `lines` decides, and failing that the name of the preset
     (a profile's first stage):
     - `crt` leading the file stem (`crt-lottes`, `crtglow_gauss`) or as a
       `-`, `_` or `.` separated word anywhere in it (`zfast-crt`,
       `newpixie-crt`);
     - a word starting with `scanline`;
     - a parent folder named `crt`, as in RetroArch's `shaders_slang/crt/`.

     A CRT preset named otherwise and kept outside a `crt` folder, such as
     `megatron`, `gtu` or `mame_hlsl`, needs `"guest"` (or a profile with
     `lines = "guest"`).
  3. The chain runs into an intermediate the size of the letterboxed
     rectangle, so each pass's `OutputSize` and `FinalViewportSize` are
     the picture's, not the whole window's (librashader gives the final
     pass its target's size, and a mask or curvature laid out for the
     window was cut by the rectangle).
  4. The intermediate is copied 1:1 into the rectangle; the bars stay
     black. Under `present.surface = internal` the rectangle is the
     letterboxed fit the compositor would have given the stock frame,
     even with `present.filter = integer`, whose largest whole multiple
     (1x at scale 1 on an 800-row screen) showed the picture smaller
     with the shader on than off.
- **Shader parameters.** <a id="shaderparams"></a>A slang preset declares
  runtime parameters (`#pragma parameter`: a name, a label, a range and a
  step). librashader reads them when the chain is built, and the menu's
  **Shaders** page lists the active chain's: under the stage row for a
  one-stage chain, else in each stage's options (that stage's own names;
  a name an earlier stage declares is listed there). One range row each: the label, the value at the step's decimals, and the
  name, range and preset in the footer. A change applies at the next
  present on Metal and D3D11; *Reset to preset* drops the active preset's
  overrides, and the Save page's *Reset to defaults* drops every preset's.
  Save writes each preset's overrides as differences only, in their own
  table:

  ```toml
  [present.shader_params.crt-lottes]
  warpX = 0.0       # no horizontal curvature
  maskDark = 0.6
  ```

  A value equal to the preset's own is no override, so Save deletes that
  key. Overrides of a preset that is not running are kept as read. The
  table may sit in any layer (defaults, user, title; a later one wins);
  Save writes the user file only, so a title or defaults value that a
  slider or a reset removed loads again at the next start; remove it from
  that file to drop it for good. Overrides are keyed
  by file stem, so a path preset named like a bundled one
  (`mods/x/crt-lottes.slangp`) shares the bundled one's. A key whose name has no `<preset>.<param>`
  shape, or whose value is not a number, warns once and is skipped; a
  parameter the preset does not declare is ignored. There is no
  environment variable for the parameters. The bundled `crt-lottes` and
  `crt-lottes-fast` set a gentler curvature than upstream (their
  `assets/shaders` README lists both); no bundled preset has a
  convergence control, so the mask settings (`shadowMask`, `maskDark`,
  `maskLight`, `MASK`, `MASK_INTENSITY`) are the ones to adjust colour
  fringes with.
- **`present.pacing`.** Only for a title generated with
  `tools.recomp --spin-waits` ([lifting](../pipeline/04-lifting.md#spin-waits)),
  whose busy-wait loops call `RECOMP_SPIN_WAIT` on their back edge. `sleep`
  (the default) blocks the thread until the kernel signals a state change
  (a vblank, an ISR or DPC, a fence write) or 1 ms passes
  (`src/kernel/kernel_pacing.h`), which frees a host core. `spin` is the
  console-original timing and does nothing there: the loop holds a host
  core as it holds the console's CPU. Set it with `[present]`
  `pacing = "spin"` in the player's `config.toml`, or
  `RECOMP_PRESENT_PACING=spin`; a title's `config.default.toml` can ship
  either, and its value wins over this default, and a game can change the
  default itself with `enhance_cfg_set_code_default` (see
  [Two files and the environment](#two-files-and-the-environment)). Without the layer the
  kernel stays at `spin`. The frames are the
  guest's own either way; what can move is when the loop sees its exit,
  by up to the wake latency. Without lowered loops the key changes nothing.
  A site lowered with `"sleep": "kick_event"` (`RECOMP_SPIN_WAIT_KICK`)
  sleeps only while the walker's kick event wait is on as well, and spins
  otherwise, so with `kick_event` off it is `spin` at either value.
  `RECOMP_TRACE=pacing` prints the flip intervals, process CPU and, per
  site, how its waits ended and how its loops left: on a wake, or on the
  1 ms timeout. A site whose loops keep leaving on the timeout (more than
  5% of exits) has a writer the kernel never signals; timeouts in the
  middle of a loop are normal.

### Shader profiles

<a id="shaderprofiles"></a>A shader profile is a named chain of 1 to 8
slang presets, run as one chain in place of the present
(`src/enhance/shader_profile.h`). Each is one file,
`<root>/config/shader-profiles/<slug>.toml`, beside the user's
`config.toml` whatever title file the layers read, so a game with several
titles keeps one set. With `RECOMP_CONFIG=<file>` the folder is
`shader-profiles/` beside that file; with `RECOMP_CONFIG=none` there are no
profiles. The file name is the profile's slug: its name with ASCII letters
lowered, digits, `-` and `_` kept, and every other run of bytes one `-`
(`Soft CRT` is `soft-crt.toml`). A file whose stem is not a slug is skipped
with one line.

```toml
# xboxrecomp shader profile (docs/runtime/config.md, "Shader profiles")
version = 1
name = "Soft CRT"
stages = ["fsr", "crt-lottes"]

[params.fsr]
FSR_SHARPENING = 0.2

[params.crt-lottes]
warpX = 0.0
maskDark = 0.6
```

- `version` (integer, required): 1. A higher one is listed read-only as a
  newer format and never written; a missing one, or one that is not an
  integer of at least 1, makes the file broken.
- `name` (string, at most 48 bytes of UTF-8, cut at a character boundary
  with one line): the display name; the slug when absent.
- `stages` (array of strings, 1 to 8; past 8 the first 8 are taken with one
  line): the chain, first stage first. A stage is a bundled name
  (`crt-lottes`, `crt-lottes-fast`, `crt-hyllian`, `fsr`), `user:<stem>`
  for `<root>/shaders/<stem>.slangp`, an absolute `.slangp` path, or a
  relative one, looked up in `<root>/shaders/` and then beside the
  executable. A file in `<root>/shaders/` named like a bundled preset is
  refused, so `fsr` always means the bundled one.
- `lines` (string, optional): `"auto"`, `"guest"` or `"host"`, as
  `present.shader_lines` for this profile, which a `"guest"` or `"host"`
  there overrides. Only the first stage sees the frame, so it is read
  for that stage. Another value is logged and taken as `"auto"`.
- `[params.<preset key>]`: parameter values, keyed by the stage's preset
  key (the bundled name, or the file stem as for
  `present.shader_params`), so a reorder changes only `stages`. A value
  that is not a number is skipped with one line. librashader keeps one
  value per parameter name over the whole chain, so a name two stages
  declare is saved once, under the first stage that declares it; a value
  found under another stage's table is read and moved there at the next
  save.

- `[param_kinds.<param>]` (optional): how the Shaders page shows a
  parameter of this profile, over the bundled table and the rules below.
  `kind` is `"toggle"`, `"choice"` or `"slider"`; `choices` are a choice's
  names, one per value from the minimum (2 to 8, each under 32 bytes), or a
  toggle's off and on words; `help` (under 100 bytes) leads the row's help
  line. Keyed by the name alone, as a profile has one row per name. The
  page reads this table, the loader skips it and a save keeps it.

**Parameter kinds.** A preset declares each parameter as a number, so the
Shaders page decides how a row shows by the first rule that matches
(`src/enhance/shader_param_kinds.h`):

1. declared: the profile's `[param_kinds]`, then the bundled
   `shaders/param-kinds.toml` beside the executable, keyed by the pass
   shader that declares it (`crt-lottes.slang`) and the name. A declared
   choice whose names are not one per value of an integer range does not
   match: the bundled file's declaration is tried next, then rule 2;
2. a range of 0 to 1 by 1: a toggle;
3. an integer range of 3 to 8 values by 1 whose description ends in
   `: name | name ...` with one name per value: a choice;
4. a slider.

A toggle or a choice drops a trailing `: a | b` list from its label
(`FSR LFGA Film Grain Color: Gray | RGB` reads `FSR LFGA Film Grain
Color`). Values stay numbers whatever the kind: a toggle stores the minimum
(off) or the maximum (on) and reads on from the middle of its range up, a
choice the minimum plus its index, so
`[params]` and `present.shader_params` hold what they always did.

Unknown keys and tables are reported (`[SHADER] <file>:<line>: unknown key
<key>`) and kept: a save changes only `version`, `name`, `stages` and the
`[params]` values (it keeps `lines` as it is), through `recomp_cfg_patch`, so comments and other lines
stay. A file that does not parse is listed broken, with its error, and is
never written.

**Composing.** librashader builds a chain only from one preset file, and a
preset can `#reference` only one other, so a profile of two or more stages
is composed into one `.slangp` at
`<save root>/ShaderCache/slangp/<slug>.slangp` (`~unsaved.slangp` for a
chain not saved yet), rewritten through a temporary at every compose and
never read back. A one-stage profile runs its preset as it is. The
composer follows each stage's `#reference` chain (16 deep, cycles refused;
the referencing file's lines win), renumbers the passes in stage order,
copies every per-pass key through, writes every shader and texture path
absolute, quoted and with forward slashes, merges the `parameters` lists
(duplicates dropped) and takes each preset-level value from the earliest
stage that sets it. A stage that is not the last and whose last pass sets
no `scale_type` gets `scale_type = "viewport"` there, the size it had on
its own (an unset last pass is viewport-sized only at the end of a chain).
The compose is refused, with a `[SHADER] profile <slug>: ...` line, for a
missing or unreadable preset, an alias or texture name two stages declare,
`PassOutputN`, `PassOutputSizeN`, `PassFeedbackN` or `PassFeedbackSizeN`
in a stage after the first (renumbering moves the pass they name), and
more than 16 passes in all.

**Which one runs at start.** `present.profile` names the profile;
`present.shader` stays the short form of a one-stage chain. They are
resolved as a pair, in this order:

1. `RECOMP_PRESENT_SHADER` set: that preset, as before profiles (benches
   and debugging), with a `present.profile=<slug> ignored` line when a
   profile is set too;
2. `RECOMP_PRESENT_PROFILE` set, and not `none`: that profile;
3. the highest file that sets either key (the per-title file, then
   `config/config.toml`, then `config.default.toml`) decides alone: its
   `present.profile` unless `none`, else its `present.shader` unless
   `none`, else stock. A lower file's keys are not consulted, so a
   per-title `present.shader = "fsr"` is not overridden by a profile in
   the shared `config.toml`, and `"none"` in a file stops a lower file's
   choice;
4. stock.

A chosen profile is named on an `[ENHANCE] present.profile=<slug>` line
(none at a stock start, as for `present.shader`), which the golden check
reads. A chosen profile that is missing, broken or of a newer format logs one
line and presents stock; it never falls back to `present.shader`. Only that
one file is read, and a stock start reads none. A one-stage profile is
requested at once. A chain is composed at the first present, since the
shader cache folder is set up after the config is read; a compose failure
then logs its line and the present stays stock.

**The Shaders page.** The menu's Shaders page edits the profiles; nothing
under `config/shader-profiles/` is read before the page first opens, and
the folder is read again each time it opens, so a file added or removed
outside the game shows up then.

- *Profile* (Left / Right): Off, the bundled presets, the configured
  preset when it is a path, then the folder's profiles by name. A broken
  or newer-format profile is listed red with its error and skipped by the
  picker. The choice is live; with `RECOMP_PRESENT_SHADER` or
  `RECOMP_PRESENT_PROFILE` set the row is read only and says which.
- *Chain*: how the chain stands (`2 passes`, `building`, or why it shows
  stock: a missing stage, a clash, a limit, a failed build).
- One row per stage. Left / Right moves it, X (Delete) removes it after a
  confirm, A opens its options: its preset and path, its parameters, Move
  up, Move down, Remove and *Reset this stage* (the values it declares
  first). The last stage stays; pick Off instead. A stage that reads the
  game frame (`Original`) or sees the scaled picture is noted.
- *Add shader*: the bundled presets, then `user:<stem>` for each
  `.slangp` in `<root>/shaders/`. A preset is added as the last stage only
  when it is found, parses and composes with the chain; past 8 stages or
  16 passes the row is dimmed with the limit.
- *Save as new* (a copy named `Profile N`), *Duplicate* (`<name> copy`),
  *Rename* (a text entry: keyboard, or the pad's Up / Down to cycle a
  character), *Delete* (after a confirm; the file and a `present.profile`
  naming it go at once, not at Save) and *Reset to preset*.

The first chain edit on a bundled preset or a path forks it into an
unsaved `<name> custom` profile and says so. Every edit is live; Save
writes each changed profile to its file (a rename moves the file) and
`present.profile`, or `present.shader` for a bundled preset, in the user
`config.toml`, and the unsaved count on the Save page includes them. The
Save page's *Reset to defaults* sets the picker to Off and leaves the
files. When the game runs from its own folder (`<root>` is the
executable's directory), `<root>/shaders/` is the bundled folder itself:
the bundled presets there are not listed twice and raise no name clash.

A later stage sees the earlier one's output, so a CRT preset after `fsr`
or another CRT draws its scanlines per output line; that is allowed, as in
RetroArch. `Original` and `OriginalSize` stay the game frame in every
pass, so a later stage whose shaders read them (crt-hyllian does) mixes
the game frame's size with the scaled picture and may look wrong; that is
allowed too, and flagged. A declaration alone (every bundled shader
declares `OriginalSize` in its uniform block) is not a read.

### Texture packs

- **`textures.pack = "<file>"`.** A pack of host-side texture replacements
  ([texture-pack.md](../formats/texture-pack.md)), keyed by the hash the
  Metal and D3D11 backends already compute for every upload. A relative
  path is resolved against the executable's directory, where
  `config.default.toml` is, so a bundle's defaults name `enhance/hud.pack`
  (a `.app` keeps the pack beside the executable too). The layer
  loads the pack's index at startup and logs one line after the settings
  line: `[ENHANCE] textures.pack=none` when unset, or
  `[ENHANCE] textures.pack=enhance/hud.pack (41 entries: 30 ui, 1 font, 10 text, 0 world; v2; scale 4, text 2; 0 bc1, 10 bc3) textures.font=on textures.text=on textures.world=on`
  (the path ends at the first space). A
  file that is missing, has another magic or version, a non-zero reserved
  field, flags the version does not define, an entry outside the file, an
  entry with more levels than its size allows
  (`1 + floor(log2(max(w, h)))`), a BC3 entry whose size is not a multiple
  of 4, or an unsorted index is refused whole
  (`[ENHANCE] textures.pack=<path>: <reason> (<resolved>); no pack`) and
  the run stays stock. On an upload (a cache miss or a texture whose
  bytes changed since the last flip) a backend looks the texel hash up
  and, on a hit whose entry has the guest texture's aspect ratio, creates
  the host texture at the pack entry's size and level count from the
  pack's texels (if the device refuses that texture, the guest texels go
  up instead); everything else (cache key,
  eviction, the per-flip rehash, samplers, UVs) is as it was, so a texture
  the game rewrites stops matching and returns to its guest bytes. Without
  a pack the only added work per upload is the entry-count test. The CPU
  rasteriser samples guest memory and ignores the key with one line
  (`[ENHANCE] textures.pack ignored: ...`). Every 600 flips a backend
  with a pack logs its hit count; under `RECOMP_TRACE=tex_pack` it also
  prints `[TEXPACK] resident <n> KB of <budget> KB, <n> freed` (the CPU
  copies held under the 512 MB budget) and the uploads the pack did not replace, one
  `[TEXPACK] miss <hash> <w>x<h> <format> x<count> candidate|linear`
  line each (texture-pack.md, "Misses"), which says what a pack still
  lacks. A texture the pack holds but did not use carries a tag:
  ` (text off)` (or art, font), ` (pack off)`, or ` (entry refused)` when
  the backend could not take the entry (a GPU without BC3, such as an
  older iPad, says so once; a texture too large; a failed read).
- **`textures.font`, `textures.text`, `textures.world` = `"on" | "off"`.** A
  pack marks each entry as art, the font atlas, baked text or (version 3) a
  texture of the 3D scene; `textures.world` turns those off alone.
  `off` makes the pack skip that kind, so the game's own glyphs or text
  images are drawn, for a player who prefers them or a language the pack
  does not cover. Both are read whether or not a pack is set; with one
  loaded they are in its log line. A change made at run time (the
  in-game menu, through `nv2a_host_opts_request`) takes effect at the next
  flip: the backends upload again every texture made under the old
  setting. A golden run must show
  `textures.pack=none`: the golden check fails any other value.
- **Magnification trace: `RECOMP_TRACE=tex_mag`.** How far each texture hash
  is ever magnified, with or without the dump: into the sidecars with it, in
  `[TEXDUMP] mag` lines at exit without it. A shader variant and a device
  buffer under the trace only; an upper bound
  ([texture-dump.md](../formats/texture-dump.md)).
- **`RECOMP_DEBUG=tex_pack_keep`.** Keeps a pack entry's payload in RAM after
  its GPU upload, under the 512 MB budget, instead of freeing it, for A/B.
- **Texture dump: `RECOMP_DEBUG=tex_dump=<dir>`.** Not a config key: a
  debug entry, off by default. The Metal and D3D11 backends write every
  texture they upload into `<dir>` once per texel hash, the hash a pack
  entry replaces: `<hash>.xtd` (the decoded A8R8G8B8 levels behind a
  64-byte header) and `<hash>.json` (size, levels, format, layout,
  palette, wrap, filters, address, first flip), with `counts.json` and
  `volatile.json` beside them ([texture-dump.md](../formats/texture-dump.md)).
  Under a pack hit the guest texels are decoded for the dump, so it
  always holds the game's own texture. Linear textures get their
  sidecar only; an address that takes more than 8 hashes, a render
  target bound as a texture and a texture over 16384 are counted, not
  written. A `[TEXDUMP]` line every 600 flips and at exit gives the
  counts. A run writes at most 2048 MiB, then the dump stops with one
  line. The CPU rasteriser keeps its first-use BMPs under the same
  entry and says it does not write the dump. The dump holds game
  texels: keep it out of anything published.

### Input

`[input]` turns the keyboard and mouse on as a source on port 0 (player 1)
and tunes them; `[input.bindings]` says which key or mouse input drives
each of the Duke's controls. `xbox_enhance_init` reads both through
`src/enhance/enhance_input.c` into `src/input` (`keyboard.c`, `mouse.c`).
The keys and mouse merge onto a pad on port 0: buttons are OR'd, the larger
analog value wins, and a stick the keys or mouse move replaces the pad's.
A title that never calls `xbox_enhance_init` keeps the compiled-in key
table of `src/input/README.md` and no mouse.

| Key | Type | Values | Default | Env override |
|---|---|---|---|---|
| `input.keyboard` | bool | the keys drive port 0 | `false` | `RECOMP_KEYBOARD` |
| `input.mouse` | bool | the mouse drives port 0: motion is a stick, buttons and the wheel as bound | `false` | `RECOMP_MOUSE` |
| `input.mouse_look` | string | `"right_stick"`, `"left_stick"`, `"none"`: the stick mouse motion drives | `"right_stick"` | none |
| `input.mouse_sensitivity` | float | `0.1` .. `10.0`; at `1.0`, 40 counts in one poll is full deflection | `1.0` | none |
| `input.mouse_invert_y` | bool | mouse forward looks down | `false` | none |
| `input.mouse_invert_x` | bool | | `false` | none |
| `input.mouse_min_deflection` | float | `0.0` .. `0.9`: any motion deflects the stick at least this much | `0.45` | none |
| `input.mouse_capture` | string | `"focus"`: the cursor is grabbed while the window has focus (the click that brings the window forward drives nothing: Win32 says which click that is, and on SDL and under a window manager that focuses first it is the first press within 250 ms of the focus); `"click"`: the first click in the window grabs it (and drives nothing); `"never"` | `"focus"` | none |
| `input.mouse_release_key` | string | a key name: frees the cursor, and grabs it again; `""` or `"none"`: no release key | `"F9"` | none |
| `input.wheel_pulse_ms` | int | `20` .. `2000`: how long one wheel notch holds its control | `100` | none |
| `input.stick_ramp_ms` | int | `0` .. `2000`: a stick key goes from 0.41 of full deflection to full over this time; `0` is full at once | `0` | none |
| `input.walk_key` | string | a key name: while held, the stick keys give `walk_scale` of full; `""` or `"none"`: no walk key | none | none |
| `input.walk_scale` | float | `0.41` .. `1.0` (outside is clamped) | `1.0` | none |
| `input.chord_delay_ms` | int | `0` .. `100`: a member of a host-action pad chord reaches the game this long after its press, so a chord completed inside the window never does ([input.actions], rule 3) | `0` | `RECOMP_INPUT_CHORD_DELAY_MS` |
| `[input.actions]` | table | an action name to a binding or a list of them; `[]` unbinds (below) | the actions' defaults | none |
| `[input.pad]` | table | a Duke control to the host pad sources that drive it (below) | the identity: the game sees what it sees with no file | none |
| `input.pad_deadzone_left` | int | `0` .. `95`: percent of full deflection; the stick is rescaled so it still reaches full. `0` keeps the raw stick | `0` | none (`RECOMP_PAD_DEADZONE` below) |
| `input.pad_deadzone_right` | int | as above, for the right stick | `0` | none |
| `input.pad_threshold` | int | `1` .. `100`: percent a trigger or stick direction must reach to press a digital control it is mapped to | `50` | none |
| `input.pad_invert_ly` | bool | the left stick's Y axis is inverted, before `left_stick`/`right_stick` swap | `false` | none |
| `input.pad_invert_ry` | bool | the right stick's Y axis is inverted | `false` | none |
| `input.pad_square_left` | bool | circle to square: the left stick is stretched along its angle so a full diagonal of a round gate reads about (1, 1), as on the Duke's square gate. The stretch blends in from 0.7 of full to full (smoothstep). Full diagonal movement in titles that run at the length of the stick (BLiNX). Applied last, after the deadzone, the swap and the merge of the alternate map (inversion comes before them, on the raw stick); cardinals unchanged. Also keeps two left-stick keys at full on each axis when `input.stick_ramp_ms` is above 0 (no 1/√2). Menu: Controls | `false` | `RECOMP_INPUT_PAD_SQUARE_LEFT` |
| `input.pad_square_right` | bool | as above, for the right stick | `false` | `RECOMP_INPUT_PAD_SQUARE_RIGHT` |
| `input.glyphs` | string | `"auto"`, `"xbox"`, `"playstation"`, `"nintendo"`, `"steam"`, `"generic"`, `"gamecube"`: the button family the overlay draws button glyphs for (`generic` is lettered, for a pad of no known family); `auto` asks the pad (below), and stays `xbox` on Windows and under Proton, where XInput cannot tell | `"auto"` | none |

These defaults are the layer's. The three stick knobs default to the
legacy keyboard (no ramp, no walk key), so a title that ships no `[input]`
keys keeps it; BLiNX 2's `config.default.toml` sets 60 ms, `LAlt` and
`0.5`. A title's `config.default.toml` also sets the two switches (BLiNX 2
ships both `true`), and `RECOMP_CONFIG=none` reads no
file, so both stay off unless the environment turns them on.
`RECOMP_KEYBOARD` and `RECOMP_MOUSE` predate the naming rule and keep their
names.

The floor of 0.41 is there because titles commonly ignore a stick below
about 0.4 of full deflection (BLiNX 2 does): a tap of a stick key or a
slow mouse motion that stayed under it would do nothing.

**Bindings.** Each key of `[input.bindings]` is a control, and its value
a string or a list of strings naming inputs:

```toml
[input.bindings]
a = "Space"
start = ["Enter", "Escape"]
b = "mouse1"
left_thumb = []             # unbound
```

Controls, by upstream's names (`src/input/input_map.c`): `dpad_up
dpad_down dpad_left dpad_right start back left_thumb right_thumb a b x y
black white left_trigger right_trigger left_stick_up left_stick_down
left_stick_left left_stick_right right_stick_up right_stick_down
right_stick_left right_stick_right`.

Inputs, in any case, by the label the host layout gives the key (the key
labelled Z is `Z` on AZERTY too): `A`..`Z`, `0`..`9`, `F1`..`F12`,
and upstream's key names (`xbox_InputMap_KeyFromName`, listed under
"Key names" in `src/input/README.md`): `Space`, `Enter` (or `Return`),
`Escape` (or `Esc`), `Backspace`, `Tab`, `Pause`, `CapsLock`, `LShift`
`RShift` `Shift`, `LCtrl` `RCtrl` `Ctrl` (or `Control`), `LAlt` `RAlt`
`Alt` (the unsided names match either side), `Up` `Down` `Left` `Right`,
`Insert` `Delete` `Home` `End` `PageUp` `PageDown`, `Numpad0`..`Numpad9`,
`NumpadAdd` `NumpadSubtract` `NumpadMultiply` `NumpadDivide`
`NumpadDecimal`, the OEM keys (`Comma` `Period` `Slash` `Semicolon`
`LeftBracket` `RightBracket` ...), and the mouse buttons `Mouse1`..`Mouse5`
(left, right, middle, back, forward). The fork adds `NumpadEnter`,
`WheelUp` and `WheelDown`. The Cmd and Windows keys are not bindable: they
belong to the system. The table lives in `src/input/input_names.c` over
upstream's `input_map.c`; `RECOMP_TRACE=key` prints each key's name as
it is pressed. A control holds up to 8 inputs.

**Resolution.**

- Per control, the highest file that names it wins whole: the player's
  `start = "Enter"` replaces the defaults' `["Enter", "Escape"]`, and
  `a = []` unbinds.
- An input a higher file names under one control is removed from every
  control a lower file gave it, so binding `Escape` to `back` in your file
  takes it off `start` without touching `start`.
- Inside one file, an input named under two controls is reported and the
  second ignored.
- A control no file names keeps the compiled-in table's keys, less any a
  file binds elsewhere.

**Errors** never stop the game and leave the rest of the file applied; each
is one line, with the file and the line of the key:

```
[INPUT] config error: config/config.toml:7: unknown control 'jmp' under [input.bindings] (controls: dpad_up dpad_down ... right_stick_right); ignored
[INPUT] config error: config/config.toml:8: unknown input 'Spacebar' in input.bindings.x (see docs/input.md); skipped
[INPUT] config error: config/config.toml:3: 'J' already bound to input.bindings.x on line 2; ignored here
[INPUT] config error: config/config.toml:9: input.bindings.a is a string or a list of strings (got integer); ignored
[INPUT] config error: config/config.toml:2: input.mouse_sensitivity 0 out of range 0.1 .. 10; using 1
[INPUT] config error: config/config.toml:3: input.mouse_look 'camera' is not one of right_stick, left_stick, none; using right_stick
[INPUT] config error: config/config.toml:5: input.stick_ramp_ms should be an integer (got string); using the default
```

`F9`, `F10` and `F11` are reserved (freeing the cursor, `menu.open`'s
default in `[input.actions]`, fullscreen); binding one is applied with a
warning in the same format, `... input.bindings.white 'F10' is menu.open's
default ([input.actions]); bound anyway`. A key bound here is then refused
for a host action (rule 2 of `[input.actions]`), so F10 bound to a control
leaves `menu.open` on its pad bindings.

The release key and the walk key act before the bindings, so a control
bound to either never sees it. That gets a warning, as does setting both to
the same key:

```
[INPUT] config warning: input.mouse_release_key 'F9' is also bound to input.bindings.y
[INPUT] config warning: input.walk_key and input.mouse_release_key are the same key
```

`RECOMP_KBM_SCRIPT` plays through the keyboard and mouse, so with both off
it is ignored, with `[INPUT] kbm_script ignored: keyboard and mouse off`.

**Log.** One line at init: the switches, the look target and sensitivity,
and how many controls each file decided, lowest file first:

```
[INPUT] config: keyboard=on mouse=on look=right_stick sens=1.0 bindings=24 (<exe dir>/config.default.toml) +1 (./config/config.toml)
[INPUT] config: keyboard=off mouse=off (RECOMP_CONFIG=none)
```

The `[ENHANCE]` lines do not mention `[input]`, and the golden runs, which
run under `RECOMP_CONFIG=none`, keep both off.

**The pad map (`[input.pad]`).** Which host pad button or axis drives each
of the Duke's controls. Without the table the game sees the identity, the
same pad it saw before the table existed: the face buttons by position,
White on the left shoulder and Black on the right, the triggers and both
sticks raw, no deadzone. `src/input/pad_map.c` holds that identity and
applies the map in `input_core.c`, under upstream's `PadMap`
(`src/input/input_map.c`).

```toml
[input.pad]
a = ["south", "paddle1"]    # two sources: the second is the alternate map
white = "right_shoulder"
black = "left_shoulder"
left_thumb = []             # unbound: the game never sees it
right_stick = "left"        # the physical left stick drives the Duke's right
```

Keys are the Duke's controls by the names of `[input.bindings]` (`dpad_up`
.. `right_trigger`; the eight stick directions are keyboard-only), plus
`left_stick` and `right_stick`, each `"left"`, `"right"` or `"none"`: which
physical stick drives it. A value is a host source, a list of up to two, or
`[]` / `"none"` to unbind. Sources, by upstream's names: `south east west
north` (the face buttons by position: an Xbox pad's A B X Y, a PlayStation
pad's cross circle square triangle, a Nintendo pad's B A Y X), `back guide
start left_stick_click right_stick_click left_shoulder right_shoulder
dpad_up dpad_down dpad_left dpad_right misc1 touchpad paddle1 .. paddle4
left_trigger right_trigger`, and the stick directions `left_stick_up` ..
`right_stick_right`. Upstream's names only: `a` or `lb` as a source is an
unknown name.

Resolution is that of `[input.bindings]`: per control, the highest file
that names it wins whole, and a source a higher file maps leaves every
control a lower file gave it, so `b = "left_shoulder"` in your file takes
the left shoulder off White. A source named under two controls in one file
is reported and the second ignored. Mapping `guide` is applied with a
warning, since most hosts keep Guide for themselves. A map changes which
sources the host actions may use (below).

`input.pad_deadzone_left` and `_right` are upstream's percent with its
rescale; `RECOMP_PAD_DEADZONE` keeps its integer meaning (0 .. 32767, the
stock path) and a file deadzone wins over it on that stick, with one line.
With a map in force, init logs it after the `[INPUT] config` line, by the
same per-file counts:

```
[INPUT] pad map: 3 (./config/config.toml) deadzone=10/0% invert=ry
[INPUT] config: input.pad_deadzone_left wins over RECOMP_PAD_DEADZONE=4000 on the left stick
```

**Pad family and Steam Input.** `xbox_InputPadFamily(slot)` says which
button family a pad has, for the overlay's glyphs: `xbox`, `playstation`,
`nintendo`, `steam` (the Deck's own pad, 28de:1205), `gamecube` (SDL 3.4's
GameCube type, whose A X B Y sit south, east, west, north) or `generic` (a
pad SDL cannot name). SDL builds take it from SDL's pad type and the vendor and
product; the XInput backend answers `xbox`. `input.glyphs` other than
`auto` overrides it for every pad. `xbox_InputSteamInput(slot)` says
whether Steam Input stands between the pad and the game: on SDL, `on` when
SDL gives the pad a Steam handle or it is Steam's virtual pad (28de:11ff),
else `off`; on XInput (Windows, Proton) it cannot tell, and answers
`maybe` when Steam launched the game (`SteamGameId` set, or `SteamDeck=1`,
in the environment, read through `recomp_env`), else `off`.

### Host actions (`[input.actions]`)

An action is something the host does rather than the game: open the menu,
toggle the stats strip, a mod's free camera. `src/enhance/enhance_actions.c`
binds actions to keys, to host pad sources and to chords of them, and
never gives the game a press it would not have had: it reads the raw host
pad, before `[input.pad]` maps it, beside the state the game gets, and only
ever takes presses out of that state (a consumed chord). Remapping the
game's own controls stays with `[input.bindings]` and `[input.pad]`.

```toml
[input.actions]
menu.open = ["F10", "guide+a", "paddle1", "back+left_thumb@400"]
camera.free.toggle = "F6"
overlay.stats = []                  # unbound
```

The action name is a bare dotted key, as above, or quoted: `"menu.open" =
[...]` is the same entry, which is what a TOML writer emits for it. A quoted
name must be the whole key directly under `[input.actions]` (or
`actions."menu.open"` under `[input]`); writing both spellings of one
action is a duplicate key. Because a bare `menu.open` is a table `menu` in
TOML, no action is named after another plus a part (`menu.open` and
`menu.open.all`); the registry refuses the second. Per action, the highest file that names it decides all of its
bindings, as for a control under `[input.bindings]`; an action no file
names keeps its defaults. An action holds up to 8 bindings.

The toolkit's actions:

| Action | Default bindings | Does |
|---|---|---|
| `menu.open` | `F10`, `guide+a`, `paddle1` | opens and closes the menu when `menu.enabled` is on |
| `overlay.stats` | none | toggles the stats strip |
| `shot.window` | none | a window shot (a log line for now) |

A game's `config.default.toml` adds to these, for example a fallback chord
for hosts where Guide is not seen (BLiNX 2: `back+left_thumb@400`). A mod
registers its own actions, named after the mod (`camera.free.toggle`).

**A binding** is one of:

- **A key**, by the names of `[input.bindings]`, optionally after
  modifiers: `F10`, `ctrl+F6`. A plain key fires only with no modifier
  held, and `ctrl+F6` only with Ctrl and no other modifier. Mouse inputs
  are not bindings.
- **A host source** alone, by the source names of `[input.pad]`:
  `guide`, `misc1` (share, capture), `paddle1` .. `paddle4` (upper right,
  upper left, lower right, lower left: the Elite's P1, P3, P2, P4, the
  Deck's R4, L4, R5, L5, and the DualSense Edge's right back button, left
  back button, right Fn button and left Fn button), `touchpad`, or any
  other source the pad map does not give the game. The identity gives the
  game none of the first ones, so by default the game never sees them.
- **A pad chord**: two to eight sources joined by `+`, with an optional
  hold `@<ms>` (`0` .. `2000`). It is read on port 0 (player 1) unless it
  starts with `pad2:` .. `pad4:`. A Duke control's name (`a b x y black
  white left_trigger right_trigger start back left_thumb right_thumb
  dpad_up` ..) in a binding means the source the identity gives it (`a` is
  `south`, `white` is `left_shoulder`, `left_thumb` is
  `left_stick_click`), so a remap in `[input.pad]` never moves a binding:
  `guide+a` stays Guide plus the south button whatever the map does with
  them. Names are lower case.

**The kind of a pad binding** follows from which of its sources the pad
map gives the game (either map, and every direction of a stick a map
uses), and is worked out again whenever the map changes:

| Sources the game uses | Kind | Fires |
|---|---|---|
| none, one source | host-only | on its press |
| none, a chord | a host-only chord | when all are down, after the hold |
| some | a modifier chord: the unused ones are the modifiers | rule 7 |
| all, a chord with `@200` or more | a guest chord | rules 1 and 3 |
| a single source, or all of a chord with a shorter hold | inactive, with a line | never |

**Rules**, so a binding never takes what the game uses. A refused binding
gets one line and the rest of the action's bindings stand:

1. A single source the game uses (`a`, `start` under the identity) is
   inactive. A chord whose sources the game all uses needs a hold of at
   least 200 ms, or it is inactive.
2. A key the game receives is refused, and the game keeps it. The check
   is against the effective key map: the compiled-in table, then
   `[input.bindings]`, `input.walk_key` and `input.mouse_release_key`.
   Modifiers count: Ctrl and Shift drive the game in the compiled-in
   table, so `ctrl+F6` is refused unless the player unbinds them.
3. A chord is consumed. From the moment all of its buttons are down, they
   reach the game released until each is let go; a stick direction
   (`left_stick_up`) reaches it centred on that half of the axis, so the
   other way still passes. A chord let go before
   its hold fires nothing. The game does still see the first button's
   press, and masking then gives it a release the player did not make:
   with `back+start`, START pauses BLiNX 2 and is then released. So pick
   members the game ignores on a lone press (a button with no use plus
   one used only in menus, as `back+left_thumb` in BLiNX 2), or set
   `input.chord_delay_ms`, which holds each chord member back from the
   game for that long after its press. That costs the same latency on
   those buttons, which is why it is off by default.
4. The defaults use only F-keys, sources the identity leaves to the host,
   modifier chords and chords with a hold.
5. A scripted or replayed pad (a game harness's) never fires an action:
   actions read the host pad, which the script does not touch. A key from
   `RECOMP_KBM_SCRIPT` does, because it goes through the same key table
   as a real key; that is how an unattended run drives the free camera.
6. Only a feature key arms the actions: `menu.enabled`, `overlay.stats`,
   or a mod's enable key (`camera.free`). Registering an action never
   does. Disarmed, nothing is read or masked. The start line says which:

   ```
   [ENHANCE] actions hook=off
   [ENHANCE] actions hook=on (menu.enabled, camera.free)
   ```

   A golden run (`RECOMP_CONFIG=none`) has `hook=off`. The overlay's
   stats and toast layers are armed with the hook and only then: stock
   bakes no font, and a mod's toast is then its log line only.
7. A modifier chord (`guide+a`) fires on the press of its last member
   the game uses while all its modifiers (the members the game does not
   use) are held; a hold `@N` is kept, counted from that press. Order in
   the text does not matter (`left_thumb+guide` is `guide+left_thumb`),
   and a chord may have several modifiers (`guide+paddle1+a`). While its
   modifiers are held, the used members of every modifier chord on that
   port reach the game released from their press until they are let go,
   even when a modifier is let go first, so the game never sees the A of
   `guide+a`. A used member pressed before the modifiers goes to the game
   and does not complete the chord. The button that closes the menu stays
   masked after the close until it is let go.
8. One input fires one action. Two bindings take the same input when they
   are the same key with the same modifiers (`LAlt` and `RAlt` are two,
   `Alt` matches either), or the same pad sources on the same port
   whatever their holds. Of two such bindings, `menu.open`'s always keeps
   the input, since the menu is the one place to repair a binding and on
   the Deck under Proton F10 is the one way into it. Of any other two,
   one written in a file or the menu beats a default, and otherwise the
   action registered first keeps it (the toolkit's before the mods'); the
   other is dropped with a line. So `overlay.stats = "F10"` leaves F10 on
   `menu.open`:

   ```
   [INPUT] action overlay.stats: 'F10' refused: menu.open has it ('F10'); one input fires one action
   ```

   To move F10 to another action, give `menu.open` other bindings first
   (`menu.open = ["guide+a", "paddle1"]`). A written binding that takes
   the default of an action registered before it logs the default's side.
   With a mod `zoom` whose `zoom.toggle` defaults to F6, registered before
   the camera mod, `"camera.toggle" = "F6"` gives:

   ```
   [INPUT] action zoom.toggle (default): 'F6' gives way to camera.toggle ('F6')
   ```

   The menu's capture refuses such a binding outright, naming the owner
   (`F10 refused: Open the menu has it (F10); clear it there first`). A
   key and a pad source are never one binding: a Steam Input grip mapped
   to F10 stays a key.
9. The longest match fires. A pad binding whose sources all belong to a
   longer binding on the same port (`guide` beside `guide+a`) waits: it
   fires when it is let go after its hold, and only if no longer binding
   was held whole meanwhile, whether or not that one fired. So holding
   `back+start` past its hold, adding `left_thumb` and letting all go
   before `back+start+left_thumb@400`'s hold fires neither: one press,
   one action. Such a binding never reads as held down. A binding with no
   longer one beside it fires on its press as before.
10. Nothing fires while the menu captures a binding, nor on the first
    keyboard step after it ends, which the key that ended it may reach
    first. The keys
    and buttons held during a capture stay spent until they are let go,
    so the press being captured never fires its old action.

The game may stop polling the pad (a loading screen). The flip then reads
the host pad itself when a port has not been polled for 100 ms, so keys,
host-only buttons and modifier chords still fire there. Plain chords of
guest buttons fire only from a poll.

**Errors and the log.** Each refusal or inactive binding names the action,
the file and the line (or `(default)` for a default), and the binding. An
inactive binding names the sources the game has and through which
controls; it stays in the table and comes back if the map frees them:

```
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'a' inactive: south (the game's a) goes to the game; a single source the game uses would take it from the game; use a chord of two with a hold (back+left_thumb@400) or a source the game does not use (guide, paddle1)
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'back+left_thumb' inactive: every member goes to the game (back (the game's back), left_stick_click (the game's left_thumb)); such a chord needs a hold of at least 200 ms (@400)
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'back+lthumb@400' refused: unknown name 'lthumb'
[INPUT] action input.actions.menu.open (./config/config.toml:3): 'Z' refused: 'Z' is bound to the guest's a (input.bindings); the game keeps it
[INPUT] action input.actions.menu.open (./config/config.toml:3): more than 8 bindings; the rest skipped
[INPUT] config warning: ./config/config.toml:4: no action named 'menu.opne'
[INPUT] config warning: input.chord_delay_ms=500 out of 0..100; using 100
```

When a map change (a reload) changes a binding's kind, one line says so:

```
[INPUT] action menu.open (default): 'paddle1' inactive: paddle1 (the game's x) goes to the game; ... (was host-only)
[INPUT] action menu.open (default): 'paddle1' is now host-only (was inactive)
```

Every fire is logged, at most 10 lines a second:

```
[ACTION] menu.open via guide+a
[ACTION] 2 more fires not logged
```

**Guide per host.** `guide+a` needs the host to hand the Guide button to
the game. Where it does not, F10 and the game's fallback chord still work.

| Host | Backend | Guide seen? | What to do |
|---|---|---|---|
| macOS | SDL3 (GameController or HIDAPI) | Usually. macOS may keep the Home button: System Settings > Game Controllers > the controller, then let apps use the Home button. | the setting; F10; the fallback chord |
| Linux desktop, native | SDL3 (evdev or HIDAPI) | Yes, unless Steam runs with Steam Input on for the pad, which keeps Guide for its overlay | Steam Input off for the title; F10; the fallback chord |
| Steam Deck | XInput through Steam's virtual pad (Proton), or SDL3 | No: SteamOS keeps the Steam button and its chords for itself | R4: bound to F10 in Steam Input under Proton (below), `paddle1` where SDL3 reads the pad; the fallback chord |
| Windows, native | XInput (`XInputGetStateEx`) | Yes, unless the Xbox Game Bar takes the Xbox button: Windows 11: Settings > Gaming > Game Bar, "Allow your controller to open Game Bar", off | the setting; F10; the fallback chord |
| Windows build under Proton (desktop Linux) | XInput through Wine | Only with Steam Input off for the title | Steam Input off; F10; the fallback chord |

Only Guide reaches a Windows build: XInput has no paddles, share button or
touchpad. Whether Steam Input is in between, as `xbox_InputSteamInput`
reports it (above): SDL knows (`on` or `off`); XInput says `maybe` when
Steam launched the game. A chord with Guide stays incomplete where Guide is not seen, and
its guest member then reaches the game as usual.

**The Steam Deck and Steam Input.** On the Deck the menu goes on R4, the
upper right back grip. SteamOS keeps the Steam button and its chords for
itself, so `guide+a` never reaches the game there. Under Proton, Steam Input is on by default and the game
sees Steam's virtual pad, an XInput pad with no grips (L4, L5, R4, R5).
So bind R4 to a key once, in Steam Input:

1. With the game selected in the library (or from the in-game Steam
   menu), open the controller settings: the controller icon, then *Edit
   Layout* (*Controller Settings* in Desktop mode).
2. Pick *Back Grip Buttons*, then *R4*, then *Keyboard* and the key
   `F10`.
3. Back out; Steam saves the layout for that game.

R4 then sends F10, which fires `menu.open` like a real key (the log shows
`[ACTION] menu.open via F10`); a second press closes the menu. Another
grip can carry another action's key the same way, for example R5 to `F6`
for BLiNX 2's free camera. Leave L4 and L5 unbound for now: they are kept
for quick save and quick load (F5 and F9), and save states have not
landed yet. F9 frees the cursor today (`input.mouse_release_key`).

A build that reads the pad through SDL3 sees R4 as `paddle1`, which
`menu.open` has by default, so there it needs no Steam Input binding.
That will be a native Linux build, once there is one, with Steam Input
off for the title (SDL's HIDAPI Steam Deck driver). Today the SDL3 case
is macOS, where an Elite's P1 opens the menu by default. An Elite's
paddles and the DualSense Edge's back and Fn buttons show as `paddle1`
.. `paddle4` only where the firmware and SDL report them. An Elite
profile that copies a face button reports the face button instead,
which is a guest button.

### In-game menu (menu.enabled)

`menu.enabled = true` (or `RECOMP_MENU=1`) arms a menu drawn by the
overlay over the game. Off, nothing of it runs: the start log says
`[ENHANCE] menu.enabled=off` and the `menu.open` action is disarmed with
the rest of the hook unless another feature key arms it. On, the log names
the bindings that open it:

```
[ENHANCE] menu.enabled=on (F10, guide+a, paddle1)
```

`menu.open` opens and closes it. While it is open the game keeps running
(there is no pause) but reads a released pad with a frozen packet number,
so it sees no input at all; the keys go to the menu. A button still held
as the menu closes reaches the game only after it is let go, for at most
a second after the close.

| Pad | Keyboard | Does |
|---|---|---|
| D-pad, left stick | arrows | move; left and right change the value. Held: a repeat after 150 ms, then every 60 ms. Down past the last row moves to the footer's buttons (on Save when something is unsaved, else Cancel); Up from the first row stays |
| A | Enter, Space | toggle, enter a list, start a capture, run an action, press the focused footer button |
| B | Esc, Backspace | leave a list or a capture. At a section's top it moves to the section's entry in the toolbar and a second press closes (asking first when something is unsaved) |
| | Tab, Shift+Tab | the focus round the toolbar, the rows and the footer |
| LB, RB | Q, E, PgUp, PgDn | the previous or next page |
| Y | F5, Ctrl+S, Cmd+S | Save |
| X | Delete | clear an action's bindings |
| View (on release) | Shift+Esc | Cancel: close, asking first when something is unsaved |
| L3 (on release) | F1 | the hints: tooltip or inline (`overlay.menu_hints`) |
| R3 (on release) | | the size: compact or expanded (`overlay.menu_size`) |

In the toolbar, Up and Down (or LB and RB) walk the sections and A or
Right enters one; in the footer, Left and Right pick Cancel or Save and
Up or B goes back to the row. The global buttons (Y, View, L3, R3, LB,
RB) do the same everywhere; no page binds them.

The mouse works the menu while it is open: the cursor is freed for it
(a mod holding the mouse gets no motion until the menu closes) and hides
on a pad or key press until the mouse moves again. Moving the pointer
over a row selects it, but only real motion does: a mouse resting on the
panel never takes the selection from the pad or the keys. A click acts
on what it hits: a switch flips, the arrows step a choice, a slider
jumps to the point and follows a drag, a row's label is its A, and the
header buttons, the footer buttons, the toolbar's entries and the
dialog's buttons press. The wheel over a slider or a choice steps its
value, one step a notch, applied as the arrows apply it; anywhere else
it scrolls the rows without moving the selection, and the next key or
pad press brings the selection back into view. The right button is B
(Go Back in the dialog). Clicks on the dimmed game or outside the panel
do nothing. Under `present.surface = internal` the pointer maps through
the presenter's scaling (Metal's letterbox, D3D11's stretch to the
window), so a click lands on what is drawn under it.

Renaming a shader profile edits the name in place: a keyboard types in
its own layout (the window's text input, so dead keys and IMEs work),
with Left, Right, Home, End, Backspace and Delete, Enter to keep it and
Esc to cancel. When the pad was the last input, or the mouse or a
touch started the rename and no key has been pressed this session, an
on-screen keyboard opens under the field: the d-pad moves, A types the focused key, X
deletes, Start or Done keeps the name, B cancels, and LB and RB move
the text cursor. Shift gives one capital. Under Steam (`SteamGameId`, or
`SteamDeck=1`) the footer also names Steam + X, Steam's own keyboard. The mouse clicks
its keys, and the right button cancels.

`RECOMP_TRACE=menu_input` prints the menu's input as it happens: the
device each event came from, focus moves, pointer hits and clicks, the
tooltip's shows and the text field's input.

The menu comes in two sizes. Compact is a panel down the window's left,
the game in view beside it; expanded fills the window over the game
dimmed. The header names the game (the window title), and its buttons
are the hints, the size and close. The sections sit in a toolbar down
the panel's left: icons while the rows have the focus, unfolded with
their names when the toolbar has it, drawn over the rows' left edge so
the rows do not move. LB and RB unfold it for 1.5 s to show where they
went. The footer holds the selected row's help, the unsaved count, and
Cancel and Save with the input that fires them (View and Y on a pad,
Shift+Esc and F5 on the keyboard). Everything scales with the
automatic scale times `overlay.ui_scale` ([Overlay scale](#overlay-scale)).

<a id="overlay-scale"></a>
**Overlay scale.** The theme's sizes are set for 1080 lines; the
automatic scale is `max(clamp(lines / 1080, 0.5, 3), floor)`, then times
`overlay.ui_scale` and clamped to 0.5-3. `lines` is the height the
overlay shows at on screen, not the frame's: the overlay is placed on the
whole back buffer or drawable (never the frame's rectangle in it), and its
height is taken through the window's stretch (`present.surface =
internal`) and, under gamescope, through the compositor's fit of the
window to the screen. So `render.scale`, `present.filter` and
`present.shader` change neither the overlay's size nor its hit areas.
Under `present.surface = internal` on Windows the window is
held to the screen (aspect kept) and the larger back buffer is stretched
into it, so a pointer or touch reaches every part of the menu. Under `overlay.ui_fit = "display"`
the floor is the primary display's, probed once:
the OS's UI scale (DPI over 96 on Windows; SDL3's pixel density times
content scale elsewhere) and, on a handheld, its density over 160 ppi.
The one handheld known is the Steam Deck's own panel (`SteamDeck=1`, or
the SMBIOS names Valve Jupiter or Galileo since Steam sets `SteamDeck`
only in Game Mode, and a 1280x800 display; docked to a TV it is a desktop): 1280x800 at 7" is
216 ppi, a floor of 1.35. An explicit `SteamDeck=0` does not override the
SMBIOS names (a Desktop Mode run on the Deck may carry it); `ui_fit =
"output"` turns the floor off (the height shown, through the window's
stretch and gamescope's fit, still sets the scale). The floor is in
display pixels and is held to `lines / 600` so a small window keeps 600 token lines. The atlas
is baked for the display's height (the size a fullscreen menu draws at);
past 2.0 it keeps to the output's own scale, and it never passes 3, the
largest scale, at which `test_fit` proves the atlas fits 2048x2048 with
the symbols. Only the primary display is probed: a window on another
monitor keeps the primary's floor, and a Deck docked to a 1280x800
external screen counts as its own handheld panel. Over the output height's own scale the compact panel
keeps 500 token columns (up to 3/4 of the width), and a pad glyph in
small text is drawn at the body size so its letter or shape stays
legible. Both hold under `ui_fit = "output"` too: the wider panel when
`overlay.ui_scale` lifts the scale over the output height's, the glyphs
always (a look change, not a size rule). Under 1080 lines a desktop
window grows as well: a 1280x720 window on a 100% desktop was 0.667 and
is 1.0, the OS's scale, unless `ui_fit = "output"`. On Windows the exe is
not DPI aware (a GDI window, no manifest entry), so the probe reads the
virtualised 96 dpi and scaled size and DWM stretches the window, menu
included, by the desktop's scale; the OS term is 1 there. A change of either key in the menu bakes the atlas again on the
presenting thread's next poll, under the menu's lock, and every layer
composes again (a list composed from the old atlas is not drawn). The probe and
its `[OVERLAY] display` log line run under either `ui_fit` (a live switch to
`"display"` needs the floor, and gamescope's fit needs the display's size),
at the first atlas bake on Windows (cheap GDI calls); with SDL it waits
for the presenter's own `SDL_Init` and never starts the video itself, so
the atlas is baked again then (between frames when a layer already drew). A headless run,
or one whose window does not open, never probes. Tests:
`tests/overlay` (`test_display_scale`), `tests/enhance_menu_layout`.

The pages are Render, Shaders, Pack, Game (a title's own items, from
`enhance_menu_add`), Mods, Stats, Controls, Debug and Save.

- **Controls** lists every action with its bindings, grouped by owner
  under a header: Toolkit first, then Game (actions a title registered),
  then one group per mod under its title. A mod that is off has
  "(mod off)" on its header; its bindings still hold their inputs, so
  enabling it moves nothing. A mod's captured controls (BUTTON, STICK)
  are on its own page only. A on a row captures a
  new one: the next key, or the pad buttons held together for 400 ms (one
  button, a two-button chord, or a modifier chord such as `guide+a`). It
  is checked by the rules above: a key or a single button the game uses is
  refused with the reason in the footer, as at load. The binding is live
  at once and is added to the row's list; X clears the list (`menu.open`
  always keeps one). An input another action holds is refused with its
  owner and label ("F6 refused: Free camera: Toggle flight has it (F6)
  (mod off); clear it there first"), a mod that is off included, and so
  is a key a mod's captured control names ("R refused: Free camera: Rise
  uses it"). Esc or 10 s without input cancels. A Sticks group
  follows the actions: `input.pad_square_left` and `_right` as toggles,
  live at once. A Mouse group ends the page: `input.mouse_capture` as a
  choice, the release key and the free keys as information, and the
  capture state now (`captured`, `free: click to capture`, `free: F9`).
- **Mods** has one row per mod, under its descriptor title (else its name):
  its switch (left and right), and, entered
  with A, the mod's page. A mod that gave `on_enable` turns off and on at
  once; one that did not shows "applies at restart". Turning a mod off
  stops nothing by itself: its frame hooks and armed actions keep
  running, and the mod gates them while off. The HUD table's owner is
  the exception: its hooks follow the state the layer took at the last
  frame, so on the game thread a switch lands whole at the next frame
  (the render thread reads the layer's aspect state when it executes the
  pushbuffer, so a render thread running behind can show the edge HUD
  clipped for a frame or two). `RECOMP_DEBUG=mod_set_at=<frame>:<key>=<value>,...` makes the
  same change from the flip tick once the guest has run that many
  `xbox_mod_frame` calls (the menu must be armed), to shoot a live switch
  without the menu: `mod_set_at=900:mods.hud.enable=off,1200:mods.hud.enable=on`.
  The page lists, top down:
  - the title and `v<version>`, then Author, About (the summary,
    wrapped to the panel at any width) and State: `On`, `Off`,
    `Off (mods."<id>" = false)` when that key holds it off, `Failed:
    <reason>`, or `On at restart` / `Off at restart` for a change waiting
    for the next start;
  - Enabled, the same switch as the list's. Turning it on with
    `mods."<id>" = false` in the file stages removing that key;
  - for a v3 mod, **Controls**: an action as on the Controls page (its
    help in the footer, "(mod off)" after its bindings while the mod is
    off); a BUTTON or STICK control with its bindings and the keys behind
    them (`rt, R (keys R)`), its `when` note dimmed, rebound by capture
    (below); an INFO control as text;
  - **Settings**, one row per descriptor setting: a toggle, a choice or a
    slider by `step`, with its `unit` after the value and its live class
    (below). A row an environment variable forces is dimmed with "set by
    `RECOMP_...`" and never saved. A link setting's row is its toolkit
    item's own row (below);
  - the items the mod added with `menu_add` (a v2 mod has only these);
  - **Reset this mod**: stages the mod's settings, action bindings and
    captured controls back to the stock: the defaults file's value, else the descriptor's
    default (Save writes it, which removes the keys). Enabled is not
    reset, and other mods and unknown keys are left alone;
  - **Unknown keys**: the mod's keys nothing read, "not read; kept".

  A mod that failed lists its controls and settings as "unavailable" and
  has no Reset. The footer shows the selected row's help whenever no
  message is showing.
- **Pack** shows the pack loaded, its toggles and the last misses, a copy
  of the miss ring taken at the flip while the page is shown.
- **Stats** and **Debug** show live numbers: frame rate and time, the
  presenter, the host-option generation, the extra buttons seen in the last
  second, the atlas, and the `RECOMP_TRACE` channels (read only: a trace
  is set at start).
- **Save** writes the changes to the player's file
  (`enhance_cfg_user_path()`, the `config/config.toml` above) and nothing
  else: a key whose value equals `config.default.toml`'s, or the code
  default when that file has none, is removed instead. The file's other
  keys and its comments stay, and so do the comments between a binding
  list's items while the list keeps its length; a list that grows or
  shrinks is written on one line and loses the comments inside it. A
  comment line right above a removed key stays (it may be the table's).
  The file is flushed to the disk before it replaces the old one, and a
  `config.toml` that is a symlink is written at its target. The log says
  `[ENHANCE] menu: saved 2 keys to ./config/config.toml`. The run's
  loaded copy of the file follows the Save: later lookups in the run, and
  a reload (`enhance_input_reload`), read the saved values, not the ones
  from the start; a key an environment variable forces keeps the forced
  value.
  Under `RECOMP_CONFIG=none` Save is off. "Reset to defaults" asks once, then
  puts every item and binding back to its default; Save keeps that. A
  mod setting or action read from a retired key in the player's file is
  moved to its current key by the first Save, even one with no change.

A value forced by an environment variable shows "set by `RECOMP_...`"
(or "(environment)" when the row has no variable name) and is read only;
changing it says so in the footer. Each item takes effect at one of five
points, its live class; the footer names the selected row's:

| Class | Takes effect | Items |
|---|---|---|
| live | at the next flip | `render.aniso`, `render.snap_2d`, `present.filter`, `present.shader`, `present.pacing`, `display.overscan`, `display.movie`, `display.hud_margin` (listed while a HUD mod has registered a table; a table only the capture loaded does not count), `overlay.stats`, `overlay.ui_scale`, `overlay.ui_fit`, `input.mouse_capture`, every action binding, a mod with `on_enable` |
| at the flip | at the next flip, rebuilding the targets or the texture cache | `render.scale`, `present.fullscreen`, `textures.pack`, `textures.font`, `textures.text`, `textures.world` |
| next stage | at a later game point (a title's item) | none of the toolkit's |
| next activation | when the mod next turns its feature on | a mod setting the descriptor marks so (the free camera's HUD choice) |
| at restart | at the next start; the change is saved, not applied | `display.aspect`, `present.surface`, a mod without `on_enable` |

The `textures.pack` toggle turns the loaded pack off for this session only
and is never saved (the key names the pack's file). A title's items and a
mod's items give their own class. Tests: `tests/enhance_menu` (ctest
`enhance_menu`).

#### Import and export (the Files page)

The Files page browses folders inside the overlay: Exports
(`<user dir>/exports`), Config, Shader profiles, Home and, on Windows, each
drive. Only folders and `.toml` files are listed, and a link that leads out
of its place cannot be opened. Under Proton no drive roots are added
(Wine's drives are its own prefix), and Home is the Unix home as Wine maps
it, `Z:\home\<user>`. A folder that cannot be read says so; there is no
system file picker.

- **Export settings** writes the config as Save would write it now,
  unsaved values included, to a name you type (the default is
  `settings-<yyyymmdd-hhmm>.toml`). The first lines say what the file is
  and which values were unsaved:

  ```
  # xbr-export: config 1
  # xbr-export: 1 unsaved value included: render.scale
  ```

  A name Windows or another host would refuse is refused on every host.
  An existing file asks before it is replaced, with the focus on No. The
  write goes to a temporary file and is renamed into place, and the
  footer names the path.
- **Export shader profile** writes the active profile's file as last
  saved. Only a saved profile exports: a bundled preset or a path is a
  config key and goes out with the settings.
- **Import** reads a `.toml` file of at most 256 KB and shows what it
  would do before anything changes: the settings it changes, the keys
  it keeps as written (keys no setting here takes, and keys of another
  page such as `present.shader` or `[input.controls]`), and the keys it skips (set
  by the environment, or with a value the setting does not take). A file
  that does not parse, an export in a newer format or an export of
  something else is refused, and nothing changes. **Apply** makes the
  changes unsaved: Save keeps them, and Exit without saving drops them,
  the kept keys with them. A kept key takes effect at the next start;
  `menu.enabled = false` is one, so an import never closes the open menu.
  A file with `stages` is a shader profile: Apply copies it into
  `shader-profiles/` under its name's slug, or the first free name after
  it (`<slug>-2.toml` ...), and never replaces a profile.
- **Open exports folder** shows the folder in the desktop's file manager
  (`ShellExecuteW` on Windows, which under Proton opens Wine's explorer;
  `SDL_OpenURL` elsewhere). It is not shown
  under Steam Game Mode (`RECOMP_HOST_GAMESCOPE=1`), which has no file
  manager.

With `RECOMP_CONFIG=none` there is no user folder, so import and export
are off.

#### Saves (the Saves page)

The Saves page lists the running title's saves (`UDATA/<title id>` on the
emulated hard disk; each save by the `Name=` in its `SaveMeta.xbx`, with
its size and date) and the host folder they live in, and moves them
between computers as one `.zip`. It shares the Files page's browser,
listing folders and `.zip` files, and the same name rules. Until the XBE
has loaded the title is not known, and the rows say so.

A saves zip is stored (not compressed), so any unzip tool opens it:

```
xbr-saves.toml           format, kind = "saves", title_id, title, created,
                         keys, files, bytes, toolkit
hdd/UDATA/<tid>/...      the saves
hdd/TDATA/<tid>/...      the title's own data, when it has any
```

- **Export saves** writes both trees to a name you type (the default is
  `<title slug>-saves-<yyyymmdd-hhmm>.zip`), through a temporary file and
  a rename, as the Files exports are.
- **Import saves** reads the zip whole (at most 64 MB) and checks it
  before showing anything: the structure, every name (the FATX rules:
  42 bytes a component, no `..`, no device names, at most 8 deep), the
  sizes (16 MB a file, 64 MB in all, 4096 entries), each CRC, and the
  manifest. A zip for another title, in a newer format, or made with other
  console keys is refused, and nothing changes. The preview names the
  file, the game, when it was exported, the saves in it, and what it
  replaces.
- **Import and quit** first exports the saves here to
  `<user dir>/exports/save-backups/<title slug>-saves-backup-<stamp>.zip`
  (a failed backup stops the import), then writes the zip's trees beside
  the live ones, each in a folder of its own
  (`.saves-import-<stamp>-<pid>-<n>-<TREE>` in the hdd folder), renames
  each live tree aside and the new one in, and removes the old ones last.
  A failed rename puts back every tree already swapped. If putting one
  back fails too, the message says so and names the folder that keeps
  that tree's old saves, which is left in place. On Windows a folder the
  game holds open does not move; the message then says to quit to the
  title menu and try again. A tree the zip does not
  carry (often TDATA) is kept as it is. The game is then asked to quit,
  since it still holds the old saves in memory and would write them back;
  with no window to take the quit (a headless run) the page says to quit
  without saving.

**Keys.** The saves are signed with the console's keys, which this runtime
leaves at zero, so a save moves between hosts as it is and is never
re-signed. The manifest's `keys` is `zero`, or a fingerprint of the keys
in force (`xbox_saves_keys_id`); an import needs the same value.

**A save in progress.** The kernel keeps a table of open handles under a
title's save trees (`xbox_saves_busy`, `kernel_file.c`): export and
import refuse while one is open for a file or for 2 seconds after the
last create, write, rename, delete or close there ("The game is saving;
try again in a moment"). The game is not paused while the page copies,
so every such touch also moves a generation (`xbox_saves_gen`): the page
reads it before the export (or the backup) and again after it, and just
before the import's first rename, and drops the copy when it moved. A
handle `NtDuplicateObject` closes with `DUPLICATE_CLOSE_SOURCE` leaves
the table too.

Tests: `tests/saves_zip` (the zip, the manifest, the checks, the export,
the swap, its rollback with renames failed on purpose, and a failed
undo), `tests/kernel_directory_posix` (the busy table and the
generation), `tests/enhance_menu` (the page).

### Naming rule

- Tables group keys by feature: `[render]`, `[present]`, `[display]`, and a title's own.
  Key names are lower case, with `_` between words.
- A key that has an environment override uses the variable `RECOMP_` + the
  dotted key in upper case with `.` replaced by `_`. For example,
  `render.scale` becomes `RECOMP_RENDER_SCALE`.
- A choice is a string from a fixed list, never a number code.

## Environment

The layer's variables belong to the **config** tier of `recomp_env.h`. They
exist only when the layer is built (`RECOMP_ENV_HAVE_ENHANCE_KEYS`):

| Variable | Meaning |
|---|---|
| `RECOMP_CONFIG` | path of the player's file, which replaces both `config.toml` files, or `none` for no file at all (the defaults included). `RECOMP_ENHANCE_CONFIG` is its earlier name, read with a deprecation line |
| `RECOMP_RENDER_SCALE` | overrides `render.scale` |
| `RECOMP_RENDER_SNAP_2D` | overrides `render.snap_2d` |
| `RECOMP_DISPLAY_ASPECT` | overrides `display.aspect` |
| `RECOMP_DISPLAY_MOVIE` | overrides `display.movie` |
| `RECOMP_DISPLAY_OVERSCAN` | overrides `display.overscan` |
| `RECOMP_DISPLAY_HUD_ASPECT` | overrides `display.hud_aspect` |
| `RECOMP_DISPLAY_HUD_MARGIN` | overrides `display.hud_margin` |
| `RECOMP_PRESENT_FILTER` | overrides `present.filter` |
| `RECOMP_PRESENT_FULLSCREEN` | overrides `present.fullscreen` |
| `RECOMP_PRESENT_PACING` | overrides `present.pacing` |
| `RECOMP_PRESENT_SURFACE` | overrides `present.surface` |
| `RECOMP_HOST_GAMESCOPE` | set by the SteamOS launcher (`1`) when it runs under gamescope; `present.surface = "auto"` then means `internal`. No file key binds it; a line in the bundle's `config/launch.env` overrides it |
| `RECOMP_TEXTURES_PACK` | overrides `textures.pack` |
| `RECOMP_TEXTURES_FONT` | overrides `textures.font` |
| `RECOMP_TEXTURES_TEXT` | overrides `textures.text` |
| `RECOMP_TEXTURES_WORLD` | overrides `textures.world` |
| `RECOMP_OVERLAY_STATS` | overrides `overlay.stats` |
| `RECOMP_MENU` | overrides `menu.enabled` |
| `RECOMP_KEYBOARD` | overrides `input.keyboard` (an older name, outside the naming rule) |
| `RECOMP_MOUSE` | overrides `input.mouse` |
| `RECOMP_STORAGE_RAM_CACHE` | overrides `storage.ram_cache`; a row of the main table, so the kernel reads it without the layer too: unset or empty is on, `0`, `false`, `no` or `off` is off, and any other value is warned about and ignored. With the store used, the `[MEM] exit` line adds `ramcache_mib` and `ramcache_peak_mib`: its FATX charge (sizes in 16 KB clusters, seed included, what the 750 MB cap counts) at exit and at its peak. That is not its host memory, which `ram_mib` covers |
| `RECOMP_SYSTEM_LANGUAGE` | overrides `system.language`; a row of the main table, so the kernel reads it without the layer too (at the first `XC_LANGUAGE` query). Unset or empty is English; codes are lower case, and any other value gives English with one log line |
| `RECOMP_MODS_DESCRIBE` | `json` writes the mods' descriptor dump and exits before the game starts (see "Mods", "The descriptor dump"). Any other value logs one `[MOD]` line and is ignored |
| `RECOMP_MODS_DESCRIBE_OUT` | the file the dump replaces; unset, it goes to stdout. A Windows or Proton launcher needs it, since the game has no stdout there |

The layer converts an environment value to the type of the key:

- Integers are read like `strtoll(v, 0, 0)`, so `0x2` works.
- Booleans accept `1/0`, `true/false`, `yes/no` and `on/off`, in any case.
- A choice must be one of the listed strings.

An empty value counts as unset. A value that does not convert is reported once
and ignored, so the file decides.

The env-key names follow the naming rule. Design D6 used
`RECOMP_RENDER_ASPECT`; it became `RECOMP_DISPLAY_ASPECT` because the key is
`display.aspect`.

## API

There are two layers. Both are C11 and need only libc.

`recomp_cfg.h` is the reader. A title can use it on its own for any file:

```c
char err[256];
recomp_cfg *c = recomp_cfg_load_file("config.toml", err, sizeof err);   /* NULL + "file:line: msg" */
long long s = recomp_cfg_int(c, "render.scale", 1);      /* default when missing or wrong type */
double    f = recomp_cfg_float(c, "render.sharpness", 0); /* an int is accepted too */
const char *a = recomp_cfg_string(c, "display.aspect", "4:3");
int       v = recomp_cfg_bool(c, "render.vsync", 1);
static const char *const modes[] = { "stock", "fast", "free", NULL };
int       m = recomp_cfg_choice(c, "game.mode", modes, 0); /* index into the list */
size_t    n = recomp_cfg_array_len(c, "render.sizes");    /* + recomp_cfg_array_int/... (c, key, i, def) */
/* recomp_cfg_count / recomp_cfg_key_at / recomp_cfg_find / recomp_cfg_line for scans */
recomp_cfg_free(c);
```

Every getter accepts a NULL config and then returns its default.

`enhance_cfg.h` is the layering that modules use:

```c
xbox_enhance_init(".", NULL);                   /* the toolkit's keys: enhance.h */
enhance_cfg_init(data_root, title_dir);          /* the reader alone; logs the files */
const char *u = enhance_cfg_user_path();         /* the player's file, for a writer; NULL under none */
const recomp_cfg *d = enhance_cfg_defaults();    /* config.default.toml as loaded, or NULL */
enhance_cfg_bind_env("game.mode", RENV_GAME_MODE); /* a game key with an env override */
int scale = (int)enhance_cfg_int("render.scale", 1);
int mode  = enhance_cfg_choice("game.mode", modes, 0);
const recomp_cfg *c = enhance_cfg_lookup("render.sizes");   /* arrays: no env tier */
enhance_cfg_report_unused();                     /* after the modules' init */
```

A settings writer (the in-game menu) writes only the keys whose value
differs from `enhance_cfg_defaults()` to `enhance_cfg_user_path()`, so the
player's file stays a list of changes. Every writer of a user file uses
`enhance_cfg_patch` (never `recomp_cfg_patch` alone): it writes the file
and updates the run's loaded copy of it, so later lookups see the new
values. `enhance_cfg_source_of(key)` says
`ENHANCE_SRC_DEFAULTS` for a value from the defaults file.

`xbox_enhance_init` does not report unused keys itself: the title calls
`enhance_cfg_report_unused()` once, after it has read its own keys, so a
game key is never listed as unused.

Read the values during init (one thread) and keep them; the layer is not
meant to be read per frame. Tests: `tests/enhance_cfg` (ctest
`enhance_cfg`).

## Mods

A mod is game code built into the title's executable that reads and steers
the guest through one small API (`src/enhance/mod.h`). It exists only with
`XBOXRECOMP_ENHANCE=ON`. A title with no mods runs exactly as before and
logs one line, `[ENHANCE] mods=none` ([The mods= line](#the-mods-line)).
The draw paths then have one untaken branch.

The title lists its mods in a NULL-terminated table and hands it over before
`xbox_enhance_init`, which runs each `init` in table order as its last step:

```c
static int cam_init(const struct xbox_mod_api *api)
{
    s_api = api;
    s_speed = api->cfg_float("mods.free_camera.speed", 1.0);   /* v2; v3: api->setting */
    api->on_frame(cam_frame, NULL);
    api->batch_filter(cam_hide_hud, NULL);
    return 0;                                   /* non-zero leaves the mod out */
}
static const struct xbox_mod free_camera = { "free_camera", cam_init, cam_shutdown, NULL };
static const struct xbox_mod *const mods[] = { &free_camera, NULL };

xbox_mod_register_table(mods);
xbox_enhance_init(".", NULL);
```

The table is handed over with a call, not a self-registering macro, for
three reasons:

- A static library drops an object that nothing names.
- A linker section is spelled differently for PE, Mach-O and ELF.
- The table fixes the order.

The title's glue calls two entries:

- `xbox_mod_frame()` once per guest frame, on the guest's thread.
- `xbox_mod_input_captured()` from its pad read. While it returns true, the
  title must see a released pad.

`struct xbox_mod_api` (version 8, `XBOX_MOD_API_VERSION`) holds:

| Member | What it gives |
| --- | --- |
| `mem`, `XBOX_MOD_MEM32(api, va)`, `XBOX_MOD_MEMF(api, va)` | The guest's memory. It is valid from the first `on_frame` or filter call; it is 0 during init. |
| `cfg_string`, `cfg_int`, `cfg_float`, `cfg_bool`, `cfg_choice`, `cfg_bind_env` | The getters above. A mod's own keys are flat names under `[mods]`, beside its switch: `"free_camera.speed" = 2` is the key `mods.free_camera.speed`. Never a `[mods.<name>]` table: it would clash with the switch `mods.<name>`, and the menu's Save leaves the switch out over one (naming it in the log) while it writes the other changes. A descriptor mod reads its settings through `setting` instead. |
| `log(mod, fmt, ...)` | One `[MOD <name>]` line. |
| `on_frame(fn, user)` | Called from `xbox_mod_frame`, in table order. Init only; up to `XBOX_MOD_MAX_FRAME`. |
| `input_state(out)` | The title's last port-0 read (the keyboard merged), and the mouse's raw counts and buttons while captured. A mod never polls, because a poll would move the keyboard's stick ramp. |
| `input_capture(on)` | Takes port 0. While captured, the mouse's motion goes to the mod instead of the look stick, and the title's pad glue releases the pad. Either edge drops the motion gathered so far. |
| `batch_filter(fn, user)` | Asked before each Metal or D3D11 draw, on the render thread, with the program hash and draw state (`struct nv2a_batch_info`). If any filter returns non-zero, the batch is skipped. `present_offset` is the surface the last flip presented, to tell a back buffer from an offscreen target. A skipped batch inside a visibility test counts as occluded. Init only; up to `XBOX_MOD_MAX_FILTER`. The CPU rasteriser does not ask. |
| `menu_add(item)` | An item (`enhance_menu.h`) on the mod's own list on the menu's Mods page. Kept, not copied, so it must be static; its key persists through Save. Init only; up to `XBOX_MOD_MAX_ITEMS`. |
| `on_enable(fn, user)` | `fn(on, user)` when the Mods page turns the mod off (0) or on (1). The toolkit calls it from the next `xbox_mod_frame`, on the guest's thread, never from the menu's. Only the last toggle before a frame is sent, so `fn` can be called with the state the mod is already in and must be idempotent. The toolkit keeps calling the mod's frame hooks and actions while it is off: the mod gates its own. A mod without one is switched at the next start. Init only. |

| `setting(desc, idx)` | v3. A descriptor setting's value now (any thread; a relaxed atomic load). A bool is 0/1, an enum the choice's index. |
| `on_setting(desc, fn, user)` | v3. `fn(idx, value, user)` after the menu changes a setting, from the next `xbox_mod_frame` on the guest's thread, as `on_enable`. A `restart` setting only records the value and never calls back. Init only. |
| `control_action(desc, idx)` | v3. The action id of an ACTION control, for `action_pressed` and `action_down`; -1 for another kind. |
| `control_value(desc, idx, out)` | v3. A captured control's value while the mod captures port 0: a STICK's axes from the pad's record (the first binding's stick, `src` which one, so the mod picks its dead zone), a BUTTON's largest binding (0..255; a host key reads 255 while down, even with no pad). Returns 1 when it read a source. Without a capture, or with the pad pulled, the pad part reads 0 and `connected` 0. |
| `hook(va, pre, post, flags, user)` | v4. Callbacks around each call of the guest function at `va` ([Hooks](#hooks-v4)). Init only. |
| `active(mod, on)` | v4. The mod's own "doing something now" state, for the `mods=` line. Any thread. |
| `hud_table(toml, len)` | v5. Registers the game's HUD table ([HUD table](#hud-table-v5)). Init only; one mod per run. |
| `hud_apply(c, d)` | v5. From a hook's pre on the call that takes a HUD element's vertices: moves, clips or stretches them for this frame. Guest threads. |
| `hud_restore(c)` | v5. From the same hook's post: puts back what an `XBOX_HUD_RESTORE` apply changed. |
| `hud_capturing(mod)` | v5. Non-zero when the mod's hooks run only for a mapping capture (it owns the HUD table and is off or dormant): a hook then does only `hud_apply` and `hud_restore`. Any thread. |
| `guest_call(va, args, eax_out)` | v6. Runs the lifted guest function at `va` ([Guest calls](#guest-calls-v6)). Only from a hook's pre or post, or an `on_frame` callback. |
| `guest_alloc(mod, size, align)` | v6. A zeroed block of guest memory the title's heap never hands out ([Guest calls](#guest-calls-v6)). Same threads as `guest_call`. |
| `glyph_name(mod, ref, out, cap, flags)` | v7. The binding of a source, an action or one of the mod's controls, named for the device in use ([Glyphs and the status panel](#glyphs-and-the-status-panel-v7)). Any thread. |
| `status(mod, text)` | v7. The mod's status panel at the top left of the screen ([Glyphs and the status panel](#glyphs-and-the-status-panel-v7)). Any thread. |
| `draw3d_begin(mod, view, proj)`, `draw3d_tris(v, n, flags)`, `draw3d_lines(v, n, flags)`, `draw3d_end(mod)` | v8. World-space triangles and lines drawn over the game's 3D frame ([3D debug draw](#3d-debug-draw-v8)). The guest's thread, once a frame. |

`struct xbox_mod` gained two fields in v4, after `desc`:

- `needs`: what the mod uses, `XBOX_MOD_NEED_ASPECT`, `_STATE`, `_POKE`,
  `_HOOK` and `_PLACE` ORed. It is a declaration only: nothing reads it
  until the enable gate (mod-connectivity-layer task 4.2) lands.
- `follows`: NULL, or a config key that turns the mod on (`"camera.free"`).
  While that key holds its default, the mod is left out of the `mods=`
  line even when its init ran. The key is read once, at start, and a
  value that does not parse counts as changed, so the mod is listed. It
  does not gate the init yet (4.2 again). A mod that reads its own key,
  or whose effect hangs on more than one key, should call `api->active()`
  instead: that follows what it actually does, live.

Tables written for v1 to v4 compile unchanged: the new fields are zero.
`struct xbox_mod` has no size field, so it is a source contract for mods
linked into the title, not a plugin ABI.

### Hooks (v4)

`api->hook(va, pre, post, flags, user)` runs `pre(c, user)` before each call
of the guest function at `va`, and `post(c, user)` after it. They run on the
guest thread that made the call, inside the call, and must not block. Either
callback may be NULL, but not both.

- **Slots.** The game declares the VAs a mod may hook in `game.toml`
  (`[mods] hook_functions`). `xbr recomp` then gives each one a slot
  (`src/kernel/recomp_hook.h`; the mods' side is
  `src/kernel/recomp_hook_mod.h`). For a VA without a slot, `hook` logs
  `[MOD <name>] hook 0x...: no hook slot` and returns 0. While nothing
  hooks a slot, it costs one load and a predicted branch.
- **`struct xbox_mod_call`.**
  - `esp` is the guest esp at entry, so `[esp]` is the return address.
    `XBOX_MOD_ARG32(api, c, n)` and `XBOX_MOD_ARGF(api, c, n)` read stack
    argument `n`, counting from 0.
  - `ecx` and `edx` hold their entry values, for thiscall and fastcall.
  - `eax` and `edx_ret` hold the return value. A post callback may change
    them. A function that returns on the x87 stack has `st0_valid` set
    and its value in `st0`. A post's change to `st0` is written back.
  - `st0_valid` is read-only. The slot decides every x87 push and
    write-back, and `st0_valid` is reset before the post callbacks, so a
    hook cannot unbalance the x87 stack by writing it.
- **Order.** The pre callbacks run in table order. The post callbacks run in
  reverse order, and see the same `esp` as the pres.
- **Skip.** A pre hook registered with `XBOX_MOD_HOOK_MAY_SKIP` may set
  `c->skip`. The function's body then does not run.
  - The slot does what the body's `ret n` would do. It pops the return
    address and the callee-cleaned bytes, then returns `eax`/`edx_ret`,
    and pushes `st0` when the function returns a float.
  - The later pre callbacks and every post callback still run. They see
    `skip` set and the return the skipper wrote. A later one that writes
    `eax`, `edx_ret` or `st0` changes what the call returns, and none can
    clear the skip.
  - Only one MAY_SKIP hook is allowed per VA. A second one is refused at
    init.
  - A skip from a hook without the flag is ignored and logged once.
  - Any flag bit other than `XBOX_MOD_HOOK_MAY_SKIP` is refused at init.
- **Mod state.** A mod's hooks run only while the mod is on. While it is
  inited dormant, or after it is switched off live, its callbacks are not
  called and the function runs as if that mod had not hooked it. From
  `xbox_mod_shutdown` on, no hook runs.
- **Failed init.** A failed init's hooks are dropped with the rest of what
  it registered. The hooks take effect together, after every mod's init:
  the log reads `[MOD] N hooked functions`.

### Guest calls (v6)

`api->guest_call(va, a, eax_out)` calls the guest function at `va` from a
mod callback, in the middle of the guest code that ran the callback. It
works only on a guest thread inside a hook's pre or post or an `on_frame`
callback; anywhere else it logs `[MOD] guest_call 0x...: refused, ...` and
returns 0. A guest thread is one the kernel started for the title
(guest-main or a title thread); a host thread (menu, render, input) is
refused even inside a callback, since it has no guest registers.

- **Arguments.** `struct xbox_guest_args` sets `eax`, `ecx` and `edx`, and
  pushes `stack[0..n_stack)`, with `stack[0]` the first argument. At most
  `XBOX_GUEST_MAX_STACK` (16).
- **Frame.** The call's frame starts 0x100 bytes below the current guest
  esp, with the first argument on a 16-byte boundary. The return address
  is `0xDEADBEEF`, so no return-address lookup matches it.
- **Registers.** The whole register set (`src/kernel/recomp_regs.h`, the
  list the DPC path saves too) is saved first and put back after. The
  callee's `eax` goes to `*eax_out` (may be NULL); every other effect is in
  guest memory.
- **Result.** 1, or 0 with one log line when `va` has no lifted function,
  the arguments are too many, or esp is not a guest stack.

The function runs with the guest's state as it is at that point. Whether
calling it there is safe (locks, re-entry, a list being walked) is the
mod's business: call only what the game itself calls from a matching
place.

`api->guest_alloc(mod, size, align)` gives the mod a block of `size` bytes
from the kernel's contiguous arena, zeroed, aligned to `align` rounded up
to a page. It returns the block's guest VA, or 0. One block per mod: a
later call with a size up to the first returns the same block, and a
larger one is refused. Two guest threads asking at once still get one
block. Nothing frees it. Use it for data the guest must
reach by address (a record a guest function reads), never for the mod's
own state.

### Glyphs and the status panel (v7)

A mod built against v7 checks `api->size` before it calls either member:
on an older runtime the fields are past the end, and the mod then shows
nothing. A mod that never calls them changes nothing. The device tracking
below starts at the first call, so a run without one stays stock.

**The device in use.** The toolkit takes the device that saw the last new
press: a pad button or trigger past half travel, or a stick pushed past
8000 of 32767, makes it the pad; a key, the keyboard; a mouse button, the
mouse. Only new presses count, so a key held down does not keep winning
over a pad in use. The pad's family (Xbox, PlayStation, ...) comes from
port 0.

**`api->glyph_name(mod, ref, out, cap, flags)`** names the binding of `ref`:

- `ref` is a source (`pad:south`, `key:F5`, `mouse:left`), an action
  (`act:camera.free.toggle`) or one of the mod's own controls by id
  (`ctl:move`). It may be written in braces (`{ctl:move}`).
- An action or a control names the first binding on the device in use,
  or the first binding at all when that device has none.
- Off the pad, a captured control (a `BUTTON` or `STICK` control) bound
  only on the pad names the keys that drive it. Such a control reads the
  guest's pad with the keyboard merged in, so the key the keyboard map
  binds to its guest control drives it: `rt` names that key, and a stick
  names its four direction keys, up, left, down, right, joined by `/`
  (`W/A/S/D` on the default map). The right stick also names the mouse
  (`Mouse move`), since the guest's mouse look drives that stick and a
  capture hands the motion to the mod. An action control keeps its pad
  chord: actions read the host pad, which no key presses.
- `XBOX_GLYPH_TOKENS` writes tokens (`{pad:guide}+{pad:y}`), which draw as
  glyphs in any text the overlay draws. `XBOX_GLYPH_TEXT` writes names
  (`Guide+Y`, `Cross` on a PlayStation pad, `F6`, `Mouse left`) for a log
  line. A chord's members are joined by `+`.
- It returns the length written. When `ref` is unbound or names nothing,
  it returns -1 and `out` holds `ref` itself, cut to `cap`. When the name
  does not fit in `cap`, it returns -1 and `out` holds only the members
  that fit whole (maybe none, `""`), never part of one: a cut member
  would name another input (`F1` out of `F10`). `out` is always ended.
- It works once the mods' inits are done. Called from `init`, it
  returns -1 with `ref` in `out`: the toolkit installs the names after
  the inits, since `[input.actions]` applies the user's bindings only
  then. A status text set from `init` is not affected, since its tokens
  expand at every draw.

**`api->status(mod, text)`** sets the mod's panel:

- `text` holds up to `XBOX_MOD_STATUS_LINES` (6) lines split by `'\n'`,
  each up to `XBOX_MOD_STATUS_LINE` (120) bytes. The first line is the
  panel's title, drawn with an accent dot; the others are drawn smaller
  under it. NULL or `""` removes the panel.
- A text over either limit is cut, never at a UTF-8 sequence's middle.
  What fits is set and the call returns -1; otherwise 0.
- The text is copied under a lock. Tokens (`{pad:}`, `{key:}`, `{mouse:}`,
  `{act:}`, `{ctl:}`) are kept as written and expanded at every draw, so
  the panel follows the device in use and a rebinding in the menu without
  another call. Set the text when it changes, not every frame: a call
  with the same text does nothing.
- The panel is dropped when the mod is turned off (live or for the next
  start) or its init fails. A late call from it is ignored, except a
  clear (NULL or `""`), which is always taken.

**Placement.** The panels stack down from the top left corner, inside a
5% title-safe inset, in the mods' table order, one card each. They are
drawn at the overlay's UI scale on the `STATUS` layer, above the stats
and under the toasts, the menu and the cursor. A card that would cross
the bottom inset is not drawn, nor any after it: the stack is cut, never
squeezed. The panels are hidden while the menu is open. The toolkit draws
them again when a text, the device, the pad family or the window changes,
and every 250 ms, so a rebinding shows.

**The hook must be armed.** The panels are drawn from the host actions'
flip tick, which runs only while that hook is armed: a feature key on
(rule 6 of [Host actions](#host-actions-inputactions)), `menu.enabled`,
`overlay.stats` or a mod's enable key. With every feature key off, no
panel shows, and `status` still stores the text for when one is armed.
When the hook disarms, the toolkit hides the panels at once, so none
stays on the screen.

### 3D debug draw (v8)

A mod can draw world-space triangles and lines over the frame, seen through
the game's own camera: a collision mesh, a trigger volume, a path. Each
frame, from `on_frame` or a hook on the guest's thread:

```c
if (api->draw3d_begin(&s_mod, view, proj) == 0) {
    api->draw3d_tris(tris, n_tris, XBOX_DBG_OPAQUE);   /* 3 vertices each */
    api->draw3d_lines(edges, n_edges, XBOX_DBG_PULL);  /* 2 vertices each */
    api->draw3d_end(&s_mod);
}
```

A vertex is `struct xbox_dbg_vtx`: a world position and a colour
`0xAARRGGBB` with straight alpha. The flags:

- `XBOX_DBG_XRAY`: no depth test, so it shows through walls.
- `XBOX_DBG_OPAQUE`: triangles write depth, so later lines and blended
  faces behind them hide. Alpha still blends.
- `XBOX_DBG_PULL`: each vertex moves 0.2% of its distance toward the eye,
  so an edge drawn on its own face wins the depth test.

**Where the matrices come from.** The toolkit does not know the game's
camera: the mod passes it. `view` and `proj` are 16 floats each,
row-major, in the row-vector D3D convention the Xbox titles use
(`clip = [x y z 1] . view . proj`, clip z in [0, w]). A title keeps them
in its own memory: the mod reads them there, usually right after the
function that builds them, from a hook or the title's wrapper of that
function. The backends store depth as z / zmax,
which matches the projection's z for the usual viewport (z scale zmax, no
offset), so the overlay tests against the game's own depth buffer.

**When it is drawn.** At the flip, into the frame being presented, after
all the game's draws and before the dumps and the copy to the window. So
it sits over the game's HUD too, and a frame dump with the mod on shows
it. The drawing goes into the guest's own colour target, not into a copy
made for presentation, and opaque triangles also write the guest's depth
buffer. A title that does not clear that target before its next frame, or
that samples or reads back the last frame (a pause backdrop, a motion
blur, a screenshot), can capture the overlay in what it draws next. The order is opaque triangles, depth-tested lines,
blended triangles, then the x-ray parts. Hor+ is accounted for: the
guest's 0..w lands in the middle of the wide target.

**Which frame.** The list drawn is the last one `draw3d_end` published.
It stays on the screen until the next one, so a mod that draws every
other frame does not flicker, and an empty frame (`begin` then `end`)
clears it. A mod that is turned off is cleared at once. The camera the
mod read can be one frame older than the frame on the screen, which shows
only while the camera moves fast.

**Rules.** One mod draws a frame: `draw3d_begin` returns -1 while another
mod's frame is open or when the mod is not on, and 0 otherwise (when the
mod's own frame is already open, the calls add to it). `draw3d_tris` and
`draw3d_lines` return the primitives they added. A list holds at most 2^20
vertices; what goes past is dropped and counted. With no mod drawing,
the flip's cost is one atomic load. D3D11 and Metal draw it; the CPU
rasteriser does not. Without a depth buffer for the frame (D3D11 before
the game draws with one), only the x-ray parts show.

### HUD table (v5)

With a wide `display.aspect`, Hor+ keeps the HUD in the 4:3 columns. A HUD
mod moves the game's HUD elements to the wide frame's edges. It does so in
guest memory, at the call where the game hands an element's vertices over.
The toolkit's HUD layer (`src/enhance/hud_anchor.h`) does the work and is
game-agnostic: every address, size and kind comes from the game's table.

**The mod's part:**

1. At init, `api->hud_table(text, len)` registers the table, TOML text the
   mod embeds (one `config/hud.toml` per game).
2. It hooks the game's vertex call (`api->hook`, in the game's
   `hook_functions`).
3. From that hook's pre, it fills `struct xbox_hud_call` (`vtx_va`, `n`,
   `flags`, and `queue` and `tex` for a later capture, else 0) and calls
   `api->hud_apply(c, &d)`.
4. With `XBOX_HUD_RESTORE` (the game keeps the buffer and draws it again),
   it calls `api->hud_restore(c)` from the post, so a shift never
   accumulates. The call must copy the vertices before it returns.
   RESTORE calls may nest up to 3 deep per guest thread, each restored by
   its own post (matched by `esp`); a fourth is left untouched with
   `[hud.skip] ... restore calls nested too deep`. The saves are
   thread-local, about 24 KB per host thread (3 levels of 1024 x and u
   values). If the owner is switched
   off between a pre and its post, the mod's post is not called and the
   toolkit restores in its place.

**Active.** The layer is active while all of these hold:

- its owner mod is on;
- the table is registered;
- the canvas's target has a Hor+ pad, so `display.aspect` is wider than 4:3.

It is recomputed in every `xbox_mod_frame`. While it is not active, every
entry returns at its first test, no vertex changes and the backends see no
lift, so stock and 4:3 are unchanged. The owner's `mods=` state follows the
same flag. A second mod's `hud_table` is refused, and a failed init drops
the table.

**The schema.** An unknown key or table, a duplicate `ret_va`, or a wrapper
without a hook slot refuses the whole table with one `[hud]` line:

```toml
version = 1                  # required

[canvas]                     # where the game's HUD coordinates live
w = 640                      # canvas units (fixed and memory sources)
h = 480
origin = "centre"            # or "top-left": where x = 0 is
y = "down"                   # or "up"; the x rules never read it
view = [0, 0, 640, 480]      # the target rectangle, guest px, the units map onto
target = "frame"             # the back buffer's guest size, or [w, h]
source = "fixed"             # "fixed", "memory" or "target"
# source = "memory": w_va, h_va (required), view_va (4 values),
# type = "f32" or "u32", mem_scale (a u32 value divided by it)

[layout]
stride = 28                  # bytes per vertex, 4..256
x_off = 0                    # byte offset of the f32 x
u_off = 16                   # optional: f32 u, for the clip
prim = 4                     # vertices per primitive, 1..64 (default 4)

[[wrapper]]                  # a function whose callers name the element
va = 0x0006B1F0
name = "draw_panel"

[[group]]                    # sprites that share one transform
name = "panel"
kind = "left"
# ref_x = -200               # proportional: the reference x, canvas units
# park = 360                 # world: primitives beyond it are parked
# full = "stretch"           # full-width primitives stretched (default "keep")

[[site]]                     # one call site, by return address
ret_va = 0x0006B25A
element = "portrait"
group = "panel"              # or kind = "...": its own group
note = "free text"

[[capture.draws]]            # level 0 of the mapping capture (below);
fn = 0x0004A3C0              # read with every table, used only by it
name = "draw_quad"
vtx = "stack:0"              # where the vertices are: "ecx", "edx" or
count = "stack:4"            # "stack:N" (bytes past the return address);
stride = 28                  # count and stride may be numbers; vtx and
prim = 4                     # count go together; default layout's
# or the short form: [capture] draws = [0x0004A3C0, "0x0004A400"]
```

The canvas maps units to guest pixels by `s = view_w / w`. All rules run in
guest pixels of the target and are converted back before the write. An
offset is a whole guest pixel. A `memory` source is read once per guest
frame. A value that is not finite, not positive or over 4096 falls back to
the declared one, with a `[hud.canvas] ... fallback` line. A `target` source
takes `w`, `h` and `view` from the frame's guest size.

**Elements.** `hud_apply` finds the element from the call chain. Each
wrapper's pre pushes its `[esp]` per guest thread. Sites are matched from
the outermost wrapper inward, then by the hooked call's own `[esp]`. A call
that matches no site is unlisted: its vertices are only clipped to the 4:3
columns.

**Kinds**, per primitive (`pad` is the target's Hor+ pad; `hud_pad` is
`max(0, min(pad, pad at display.hud_aspect) - display.hud_margin)`):

| Kind | Move |
|---|---|
| `left`, `right` | by `-hud_pad` or `+hud_pad` |
| `centre` | none |
| `proportional` | by `cx / (w/2) * hud_pad`, clamped to `±hud_pad`. `cx` is the group's `ref_x`, or else the first member drawn this frame (a group of one: each primitive) |
| `world` | none and no clip, since the game projected it; a `park` bound hides parked sprites |
| `stretch` | mapped across the wide frame by the full `pad`, as Hor+ stretches a fade |

Each non-stretch primitive is clipped to the 4:3 columns moved by its
offset, with `u` clipped in step when `u_off` is set. A primitive wholly
outside them, one the game parked off-screen, is moved past the wide
frame's edge so it stays hidden. A full-width primitive (a fade) is left
for Hor+, which centres one that samples a picture texture as a movie. A
group with `full = "stretch"` maps its full-width primitives as `stretch`
does instead, for a dim or backing drawn with a picture texture by the
same call as smaller panels, which keep the group's kind. It is not
allowed on `stretch` and `world` groups. A call whose vertex count is not a multiple of `prim`, or that
has more than 1024 vertices under `XBOX_HUD_RESTORE`, is left untouched,
with one `[hud.skip]` trace line per site.

**The backends.** While the layer is active, Metal and D3D11 give a 2D
batch drawn on the canvas's target the 3D box (the lift), so moved sprites
show in the pads. Full-width batches (movies, fades) keep their box. On that
target, a batch whose triangles reach past both `-8` and `w + 8` is not
counted as full width (`nv2a_aspect_full_tris_in`), so stretched sprites are
not stretched a second time. An oversized full-screen triangle (0..2w)
passes one edge only and stays full width; a symmetric overscan draw still
passes both and is cleared. A batch without a triangle list must lie inside
both bounds. The layer pushes a stretched sprite's edges that stay inside
the band just past it. The CPU rasteriser has no lift: moved elements are
clipped there.

**Logs.**

- Always:
  - `[hud] table: N sites, G groups, W wrappers` at registration;
  - after the inits, with a table and a pad (`hud_pad` > 0), `[ENHANCE] display.hud_aspect=... display.hud_margin=... hud_pad=N`; none at 4:3, so the goldens' logs do not change.
- With `RECOMP_TRACE=hud`, once per site or change:
  - `[hud.site]`: an element's group, kind and offset;
  - `[hud.canvas]`: the canvas, view, target and source;
  - `[hud] lift on|off pad P hud_pad H`;
  - `[hud.unlisted]`: an unlisted site drawn past the 4:3 columns;
  - `[hud.skew]`: a site with a primitive the window cuts that is not axis-aligned, left unclipped;
  - `[hud.skip]`.

**The mapping capture** (`src/enhance/hud_capture.h`, design A15 of the
openspec change hud-anchors) records what the game draws as HUD, for
`xbr hud-map` to turn into a table. Its `RECOMP_DEBUG` keys:

| Key | Meaning |
|---|---|
| `hud_capture=<dir>` | on: writes `<dir>/capture.jsonl` and one `<dir>/frame_NNNNN.bmp` per captured guest frame |
| `hud_capture_at=<from>:<count>` | the guest frames, counted in `xbox_mod_frame` calls from 1 (default `1:1`, count up to 10000) |
| `hud_capture_table=<path>` | a table file the capture loads and hooks itself, so a capture needs no mod; a table a mod registered wins, with one `[hud.capture]` line |

It records and changes nothing, whatever the HUD mod's switch: it sets the
layer's active flag for recording only, so the wrappers keep their chain
and `hud_apply` reports each call, while shifts, clips and the lift follow
the mod alone. While it runs the HUD mod's hooks reach `hud_apply` even
with the mod off (which then returns -1). The capture can keep that
promise only for what the toolkit does: a HUD mod whose hooks do more
than `hud_apply` and `hud_restore` tests `api->hud_capturing(mod)` and
skips the rest while it is non-zero.

Capturing changes the game's timing: at each captured frame's start
`xbox_mod_frame` naps up to 200 ms for the walker, and on D3D11 each
picture maps a staging copy of the frame synchronously at its present.

- **Level 1** records every `hud_apply` call, before any change.
- **Level 0** hooks each `[capture] draws` entry. A row the runtime cannot
  use is skipped with a line and never fails the table: a `sig:` row needs
  its VA (`xbr hud-map` writes a copy of the table with it). The callers
  are words of the 96 above the return address that lie in the title's
  code and follow a call (`E8`, `FF 15`, `FF D0+r`, `FF 50+r`), up to 8.
  The site is the outermost listed one, else the return address. The
  vertices, when located, are read as pretransformed (x at 0, y at 4) and
  converted to canvas units. Vertices at or above 64 MB (past retail RAM)
  are not read: the `bbox` is null, said once on stderr.

Lines are gathered in a buffer on the guest's thread and written at
`xbox_mod_frame`. A frame's lines wait until its picture is written or
given up (120 frames), then go out as its sprites and its frame line last,
so a reader that sees frame F has F's sprites and picture. The picture is
the flip that presents the frame: at the frame's start the capture waits up
to 200 ms for the walker to finish what the guest kicked, then asks for the
next flip. It has the guest surface's size on every backend (D3D11 under
`render.scale` averages each block down, as Metal's write-back does), so a
`bbox` lands on the same pixels everywhere. A line a second guest thread finishes after its frame was
sealed is dropped and counted in the `dropped` total, never written into a
later frame; readers still group by `frame`. Each picture is a full BMP:
10000 frames at 640x480 are about 12 GB. One JSON object per line:

```json
{"type":"sprite","level":1,"frame":2,"queue":7,"chain":["0x00012340"],
 "ret":"0x00045678","site":"0x00012340","element":"score","group":"score",
 "n":4,"bbox":[300,10,340,30],"prims":1,"tex":"0x00000000DEADBEEF"}
{"type":"sprite","level":0,"frame":2,"queue":null,"draw":"0x0004A3C0",
 "name":"draw_quad","chain":["0x00012900","0x00012010"],"ret":"0x00012F00",
 "site":"0x00012010","element":"timer","group":"timer","n":4,
 "bbox":[100,50,200,80],"prims":1,"tex":null}
{"type":"frame","frame":2,"flip":31,"canvas":{"w":640,"h":480,
 "origin":"top-left","y":"down","view":[0,0,640,480],"target":[640,480],
 "pad":0},"bmp":"frame_00002.bmp"}
```

- `chain`: return addresses, outermost first (level 1: the wrappers';
  level 0: the stack scan's). `ret`: the hooked call's own `[esp]`.
- `site`, `element`, `group`: the matched row, else null; `group` is null
  for a site with its own `kind`.
- `bbox`: `[x0, y0, x1, y1]` in canvas units, null when no vertex was
  read or a corner is not finite (a huge vertex under a small view
  scale); `prims` is `n / prim`. A number is never `inf` or `nan`.
- `queue` and `tex`: what the mod passed in `xbox_hud_call` (`tex` null
  when 0); null at level 0.
- `flip` is null and `bmp` null when no picture could be asked for or it
  failed; `canvas` is null without a table.
- Each VA is a `"0x%08X"` string; numbers are in the C locale.

**Arrays of tables.** The toolkit's TOML reader takes `[[name]]` only
through `recomp_cfg_load_string_aot`, which the HUD table uses. Its rows
read as `name.<i>.<key>`, counted by `recomp_cfg_table_count`. `config.toml` still refuses
them.

### The mods= line

After the inits, the toolkit prints the mods that run and take effect:

```
[ENHANCE] mods=none
[ENHANCE] mods=camera.free,hud
```

A mod is listed when all of these hold:

- its init ran and returned 0;
- it is on now (a descriptor mod's enable switch, or the Mods page);
- its `follows` key, if any, differs from that key's default in
  `config.default.toml`. Values are compared after normalising, so `"off"`
  and `false` are equal, and so are `1` and `1.0`. A key that the defaults
  file lacks counts as different once anything sets it;
- the mod reports an active state and the last report was 1, or it never
  reports one. A default-on mod that does nothing at 4:3, such as
  hud-anchors' `hud`, calls `api->active(mod, 0)` in its init and stays
  out until it is active.

A live change (a Mods page toggle, an `active` call) prints the line again
at the next `xbox_mod_frame`.

A stock run prints `mods=none`. The golden check is meant to pin it,
through the CLI's `ENHANCE_STOCK`, once every game's toolkit prints the
line.

`mods.<name> = false` leaves a mod's init out at start; the Mods page
writes the key. The toolkit keeps each mod's state (`xbox_mod_state`:
on, off, failed, or a change waiting for the restart). A key no lookup
reads prints an `unused key` line, except the keys under a v2 mod left out
this way (`mods.<name>.*`), since its code never ran to read them.

### The descriptor (v3)

A mod can say what it is and what it reads as const data, `desc` in
`struct xbox_mod` (`struct xbox_mod_desc`, descriptor version 2). The
toolkit reads it before any `init`, so the Mods page and the dump can
describe a mod that is off or was never inited, without running its code.
`desc` is a source contract for mods linked into the game, not a plugin
ABI. The toolkit reads only the fields inside `size`, so a descriptor
from an older header is read as far as it goes. A mod with `desc = NULL`
keeps the v2 behaviour.

```c
static const char *const offon[] = { "off", "on", NULL };
static const struct xbox_mod_setting cam_set[] = {
    /* name, key, env, aliases, label, help, unit, kind, live, flags, def, lo, hi, step, choices */
    { "free", "camera.free", "RECOMP_CAMERA_FREE", NULL, "Free camera", NULL, NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, offon },
    { "speed", "camera.free_speed", NULL, NULL, "Speed", NULL, "x",
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0.05, 20, 0.05, NULL },
};
static const char *const toggle_def[] = { "F6", NULL };
static const struct xbox_mod_control cam_ctl[] = {
    { "toggle", "Toggle", NULL, NULL, XBOX_CTL_ACTION, toggle_def, NULL, NULL },
};
static const struct xbox_mod_desc cam_desc = {
    XBOX_MOD_DESC_VERSION, sizeof cam_desc, "Free camera", "Fly the camera", NULL, "1",
    cam_set, 2, cam_ctl, 1 };
```

The descriptor is checked once at start. A setting or control declared
twice, `lo > hi`, a default outside `lo..hi`, an enum with no choices, an
unknown kind or live class, an env name that is not a `recomp_env` row,
or a second `XBOX_SET_ENABLE` setting refuses it: the mod fails with one
line and is never inited.

```
[MOD] cam: descriptor refused: setting 'speed': lo 9 > hi 1; left out
```

**Settings.** A setting without `key` is `mods."<id>.<name>"`, a flat key
under `[mods]`. `key` keeps a legacy spelling (`camera.free_speed`).
Each value is read once at start, highest row first:

| Row | Source name |
| --- | --- |
| the `env` row, when set | `env` (read-only on the Mods page) |
| the user files' key | `user` |
| the user files' aliases, in order | `user_alias` |
| `config.default.toml`'s key | `defaults` |
| `config.default.toml`'s aliases | `defaults_alias` |
| the descriptor's `def` | `default` |

A row of the wrong type is reported and the next row is read:

```
[MOD] cam.speed: ./config/config.toml:7: mods.cam.speed should be a number; ignored
[MOD] cam.speed: RECOMP_CAMERA_FREE_SPEED='fast' is not a number; ignored
```

A number is clamped to `lo..hi` and snapped to `lo + k * step` (an int
is rounded). An unknown enum choice falls back to the default and the
key is left for the player to fix. Each change is one line:

```
[MOD] cam.speed: 25 -> 20
[MOD] cam.hud: bogus -> keep
```

A value read from a user-file alias goes on the pending-rewrite list
(`xbox_mod_pending_rewrites`): the menu's Save writes it under the key and
removes the alias. An ACTION or captured control read from an alias goes
on the same list, as an array of strings.

**The switch and the init rule.** `mods."<id>"` (bool, default true)
keeps its meaning. A descriptor may mark one bool or enum setting
`XBOX_SET_ENABLE`; the Mods page shows it as the mod's Enabled switch, and
its value is printed at start whether or not the mod is inited:

```
[ENHANCE] camera.free=off
```

The line prints the value the run uses: when `mods."<id>" = false` holds
the mod off, it says off and why, whatever the setting holds:

```
[ENHANCE] camera.free=off (held off by mods.camera.free)
```

The Mods page's toggle for such a mod is its enable setting: Save writes
that key in the setting's own type (an enum's choice text, a bool), never
`mods."<id>"`, and turning it on removes a `mods."<id>" = false` from the
user file. A setting's or control's full key must be under 64 bytes (the
env binding's limit), or the descriptor is refused.

The mod is inited when `mods."<id>"` is true and either it has no enable
setting or that setting is on. With the setting off, the menu on
(`menu.enabled`) and the setting's live class `live`, it is inited
dormant, so the menu can turn it on without a restart. A dormant mod that
gave no `on_enable` gets one line, and turning it on applies at restart:

```
[MOD] cam: inited dormant but gave no on_enable; turning it on applies at restart
```

Otherwise it is not inited. The menu's change to a setting is clamped and
snapped the same way, stored at once, and reaches the mod through
`on_setting` at the next frame. A `restart` setting only records it.

**Link settings (descriptor version 2).** A setting of kind
`XBOX_SET_LINK` puts a toolkit menu item on the mod's page. Its `key` is
the item's (`input.pad_square_left`); `label` and `help` NULL take the
item's. The row is the item's own: the same value on both pages, one
staged change, and Save writes the item's key once. An env row that
forces the item makes both rows read-only. The mod stores nothing:
`api->setting` reads the item now (a toggle 0 or 1, a choice its index,
a numeric choice or a range its number), `on_setting` is never called
for it, and Reset this mod leaves it alone. Its source name is `link`.
A change from the mod (`xbox_mod_setting_set`) is clamped and snapped as
the item's row does it; `RECOMP_DEBUG=mod_set_at` does not take a link's
key. `api->setting` calls the item's get, so link only items whose get is
safe from any thread. Both tabs carry the unsaved dot while the row is
unsaved. A key that names no toolkit item with a get and a set, a link
with `env`, `aliases` or `XBOX_SET_ENABLE`, or a link in a version 1
descriptor refuses the descriptor:

```
[MOD] fdm: descriptor refused: link setting 'square_left': no toolkit item input.pad_sqare_left; left out
```

```c
    { "square_left", "input.pad_square_left", NULL, NULL, NULL, NULL, NULL,
      XBOX_SET_LINK, 0, 0, 0, 0, 0, 0, NULL },
```

The structs are unchanged, so version 1 descriptors read as before. A
toolkit before version 2 refuses the kind ("unknown kind 4"), so a mod
that must build against one puts the row under
`#if XBOX_MOD_DESC_VERSION >= 2`.

**Controls.** An ACTION control is registered before `init` as the host
action `<id>.<control id>`, with its defaults, aliases (full keys, such
as `input.actions.camera_toggle`) and env row (`"F6,guide+y"` replaces
the bindings). While the mod is off the action is dormant: it never
fires, consumes nothing and does not arm the hook, but its bindings keep
their inputs in the one-input-one-action rule, so switching the mod on
moves no binding. When a dormant default wins a tie, one line says so:

```
[MOD] F6: camera.toggle keeps it over probe.go (dormant)
```

A BUTTON or STICK control is bound under `[input.controls]`, flat, as
an array (the highest file that names the key decides all of it):

```toml
[input.controls]
"camera.free.rise" = ["rt", "Space"]
"camera.free.move" = ["right_stick"]
```

An INFO control is shown and never bound. A binding names:

| Binding | Source | Reads |
| --- | --- | --- |
| `dpad_up` .. `dpad_right`, `start`, `back`, `left_thumb`, `right_thumb` | the guest's digital button | 255 or 0 |
| `a`, `b`, `x`, `y`, `black`, `white`, `left_trigger` (`lt`), `right_trigger` (`rt`) | the guest's analog button | 0..255 |
| `left_stick`, `right_stick` | the guest's stick (STICK controls only) | the two axes |
| `mouse1` .. `mouse5` | the mouse button (BUTTON controls only; the wheel is no source; reads 0 while `input.mouse` is off) | 255 or 0 |
| anything else: a key name (`Space`, `E`, `up`, `F6`, `Numpad0`) | the host key | 255 or 0 |

The guest sources are the pad as the game would see it, after
`[input.pad]` and the keyboard's `[input.bindings]`, so a remap moves
them with the game's own controls. Host-only buttons (Guide, the paddles,
the touchpad) have no guest slot and are no source; stick directions
(`left_stick_up`) neither. A text that names nothing is left out with a
line, `[MOD] input.controls.camera.free.rise: "pad.ls" is not a game
button, a host key or a mouse button; left out`.

While a mod captures port 0 (`input_capture`), the title reads a
connected, released pad, with a packet number that moves as the capture
starts and ends so the title drops what it held. The pad the title would
have read is recorded for `control_value` after the host actions have
taken out a consumed chord's buttons: `guide+y` toggling flight never
also sinks on Y, and Y stays out until it is let go. A Y alone sinks.

**Rebinding from the Mods page.** A on a BUTTON row captures one input,
a key (a modifier alone too: Shift, Ctrl), a mouse button (left, right
or middle click) or a game button, and adds it to the list, up to 8; X
(Delete) drops the last, and the last binding stays. A on a STICK row
captures a stick: it takes a direction only past 0.6 of full travel, so
a resting stick never binds. Esc cancels. The capture refuses, in the
status line and a toast:

1. a source another captured control of the same mod binds
   (`R is Free camera: Rise`). Two STICK rows trade sticks instead: Move
   on the right stick gives Look the left;
2. a key an action has as a single key (`F6 is Free camera: Toggle
   flight`), a dormant action's too; and the reverse, a key a captured
   control names, on the Controls page (`R refused: Free camera: Rise
   uses it`);
3. a paddle or Guide (`fly controls take buttons the game sees`), a
   stick on a BUTTON row or a button on a STICK row (a click there too);
4. a click while `input.mouse` is off (`the mouse is off; turn on
   input.mouse first`): the menu's pointer works without it, but the
   mouse then records no press, so the binding would never fire.

A button inside an action's chord is fine (Y with `guide+y`): the chord
is taken out before the record. Controls of different mods may share a
source, since one mod captures at a time. A captured control set by its
env row is read only. Save writes the changed keys, and a control back at
its stock removes its key. Reset this mod and Revert cover them as they
cover the settings; "Reset <mod> controls", under the Controls group,
puts back only the mod's actions and captured controls.

The rows draw their bindings as glyphs for the pad in use and key caps
(D8), as `{bind:}` writes them; a row too narrow for the glyphs shows the
names.

**Unknown keys.** A `mods."<id>..."` key a descriptor does not declare,
a key of an id the build does not have, and an `[input.controls]` key no
control declares are reported as `unused key`. Neither Save nor Reset
removes them. The line names the mod a key belongs to, the longest mod
id that prefixes it (under `mods.`, `input.controls.` or
`input.actions.`, or at the id itself, followed by `.`, `_` or nothing):
`[ENHANCE] unused key mods.lod.distnce, config/config.toml:5 (mod lod)`.
The mod's page lists the same keys under "Unknown keys".

### The descriptor dump

`RECOMP_MODS_DESCRIBE=json` makes `xbox_enhance_init` resolve every
descriptor as a start would, settings and bindings included, then write
the JSON to `RECOMP_MODS_DESCRIBE_OUT` (replaced) or stdout and exit 0.
No mod `init` runs and no hook is armed. A file that cannot be written
exits 1 with one line:

```
[MOD] RECOMP_MODS_DESCRIBE_OUT=/no/x.json: cannot write it (No such file or directory)
```

Schema 1. Fields are only added; one removed or changed in meaning bumps
`schema`. A field that does not apply is left out.

```json
{"schema": 1, "api": 8, "mods": [
  {"id": "fly", "title": "Free camera", "descriptor": true, "init": "no", "enable": "fly.enabled",
   "settings": [
      {"name": "speed", "key": "mods.fly.speed", "kind": "float", "label": "Speed",
       "default": 1, "lo": 0.05, "hi": 20, "step": 0.05, "live": "live",
       "env": "RECOMP_APU_HRTF", "aliases": ["fly.old_speed"], "value": 1, "source": "default"}],
   "controls": [
      {"id": "toggle", "kind": "action", "action": "fly.toggle", "label": "Toggle", "when": null,
       "defaults": ["F6"], "bindings": ["F6"], "source": "default"}]},
  {"id": "bad", "descriptor": true, "init": "failed", "reason": "setting 'x': lo 9 > hi 1"},
  {"id": "legacy", "descriptor": false}
]}
```

- `init`: `yes`, `dormant`, `no` or `failed` (with `reason`), what a start
  would do now.
- `source`: the row a value or a control's bindings came from (the table
  above).
- `value` and `default`: a bool as `true`/`false`, an enum as its choice,
  a number in the C locale's shortest round-trip form.
- `live`: `live`, `activate` or `restart`.
- A `link` setting has `name`, `key`, `kind`, the `label` and `help` the
  page shows, the item's `live`, `value` in the item's form and `source`
  `link`; no `default` or range.
- `bindings`: an action's texts as the config gives them. `refused`, only
  when nonempty, lists those of them that would never fire at a start: an
  input name the game does not know, an input another action holds, or a
  pad input the game uses itself (an off binding, see the `[INPUT]`
  warnings above). The dump builds the action table as a
  start does, so `refused` is what the game would drop; the launcher
  shows these unbound instead of rerunning the rules itself.
- `summary`, `help` and the labels are the descriptor's text as written,
  so they may hold the menu's glyph tokens (`{ctl:move}`,
  `{act:camera.free.reset}`, `{mouse:move}`, see the text tokens in
  `enhance_text.h`). A reader shows them as text or expands them from
  the dump's `bindings`.

A top-level `shaders` field lists the presets the menu can pick
(the bundled ones, and the configured preset when it is a path), each
with its parameters parsed from the preset file, without a GPU:

```json
"shaders": {"active": "crt-lottes", "presets": [
  {"name": "crt-lottes", "params": [
    {"name": "warpX", "label": "warpX", "min": 0, "max": 0.125, "step": 0.01,
     "default": 0.01, "value": 0, "key": "present.shader_params.crt-lottes.warpX"}]}]}
```

`default` is the preset's value and `value` the one this run resolved
(an override, or the default). `presets` is empty when the library is
missing, and a preset that is not there or does not parse is left out.

After `presets` come the [shader profiles](#shaderprofiles):

```json
"active_profile": "soft-crt", "profiles": [
  {"name": "fsr", "slug": "fsr", "file": "", "stages": ["fsr"],
   "builtin": true, "state": "ok"},
  {"name": "Soft CRT", "slug": "soft-crt", "file": "soft-crt.toml",
   "stages": ["fsr", "crt-lottes"], "builtin": false, "state": "ok"}]
```

`active_profile` is the profile the start chose, `none` when it chose
none (a `present.shader` choice shows in `active` only). `profiles` lists
each bundled preset that is in `presets` as a one-stage `builtin`
profile, then the folder's files by name, with `state` `ok`, `broken` or
`newer`. Schema stays 1: the fields are additions.

`tests/mod_desc/expected.json` is the full dump of the test table.

A failed init leaves nothing behind. Its registrations are dropped and the
capture goes back to what the earlier mods left. Shutdown runs in reverse
order, once, at exit. The guest and the render thread may still be running
then, and a frame or filter call may be in flight. So a mod's shutdown must
not free anything its callbacks use.
Hotkeys come from `xbox_FramebufferKeyDown` and the names in
`src/input/input_names.h`. Tests: `tests/enhance_mod` (ctest `enhance_mod`)
`tests/mod_desc` (ctest `mod_desc`, the descriptor and the dump), and
`tests/mod_hook` (ctest `mod_hook`, the v4 fields, the `mods=` line, the
hooks and the HUD table's registration), and `tests/hud_anchor` (ctest
`hud_anchor`, the HUD layer's table, kinds, clip, chain and canvas).

## Design note: why the reader is our own

The config format is TOML because the user asked for TOML, or failing that
ini. Every file the reader accepts is valid TOML, so the files can be checked
with any TOML tool.

Two existing C readers were considered:

- **tomlc99** (CK Tan, MIT). Complete TOML 1.0 with a tree API. Its README
  marks it obsolete in favour of tomlc17.
- **tomlc17** (CK Tan, MIT). The maintained successor. It is complete TOML
  1.0/1.1 and has dates, inline tables and arrays of tables.

Both licences are MIT, the same as the toolkit, so either could be vendored
with a NOTICE entry. Each is several thousand lines, and most of that code is
grammar this file does not need. Their errors are good, but their API is a
tree that we would wrap in a getter layer anyway.

The reader here, `recomp_cfg.c`, is about 900 lines including comments. It
supports exactly the needed subset:

- tables, dotted keys
- strings, integers, floats, booleans
- flat arrays
- comments

Its errors carry a line number, and every unsupported construct is reported
by name instead of being half-parsed. There is no third-party code to track,
which matters because the upstream toolkit's rule is no external
dependencies.

If a later need goes past the subset (inline tables for input bindings, for
example), the same API can be put on top of a vendored tomlc17 without
changing any caller.
