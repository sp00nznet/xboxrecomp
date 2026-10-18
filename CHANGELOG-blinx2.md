# blinx2/portability changelog

This branch is apexJCL's fork of xboxrecomp that BLiNX 2 and Burnout 3 build
against: upstream plus POSIX hosts (macOS, Linux), a Metal backend and fixes
that have not reached upstream yet. Its releases are the `blinx2-vX.Y.Z` tags;
upstream's own versions are listed in the README's Changelog. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Entries start at
blinx2-v0.3.2; earlier tags are described by their commits.

## [Unreleased]

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
