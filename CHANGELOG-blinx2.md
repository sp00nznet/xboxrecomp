# blinx2/portability changelog

This branch is apexJCL's fork of xboxrecomp that BLiNX 2 and Burnout 3 build
against: upstream plus POSIX hosts (macOS, Linux), a Metal backend and fixes
that have not reached upstream yet. Its releases are the `blinx2-vX.Y.Z` tags;
upstream's own versions are listed in the README's Changelog. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Entries start at
blinx2-v0.3.2; earlier tags are described by their commits.

## [Unreleased]

## [blinx2-v0.14.0] - 2026-11-04

### Added
- `system.language` sets the language the game is told
  (`XC_LANGUAGE`), and the menu's Game page offers the languages the game
  declares. The game reads it at boot, so a change applies at the next
  start; the default stays English.
- Mod API version 8: `draw3d_begin`, `draw3d_tris`, `draw3d_lines` and
  `draw3d_end` let a mod draw world-space triangles and lines over the
  frame at the flip, depth-tested against the game's own depth buffer or
  as x-ray, on D3D11 and Metal. With nothing submitted the flip is
  unchanged. A v7 mod still loads.
- `RECOMP_DEBUG=tex_dump=<dir>` writes each uploaded texture once, by the
  same hash the texture pack loader looks up, so a pack author replaces
  exactly what the game asks for. Off by default.
- The groundwork for restarting a game in its window: a stop probe
  (debug, off by default), a scanner and ctest that classify every
  static, and the guest unwind that brings each guest thread to a known
  stop point. Everything stays off until a game enables it.

### Changed
- The menu sizes to the display it is shown on, not the output height
  alone (`overlay.ui_fit`, default `display`): it is readable on a Steam
  Deck's 7" panel and follows a high-DPI desktop's scale. Its size no
  longer changes with render scale or a shader, the Size setting applies
  at once, and pointer and touch map through the window's fit, so taps
  land at render scale 2 and 3. `output` keeps the old rule.
- Hor+ and `render.snap_2d` no longer stop draw merging: settled 3D
  batches merge as at 4:3, on D3D11 and Metal, so BLiNX 2's stage 2 at
  16:9 no longer slows down.
- Deterministic mode steps its clock only at frame-wait loops, so BLiNX 2
  runs at about 32 flips per virtual second in the mode (8 before).

### Fixed
- On a dump of another release, a hand-written wrap whose address is no
  longer a function start stopped the build at link with an undefined
  `sub_X_gen` and no reason given. `recomp` now stops first, naming each
  address and the likely cause.

## [blinx2-v0.13.0] - 2026-11-03

### Added
- A Saves page in the menu exports a title's saves to a zip and imports
  one back. An import is checked whole (names, sizes, CRCs, title, keys)
  and previewed, the live saves are backed up, and the new ones swap in
  with renames that roll back; the game then quits so it cannot write its
  old saves over them. Export and import wait while the game is saving.
- The Files page imports and exports settings and shader profiles through
  an in-menu file browser (Home, and each drive on Windows). An import is
  previewed before Apply, and keys the menu does not know are kept.
  "Open exports folder" opens the folder in the host's file manager.
- The menu takes the mouse: hover, click, slider drag and wheel steps,
  pad focus regions with button hints, and an on-screen keyboard for
  renames started by mouse or touch. Esc and Backspace act as B. The
  pointer maps through the presented view on Metal and D3D11.
- A new menu layout: compact and expanded sizes (R3), a left toolbar that
  folds to icons, footer buttons with their shortcut glyphs, and
  `overlay.ui_scale` (0.75-2, restart-only).
- Mod API version 7: `glyph_name` and the `{ctl:}` token name a mod's
  controls for the device in use, and a per-mod status panel redraws
  them after a rebind or a device switch. A v6 mod still loads.
- The free camera page draws bindings as glyphs (`{bind:}`), fly controls
  can be held on mouse buttons 1-5, and "Reset <mod> controls" restores a
  mod's bindings without touching its settings.
- Deterministic mode (debug key `deterministic`, off by default): every
  guest clock read goes through one time source that steps once per
  emulated vblank, guest threads take turns in a fixed order, file times
  are fixed, and OHCI pads report on the virtual clock, so a pad replay
  gives the same per-frame state hashes (`state_hash`) run after run on
  one host. With the mode off the clock reads are unchanged.
- `pad_record` can count steps on the title's own pad polls
  (`pad_clock=poll`), ends a record with `@end`, and `pad_quit` closes the
  game once a script has run out.

### Fixed
- Text the CLI packages (`*.sh *.py *.in *.toml *.env *.default *.slang
  *.slangp`) stays LF on Windows checkouts with `core.autocrlf=true`, so
  the build wizard's package step no longer refuses
  `shaders/param-kinds.toml`.
- `pad_record` writes one `@end` when the quit hooks race a poll, a failed
  window close falls back to the quit thread, and the signal path no
  longer formats with `snprintf`.

## [blinx2-v0.12.0] - 2026-11-01

### Added
- The in-game menu marks each row whose value differs from the saved
  config, saves from any page (Save, or Cmd/Ctrl+S) and reverts a row on
  its own. Closing with unsaved changes asks *Exit without saving*,
  *Exit and Save* or *Go Back*. The selected row's help shows in the
  footer, and L3 toggles the hints. A Cmd+S pressed during a rename, a
  capture or the close dialog is dropped, so it never saves a half edit.
- Shader parameters on the Shaders page can be toggles and choices, not
  only sliders, so on/off effects and modes no longer read as a 0..1
  float. The kind comes from the shader's own annotations, with a
  sidecar file for shaders that carry none.
- A mod's captured controls read from one port-0 pad record and can be
  rebound on the Mods page. Ports other than 0 are left to the game, so a
  mod never takes a second player's pad.
- Overlay text can name a pad button or a key by its glyph: Lucide icons
  and Kenney Input Prompts baked into symbol faces (licences in
  `LICENSES/`), key names in the window's keyboard layout, and a
  GameCube family.
- Groundwork for the menu's coming text fields and Files page: a text
  input ring the host window feeds (SDL text input; Win32 `WM_CHAR` with
  DBCS lead bytes) and the menu reads without racing the host thread, and
  a UI-free file browser model for importing and exporting configs that
  never overwrites a file, never follows a planted link and refuses
  names Windows would refuse, on every host.
- HUD tables take a `full` group key: full-width backings (a pause dim, a
  dialogue gradient, a battle zoom) stretch to the wide frame instead of
  staying boxed in 4:3.
- `present.shader_lines` (`"auto"`, `"guest"`, `"host"`) picks whether a
  shader chain gets the guest's line count or the scaled frame; `auto`
  decides by the preset's name or a profile's `lines`.

### Changed
- The HUD layer's stretch moves only the edge still inside the 4:3 band,
  and an already-wide batch counts as full width once it passes one band
  edge. A batch with no triangle list keeps the two-edge rule. With the
  layer on, BLiNX 2's locker-room backdrop now covers the frame, as it
  does with the layer off.

### Fixed
- The shader chain draws into the stock picture's rect instead of over
  the whole window, on Metal and D3D11.
- CRT presets are fed the guest's 480 lines, so at render scale 2 or 3
  their scanline mask no longer beats against the output in wide bands.
  Other presets (`fsr`) still get the scaled frame.
- Hor+: a post pass that reads the padded frame is no longer taken for a
  movie and put back at 4:3, which left a seam and cut the lens flare in
  BLiNX 1.
- The lifter's 8- and 16-bit one-operand `mul`, `imul`, `div` and `idiv`
  read and write AL, AX, AH and DX as the CPU does. They were lifted as
  the 32-bit forms, so `imul cl` wrote the high half into EDX and
  `div r/m8` divided EDX:EAX; BLiNX 1 had a function whose float came out
  as 0 or NaN. Regenerate gen to pick it up.

## [blinx2-v0.11.0] - 2026-10-31

### Added
- Mod API v6: `api->guest_call` runs one of the game's own recompiled
  functions from a mod (inside a hook or frame callback) with every guest
  register, x87 and SSE state saved and put back around it, so the game
  carries on as if no call happened; `api->guest_alloc` gives each mod one
  zeroed block of guest memory, taken at first use so a mod that is off
  leaves the memory map stock. Both refuse host threads. The DPC path now
  shares the same register set and also keeps `g_mxcsr`.
- Shader profiles: named chains of up to 8 slang presets, saved as TOML
  under `config/shader-profiles/`, can be made, named, edited, switched
  and deleted from the in-game Shaders page on Metal and D3D11.
  `present.profile` (`RECOMP_PRESENT_PROFILE`) picks one at start (off by
  default, so stock output is unchanged). A failing stage falls back to
  the stock present with a red row, and a chain forced from the
  environment cannot be edited from the menu.
- Pad scripts and `pad_record` reach games that answer `XInputGetState`
  themselves and never start the USB host (BLiNX 2): `usb_gamepad.h`
  gains `usb_gamepad_game_press`, `_record` and `_pads`, with the same
  step format, so a record replays through either path.
- Recorded pad input can anchor to file opens: `pad_anchor=<text>|...`
  beside `pad_record` writes `@open:<file>` steps, and a replay holds its
  clock to those opens, so a load that runs longer or shorter than when
  it was recorded no longer drifts the presses off the menus. A late open
  is waited for (retrying a recorded tap up to 8 times) and given up
  after 1800 flips. With no key set nothing changes.
- `RECOMP_DEBUG=mod_set_at=<frame>:<key>=<value>,...` makes a menu
  setting change at a given frame, so a live mod switch can be captured
  unattended.

### Changed
- Each game gets its own default hard disk,
  `<data>/xboxrecomp/<title id>`, so titles no longer see each other's
  saves or share one cache. On the first run a game's old save folders
  are copied once from the shared root, which is only read: each copy is
  built in a temporary folder and renamed into place, so a full disk or a
  second instance of the game leaves no half copy, and file times are
  kept. A folder the player has already saved into is never overwritten;
  the log says where the old saves wait to be copied by hand.
  `RECOMP_HDD_DIR` and installed builds are unchanged.

### Fixed
- A start shader profile waits for the shader cache folder on a fresh
  hard disk, reports a missing stage at once instead of waiting forever,
  and its wait is ordered safely between threads.
- A HUD mod's hooks switch together with the HUD anchor layer at the next
  frame, so turning the HUD mod on or off from the menu no longer draws a
  frame with the HUD half moved.

## [blinx2-v0.10.0] - 2026-10-30

### Added
- Mod API v4: a mod declares `needs` and `follows` (the setting it acts
  on), and reports with `api->active()` when it has nothing to do, so a
  default-on mod stays out of the way. Mods can hook recompiled
  functions: `recomp_hook.h` is the contract the CLI's generated hook
  slots call (`[mods] hook_functions` in `game.toml`); hooks run pre in
  order and post in reverse, may skip the original with its stack pop
  and x87 return, and stop at shutdown and while their mod is off. A
  game with no hook slots is unchanged.
- The `[ENHANCE] mods=` token lists the mods at work (`none` at stock),
  so a golden run proves no mod changed it.
- The HUD anchor layer and mod API v5: a game's HUD mod hands over a
  table of the calls that draw each HUD element, and under Hor+ each one
  moves to its anchor (left, right, centre, proportional, stretch) at
  that call and is restored byte for byte after it. Metal and D3D11 lift
  the 4:3 2D clip for the moved batches only while the layer works.
  `display.hud_aspect` (default `display`) and `display.hud_margin`
  (default 0), a *HUD margin* item on the Render page when a table is
  loaded, and `RECOMP_TRACE=hud`. With no table it does nothing.
- HUD mapping capture (`RECOMP_DEBUG=hud_capture=<dir>`,
  `hud_capture_at`, `hud_capture_table`): per-draw HUD candidates in
  `capture.jsonl` and guest-sized frames, for mapping where a game draws
  its HUD; mods see `api->hud_capturing`.

### Changed
- `storage.ram_cache` (`RECOMP_STORAGE_RAM_CACHE`) is on by default: every
  title's cache partition (`Z:`, `Partition3`-`5`) lives in memory, capped
  at the stock 750 MB and never written back, so the host disk no longer
  takes the title's cache churn. `ram_cache = false` or
  `RECOMP_STORAGE_RAM_CACHE=0` (also `false`, `no`, `off`, now read the
  same with and without the enhancements layer) keeps it on disk. The
  `[MEM] exit` line adds `ramcache_mib` and `ramcache_peak_mib` when the
  store was used.
- The MCPX APU DSP is on by default: the GP and EP run each title's own
  DSP56300 programs (reverb, effects sends, the title's mix) on the
  dsp56300 JIT instead of the stub's stereo fold. `XBOXRECOMP_APU_DSP`
  defaults ON on every target with a prebuilt archive (macOS, Linux and
  llvm-mingw Windows, x86-64 and arm64) and OFF, without a warning, on
  the others (MSVC, iPadOS, universal macOS). A tree that cached the old
  default OFF turns ON once at its next configure, and so does an OFF
  given with `-D` under the old toolkit (its cache cannot tell the two
  apart); give `-DXBOXRECOMP_APU_DSP=OFF` again to keep the stub. Unset,
  `RECOMP_APU_DSP` runs the JIT in a build with the DSP and its device;
  `RECOMP_APU_DSP=0`, or a game's `D(APU_DSP, "0")`, keeps the stub.
- Trees that point `XBOXRECOMP_DSP56300_CACHE` at one directory share
  the dsp56300 download: the extraction is renamed into place whole, and
  a tree whose stamp matches is only read. A cached tarball that does not
  match the pin is fetched again, a download that loses the rename to
  another tree with the same file is accepted, and download or extraction
  debris older than an hour is swept. `tests/posix_ctest.sh` fills it
  once per run.
- A texture pack keeps the entries it has read under a 512 MB resident
  budget, oldest freed first, and reads them again on their next upload;
  a pack rebuilt mid-session falls back to the guest's texels.
- Metal decodes a linear read narrower than a dirty render target's pitch
  from guest memory without writing the target back on every flip, as
  D3D11 does (Kingdom Under Fire's front end at render scale 4: about 18
  to 30 fps).

### Fixed
- A window within a pixel of the picture's shape is filled instead of
  letterboxed by one pixel (Kingdom Under Fire sat one row off with a
  dark line at the edge), on D3D11, Metal, the shader viewports and the
  overlay.
- At render scale above 1, flat full-screen draws cover the first host
  row and column (Kingdom Under Fire showed a stale strip); stock scale
  is unchanged.
- The RAM cache drops seeds a title never opened, oldest first, when a
  write needs room past the 750 MB cap, so a stale shared `Cache`
  directory no longer fills it (BLiNX 2's rewind looped on a full disk).

## [blinx2-v0.9.0] - 2026-10-29

### Added
- A Shaders page in the in-game menu: the *Shader* row moves there from
  the Render page, and the active slang preset's `#pragma parameter`s get
  a range row each, applied at the next present on Metal and D3D11, with
  *Reset to preset*. Save writes only the changed values, per preset, as
  `present.shader_params.<preset>.<param>`. The page is left out where no
  shader can run (no chain presenter, or a library that is missing or
  does not load), and the log says why once. `RECOMP_MODS_DESCRIBE=json`
  lists each preset's parameters under `shaders`.
- `LICENSES/` carries the MinGW-w64 runtime and llvm-mingw (Apache-2.0
  with LLVM exception, plus the Legacy NCSA text) notices of the code
  linked into every Windows executable, and `NOTICE` names what each one
  links. The macOS librashader's crate notices
  (`third_party/librashader/THIRDPARTY-macos.txt`, reproducible with
  `notices-macos.sh`) are staged into `shaders/`.

### Changed
- x86-64 Windows uses our D3D11-only build of the unmodified librashader
  v0.12.0 source (`third_party/librashader/build-windows.sh`, llvm-mingw,
  static libc++), pinned by sha256 and fetched from the fork's
  `librashader-v0.12.0-xbr1` release with its MPL-2.0 text and notices.
  It imports only system DLLs, so `present.shader` runs on a stock
  Windows 11 without D3DX9 or the Visual C++ runtime. macOS and arm64
  Windows keep upstream's builds. A load failure names the missing
  import; `librashader_imports` checks the DLL's import table.
- While a shader runs, the present surface is the output size rather
  than the 640x480 frame, so gamescope no longer rescales the mask and
  scanlines by a non-integer factor (colour fringes). Stock output is
  unchanged.
- The bundled `crt-lottes` and `crt-lottes-fast` wrappers set a gentler
  curvature than upstream; the vendored files are untouched.

## [blinx2-v0.8.0] - 2026-10-29

### Added
- `present.shader` (`RECOMP_PRESENT_SHADER`, default `none`, behind
  `XBOXRECOMP_ENHANCE`): the present pass hands the render-scale frame to
  a RetroArch slang preset run by librashader v0.12.0 on Metal and
  D3D11, with a Shader row on the menu's Render page. Bundled presets:
  `crt-lottes`, `crt-lottes-fast`, `crt-hyllian` and `fsr`, or a path to
  your own `.slangp`. The prebuilt library is fetched by cmake, pinned by
  sha256 and loaded at run time, never linked; a missing library logs one
  line and presents as before. The CPU backend ignores it.
- `render.snap_2d` (`RECOMP_RENDER_SNAP_2D`, default off, live from the
  menu): above render scale 1, swizzled and magnified 2D stages sample on
  the guest texel grid, so loading screens and HUD tiles have no seams.
  With it off the shader texts are byte-identical to before.
- `XboxSpinSiteStats` exposes each spin site's pass and yield counts.

### Changed
- Back-to-back triangle batches with the same draw state go to the
  backend as one draw; batches that sample their own colour target are
  never merged. BLiNX 1's snow stages, thousands of tiny batches a frame,
  no longer cost most of the frame. Debug keys `pb_merge` and `pb_mlog`.

### Fixed
- The BUMPENVMAP du/dv decode divides by 128, as the NV2A does, not 127
  (checked against the nxdk_pgraph_tests Bump_map hardware goldens), on
  Metal and D3D11.
- `NtQueryVolumeInformationFile` class 1 and `NtQueryInformationFile`
  class 6 answer on every file backend, so Kingdom Under Fire: Heroes
  passes its fatal screen after character select.
- Under `RECOMP_HEAP_RECLAIM`, free blocks that end at the bump cursor fold
  back into it when an allocation would pass the heap top (KUF's battle
  load).
- The stub boot ROM copies DSP scratch into P memory when a core leaves
  reset, as the MCPX ROM does; KUF's DirectSound check passes and its
  audio is no longer choppy.

## [blinx2-v0.7.0] - 2026-10-28

### Added
- An in-game menu behind `XBOXRECOMP_ENHANCE`, off unless `menu.enabled`
  (`RECOMP_MENU`) is set: Game, Mods, Controls and Debug pages, binding
  capture that refuses an input another action owns, and Save, which
  writes only the changed keys to `config.toml`. `menu.open` defaults to
  F10, Guide+A and paddle1. Its options (aniso, movie, overscan, filter,
  fullscreen, pacing) switch live at the next flip on Metal and D3D11.
  While the menu is open, the game sees released pads.
- The overlay core: a stats strip and toasts on Metal, D3D11 and the SDL
  presenter, armed only when a feature key asks for them. Host actions
  bind keys, pad buttons, paddles and stick directions without passing
  them to the game.
- Mod API v2 (`menu_add`, `on_enable`) and settings API v3: a mod can
  describe its info, typed settings and rebindable controls, and the menu
  gives each mod its own page. `RECOMP_MODS_DESCRIBE` dumps the
  descriptors as JSON.
- Pad setups (`[input.pad]`): remaps, stick swap and inversion, deadzone,
  pad families for glyphs and Steam Input detection, applied in one place
  for every backend. Without `[input.pad]` the pad path is unchanged.
- `storage.ram_cache` (`RECOMP_STORAGE_RAM_CACHE`, off by default): the
  cache partition (`Z:`, `Partition3`-`5`) lives in memory, capped at
  750 MB and never written back, so a title that rewrites its cache all
  the time does not wear the SSD.
- A `[MEM] exit` line on every exit path with the process's RAM and GPU
  memory (Metal, D3D11); the `mem` trace key samples it while running.
- An on-disk cache of compiled D3D11 shader bytecode (`RECOMP_DXBC_CACHE`,
  on by default; `0` turns it off). A warm cache cut JSRF's title load
  gap on the Deck from 1132 ms to 330 ms.
- A USB pad source (`usb_gamepad_set_source`), so a title that reads its
  pads through the OHCI controller (BLiNX 1) gets slots, scripts, rumble
  and hot-plug.
- Opt-in XBE section paging (`RECOMP_XBE_PAGING`): non-preload sections
  load on `XeLoadSection`, as on the console, for titles whose sections do
  not fit in 64 MB at once (BLiNX 1).
- Opt-in features for debugging and benches: the APU DSP on the MIT
  dsp56300 JIT (`XBOXRECOMP_APU_DSP`, `RECOMP_APU_DSP`), the heap
  quarantine for freed guest blocks (`RECOMP_HEAP_QUARANTINE`), the
  event-driven KickOff wait (`kick_event`), the run-ahead walker with the
  PGRAPH notify interrupt (`run_ahead`), guest-priority scheduling under
  the guest lock (`guest_sched`), the in-process sampling profiler
  (`RECOMP_TRACE=prof`), flip-timed pad scripts and the pad recorder
  (`pad_record`).
- x86-64 Linux host groundwork: the toolkit runs natively there.

### Changed
- Merged upstream xboxrecomp v0.15.0 ("Second Reading") and v0.16.0
  ("Small Print"). The fork's host-pad table is now `pad_map`, so
  upstream's `input_map` keeps its name.
- Input bindings use upstream's names (`left_trigger`, `left_thumb`, SDL
  scancode key names). The fork's old names are refused and named in the
  log.
- POSIX hosts link SDL3 (3.2 or newer) instead of SDL2 or sdl2-compat:
  window, input, gamepad hot-plug and audio. SDL2's `SDL_AUDIODRIVER` and
  `SDL_VIDEODRIVER` are still accepted.
- `RECOMP_VBLANK` set to `0` or empty is off, like every other switch.
  Upstream v0.16 reads any set value as its plain tick; other values keep
  that meaning and `paced` is unchanged.
- Contiguous allocations stay off the XBE image's physical pages while
  there is room, as the console's kernel does
  (`RECOMP_CONTIG_AVOID_IMAGE=0` restores the old order).
- On macOS, `present.fullscreen` is a borderless window instead of a
  fullscreen Space.
- Metal and D3D11 keep their texture caches across a `render.scale`
  change, so a change no longer stalls while every texture uploads again.
- Cheaper pushbuffer decode and per-batch setup on D3D11 and Metal: the
  draw time of a heavy BLiNX 2 frame on the Deck went from about 18.5 to
  14.4 ms. Metal's runtime shaders compile as MSL 3.0.
- Host rumble on SDL hosts changes at most once per 100 ms; a stop is
  sent at once.

### Fixed
- Every guest thread has its own non-zero id, and oversized contiguous
  requests are placed top-down, as the console does (Test Drive: Eve of
  Destruction).
- Draws with the depth test off are no longer clipped at the far plane on
  Metal and D3D11.
- Sample data on pages the image names is read from the image, so JSRF's
  music no longer turns into a tone after a `render.scale` change.
- The fence field of register 0x400B10 sits where the title's D3D expects
  it (XDK 5558), so BLiNX 1 passes its first wait-for-idle.
- `KeSetBasePriorityThread` reaches the host thread (SSX 3's crackle on
  native Windows), but only where the host makes a priority exclusive
  (JSRF under Proton).
- APU voices: the frame a voice ends in is mixed, the last sample of a
  buffer plays, and the resampler holds no sample back.
- Late occlusion results amend a settled report (Burnout 3's sun flare
  on D3D11); Metal and D3D11 share one render-target read rule.
- Hor+ seams: perspective screen quads, letterbox band pieces and flat
  near layers.
- A switch arm after a `ret` is never taken for a gap function (SSX 3's
  memcpy).
- Under Proton without raw input, growing the window no longer traps the
  cursor re-centre fallback.
- The POSIX file backend's `O_APPEND` mapping: a `GENERIC_WRITE` handle no
  longer sends positioned writes to end of file.

## [blinx2-v0.6.0] - 2026-10-21

### Added
- Hor+ widescreen for 4:3 titles, opt-in through `display.aspect` (16:9,
  16:10 or 21:9; the default 4:3 is stock): the render target and the
  projection are padded so the 3D view widens, full-width 2D passes
  stretch, the HUD keeps its centred 4:3 layout and `display.movie` places
  movies. Square render targets are padded too, so screen-space
  reflections reach the new edges. Metal and D3D11 match; the CPU
  rasteriser ignores the key.
- `display.overscan = n` crops n guest columns from each side (and the
  matching rows) at present and zooms the rest, as a TV's overscan did.
  0 is stock; it is refused with a wide `display.aspect`.
- A static module table and a versioned mod API (v1) behind
  `XBOXRECOMP_ENHANCE`, so enhancement code such as a game's free camera
  hooks frames, input capture and exit without patching the runtime.
- The fixed-function vertex pipeline: the executor builds the equivalent
  vertex program from the fixed-function state (skinning, lighting,
  texgen, fog, point size) and caches it, so the CPU, Metal and D3D11
  paths all transform those batches. `RECOMP_PB_FF=0` turns it off.
- `RECOMP_AUDIO_THREAD_PRIORITY` raises the APU thread's priority where
  the host allows it; the thread is named `mcpx.apu_thread`.

### Changed
- Under Wine the XAudio2 source queue holds 6 buffers instead of 4, which
  removes the idle underruns on the Steam Deck. Native Windows keeps 4.
- Page-aligned allocations own whole pages and `NtAllocateVirtualMemory`
  reports the rounded size, as NT does; on hosts with pages larger than
  4 KB, guest page protection applies per guest page. The first heap
  exhaustion prints a report of live, slack and free memory.
- The GP stub's mixdown stages bins 6-9 at half and drops bin 10 (the
  reverb send), as the real GP mixes them; titles that do not use those
  bins are bit-identical.

### Fixed
- The sample-limit audio interrupt fires once per sequencer start instead
  of every 2048 samples, so a title's DirectSound service no longer
  restarts the DSPs many times a second (Jet Set Radio Future's first city
  went from 48 to 55.5 fps on macOS).
- ReadFileEx/WriteFileEx completion routines run at the issuing thread's
  next alertable wait, as on hardware, and a manual-reset event set or
  pulsed releases every waiter that started before it on POSIX hosts.
- The colour clear value is read in the colour surface's format when the
  clear runs, so 16-bit targets clear to the right colour.
- Output-buffer scatter-gather entries go to the GP FIFO table instead of
  overwriting the voice table, which cut gaps into streamed audio.
- USB: the new-game template brings up the OHCI model, and a title that
  links its own XAPI gets its pad, with physical addresses resolved as
  XAPI meant them.

## [blinx2-v0.5.1] - 2026-10-17

### Added
- Reproducible builds, opt-in per game through `cmake/reproducible.cmake`:
  file prefix maps for the game, the toolkit, the build tree and the
  llvm-mingw root, no link timestamp and a stable PDB, so the same
  sources and options give the same bytes in any folder. Packagers use
  this to ship patch updates with only the files that changed.

### Fixed
- Texture-shader CLIPPLANE stages now kill pixels on the Metal and D3D11
  backends, as the CPU rasteriser already did; before, user clip planes
  cut nothing on the GPU paths.

## [blinx2-v0.5.0] - 2026-10-17

### Changed
- Merged upstream xboxrecomp v0.14.0 ("Other Machines"): lifter fixes
  (`lock xadd` flags, FCMOV, MXCSR rounding, x87 trig, `in`/`out`, `neg`),
  the APU DSP-ack gate and device-queue stall fallback, arm64 MMIO decoder
  hardening, OHCI split access and upstream's POSIX tests. The fork keeps
  its own pushbuffer executor and Metal/D3D11 backends, its keyboard and
  mouse layer (upstream's Win32-only `RECOMP_MKB` is not carried) and
  `recomp_env` for every knob.
- The disassembler's data-pointer pass from v0.14.0 is fast again:
  probes decode a batch of instructions at a time, share one decode cache
  and memoize tail proofs. Output is byte-identical; BLiNX 2 had gone from
  30 s to more than 35 minutes.
- `xbox_InputGetState` is now `xbox_InputGetPadState` (one host pad, no
  keyboard or mouse) plus `xbox_InputMergeKbm`. A game that maps host
  slots to guest ports itself can read one pad alone; games that read
  slot N as port N are unchanged.

### Added
- `RECOMP_MISSING_FILES` and `RECOMP_MISSING_FILES_LIST`, upstream's names
  for the missing-file report, are accepted as old names of
  `RECOMP_TRACE=missing` and `RECOMP_DEBUG=missing_list`.
- SDL game controller hot-plug (macOS, Linux): a pad connected after
  launch is picked up, and a replugged pad takes its old port back. Ports
  never shift. Windows/XInput is unchanged.
- `RECOMP_FALLOFF_TAIL`: a lifted body that runs off its end into code no
  function claims now dispatches to that address and logs one miss per
  site, instead of returning silently. Under `RECOMP_ABI_CHECK`, short
  esp returns are reported per call site.
- Debug dumps for vertex programs: `vsh_dump` now works on every backend;
  `fb_dump_prog` dumps the flips around the first draw of named programs
  (`fb_dump_prog_before`, `fb_dump_prog_after`); `vsh_const_dump` writes
  each program batch's 192 constant rows (`vsh_const_flips`,
  `vsh_const_prog`). All are off by default.
- New ctests: `pb_clip_border`, `pb_stride0`, `apu_vp`,
  `kernel_file_handles`, `input_hotplug_sdl`, `nv2a_vsh_cdump`,
  `nv2a_fb_ring` and `recomp_env_list`.

### Fixed
- The disassembler refuses a weak function start (a call or tail target,
  an alias) that falls inside an instruction of the body in front of it.
  BLiNX 2 had one such start from a switch table that decoded as a call;
  the cut body returned 4 bytes short on the stack and the game hung at
  the Tom Tom rank exam.
- macOS: a Finder launch starts with a limit of 256 open files, and
  BLiNX 2 keeps 363 cache files open, so it crashed. The runtime raises
  the soft limit at startup, file handles are checked before use (a bad
  or closed handle gives `STATUS_INVALID_HANDLE`, not a crash), and
  running out of descriptors gives `STATUS_TOO_MANY_OPENED_FILES`.
- A vertex attribute with stride 0 is an array whose one element every
  vertex reads, not an absent attribute. SSX 3's rider vanished about a
  minute into a race.
- Colour clears use the clear rectangle alone, clamped to the surface,
  so a clear no longer wipes other parts of the same surface. The CPU
  rasteriser clips at the near plane with the depth test off too. The
  texture border colour (address mode 4) is sampled on D3D11, Metal and
  the CPU rasteriser.
- APU: the voice processor holds the mix while a queued voice is locked,
  as xemu does, so a title's half-updated voice no longer plays as a
  burst. ADPCM decode buffers hold 18 blocks and wider voices are
  refused, so a wide block no longer overruns the stack (upstream #217).
- A title's `E:\CACHE` (`\Device\Harddisk0\Partition1\CACHE`) now goes
  to `<save_dir>/TitleCache` (or under `RECOMP_SAVE_DIR`), not
  `<save_dir>/CACHE`. On Windows, Proton and macOS that was the same folder
  as `Cache`, where Z: and Partition3-5 (the cache partitions behind X:, Y:
  and Z:) go, so the files could mix and collide. Files left in the old
  folder are not moved; the title rebuilds its cache.

## [blinx2-v0.4.0] - 2026-10-15

### Added
- `render.aniso` (`RECOMP_RENDER_ANISO`): anisotropic filtering on
  mipmapped textures: 0 or 1 is stock (off), 2..16 the anisotropy; values
  outside 0..16 are clamped. Read at startup, on Direct3D 11 and Metal.
  The D3D11 side is by [@hgeorgilas](https://github.com/hgeorgilas).
- BUMPENVMAP and BUMPENVMAP_LUMINANCE texture stages on Direct3D 11 and
  Metal. The D3D11 side is by [@hgeorgilas](https://github.com/hgeorgilas).
  `render.aniso` leaves the stock filter on a texture stage that a
  BUMPENVMAP stage reads its offsets from; filtering it anisotropically
  turned BLiNX 2's far sea into noise. The CPU rasteriser still samples
  these modes as plain 2D.
- `RECOMP_TRACE=bump`: one `[BUMP]` line per distinct BUMPENVMAP stage
  setup (stage, input, matrix, scale, offset, formats) on both GPU
  backends.
- `RECOMP_GAME_FILES` (game files directory) and `RECOMP_HDD_DIR`
  (emulated hard disk) are now defined by the toolkit. A game that defined
  them itself must keep its rows under `#ifndef RECOMP_ENV_HAVE_FOLDER_ROWS`.
  The new-game template loads the enhancements layer, sets the window
  title and reads these keys.
- `RECOMP_DEBUG=occ_settle_us=<us>`: the per-frame bound on waiting for
  occlusion results (default 8000); 0 restores the old no-wait path.
- New ctests: `nv2a_bump`, `occ_reports`, `kernel_display_mode`,
  `kernel_path` and `fb_window_wanted`.

### Fixed
- The present size no longer alternates between the back buffers' clip
  sizes. Burnout 3's race-start card switched between 640x480 and 640x464
  every flip, so any scaled present jumped about 20 px. Recently presented
  buffers stay in the size scan, and the present uses the display mode's
  frame, so menus and races present at the same size.
- Visibility (occlusion) reports settle before the title reads them: at
  the end of each pushbuffer walk, before semaphore releases, and at the
  flip, waiting at most 8 ms a frame. Titles that skip draws on a stale
  report no longer drop them (BLiNX 2's characters on the Story Mode menu,
  Burnout 3's race-start flicker).
- `\Device\Harddisk0\Partition1\CACHE` (a title's `E:\CACHE`) resolves
  under the user-data root beside UDATA and TDATA (`RECOMP_SAVE_DIR`),
  not the game files directory, which a title could write into. Path rules
  match only at a path-component boundary.
- Windows: no GDI framebuffer window opens beside the Direct3D 11 backend.

## [blinx2-v0.3.2] - 2026-10-15

### Added
- The new-game template turns vblank and title kevents on, with a fence
  mirror example.
- Texture formats R8B8 (0x29) and LIN_R8B8 (0x16).
- The disassembler warns about call targets in code sections it did not
  sweep.
- The recompiler warns when a function body ends on `leave` / `pop ebp`
  with nothing after it (a likely cut-off function).
- New ctests: `host_nap`, `pb_semaphore` and `pgraph_fence`.

### Fixed
- Semaphore and report offsets are applied relative to the bound context
  DMA object, not as physical addresses, so a title whose fence object is
  not at address 0 no longer hangs in D3D's fence wait.
- With no fence mirror registered, PGRAPH 0x400B10 returns the executor's
  last semaphore release, so such titles no longer spin in D3D's fence
  wait. A registered mirror is never overridden.
- `RECOMP_VBLANK=0` now turns vblank delivery off (unset stays off).
- The disassembler and recompiler decode a code section up to its virtual
  end, not only its file bytes.
- Native Windows: the pushbuffer kick wait spins, then takes a
  high-resolution timer, instead of sleeping a whole timer tick, so busy
  scenes no longer drop frames (BLiNX 2's mission 1-2 city view fell to
  25 fps). `RECOMP_DEBUG=kick_nap=hybrid|timer|delay|spin` overrides the
  mode; Proton and POSIX hosts are unchanged.
