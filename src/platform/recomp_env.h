/*
 * recomp_env.h - every environment variable the runtime reads, in one table.
 *
 * Three tiers:
 *
 *   config  A small set of documented, user-facing knobs, each its own
 *           variable: RECOMP_PB_BACKEND=d3d11, RECOMP_SAVE_DIR=..., ...
 *   trace   Log and report toggles, one comma list:
 *               RECOMP_TRACE=flip,tex=2,heap=0x80123000
 *   debug   Debug hacks, A/B switches, experiments, dumps and watchpoints,
 *           one comma list:
 *               RECOMP_DEBUG=pb_fast=0,fb_dump=/tmp/f_,fb_dump_at=61,121
 *
 * A list entry is `key` (value "1") or `key=value`. A later entry for the
 * same key wins. Values may contain commas: a fragment that does not start
 * with a known key continues the value before it, so `fb_dump_at=61,121`
 * and `peek_chain=0x1315A8,8,0x10,0` read as one value each.
 * RECOMP_TRACE=help or RECOMP_DEBUG=help prints the table.
 *
 * The environment is read once (recomp_env_init, or lazily on the first
 * lookup) and cached, so a lookup is an array load: safe in hot paths.
 * Values keep getenv's meaning: NULL when unset, otherwise the string.
 *
 * Transition: every variable these replace still works through an alias,
 * which prints one deprecation line naming the new spelling.
 * RECOMP_TRACE=help and RECOMP_DEBUG=help print the table; a title's own
 * environment document lists every key with its meaning and defaults.
 */
#ifndef RECOMP_ENV_H
#define RECOMP_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

/* A game adds its own keys, same row format, by defining
 * RECOMP_ENV_HAVE_GAME_KEYS on the recomp_env target (PUBLIC, so every user
 * agrees on the table) and putting a recomp_env_game.h on its include path
 * that defines RECOMP_ENV_GAME_KEYS(X). Its rows come after the toolkit's, so
 * the toolkit's ids are the same with or without them. A game key whose value
 * may contain commas is named in RECOMP_ENV_GAME_COMMA_KEYS, a string list:
 *   #define RECOMP_ENV_GAME_COMMA_KEYS "mem_dump",
 * A game can also give a toolkit key a default, used when neither its new
 * nor its old spelling is set, with RECOMP_ENV_GAME_DEFAULTS(D):
 *   #define RECOMP_ENV_GAME_DEFAULTS(D) D(APU_DSP_ACK, "auto")
 */
/* GAME_FILES and HDD_DIR (RECOMP_GAME_FILES, RECOMP_HDD_DIR) are toolkit
 * rows since b3-public-release; BLiNX 2 and Burnout 3 defined them as game
 * keys before. A key defined twice does not compile, and a game's public
 * build pins an older toolkit tag while its local build uses the toolkit
 * checkout, so the game header keeps its two rows under
 * #ifndef RECOMP_ENV_HAVE_FOLDER_ROWS until its pin is past this change.
 * Defined before the game header is included, for that test. */
#define RECOMP_ENV_HAVE_FOLDER_ROWS 1
/* RUMBLE (RECOMP_RUMBLE) is a toolkit row since v0.16, read by the OHCI pad
 * model; a game that defined it as its own key keeps that row under
 * #ifndef RECOMP_ENV_HAVE_RUMBLE_ROW, as above. */
#define RECOMP_ENV_HAVE_RUMBLE_ROW 1
/* PAD_CLOCK and PAD_QUIT: a game that gives them defaults tests
 * this first, since its public pin may predate them. */
#define RECOMP_ENV_HAVE_PAD_CLOCK_ROW 1

#ifdef RECOMP_ENV_HAVE_GAME_KEYS
#include "recomp_env_game.h"
#endif
#ifndef RECOMP_ENV_GAME_KEYS
#define RECOMP_ENV_GAME_KEYS(X)
#endif

/* The opt-in enhancements layer (src/enhance/, XBOXRECOMP_ENHANCE=ON) adds
 * its config-tier keys by defining RECOMP_ENV_HAVE_ENHANCE_KEYS on this
 * target. They mirror keys of the configuration files (config.toml over
 * config.default.toml) and win over them: the variable is RECOMP_ + the
 * dotted key, upper case, '.' -> '_'.
 * Without the layer they do not exist, so the table is what it always was. */
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
#define RECOMP_ENV_ENHANCE_KEYS(X) \
    X(CONFIG,            CONFIG, "RECOMP_CONFIG",            "RECOMP_ENHANCE_CONFIG", "configuration: the user's config.toml (a path), or none for no file at all; default <root>/<title>/config.toml, <root>/config/config.toml; config.default.toml beside the executable is read under it") \
    X(RENDER_SCALE,      CONFIG, "RECOMP_RENDER_SCALE",      NULL, "enhancements: render.scale, internal resolution factor (1 = stock)") \
    X(RENDER_ANISO,      CONFIG, "RECOMP_RENDER_ANISO",      NULL, "enhancements: render.aniso, anisotropic filtering on mipmapped textures, 0/1 (stock) or 2..16") \
    X(RENDER_SNAP_2D,    CONFIG, "RECOMP_RENDER_SNAP_2D",    NULL, "enhancements: render.snap_2d, above render.scale 1 2D batches sample textures on the guest grid, as at 1x (no seams between tiles, blockier 2D art) (1/0, default 0)") \
    X(DISPLAY_ASPECT,    CONFIG, "RECOMP_DISPLAY_ASPECT",    NULL, "enhancements: display.aspect, 4:3 (stock), 16:9, 16:10, 21:9") \
    X(DISPLAY_MOVIE,     CONFIG, "RECOMP_DISPLAY_MOVIE",     NULL, "enhancements: display.movie, the sides of a 4:3 movie or still on a wide display.aspect: black (default, pillarbox), fit (stretched to the width), fill (scaled to cover, top and bottom cropped)") \
    X(DISPLAY_HUD_ASPECT, CONFIG, "RECOMP_DISPLAY_HUD_ASPECT", NULL, "enhancements: display.hud_aspect, how wide the HUD reaches under a HUD mod on a wide display.aspect: display (default, the frame's edges) or W:H from 4:3 up") \
    X(DISPLAY_HUD_MARGIN, CONFIG, "RECOMP_DISPLAY_HUD_MARGIN", NULL, "enhancements: display.hud_margin, guest px a HUD mod's edge elements are pulled in from the HUD's edge, 0 (default) to 64") \
    X(DISPLAY_OVERSCAN,  CONFIG, "RECOMP_DISPLAY_OVERSCAN",  NULL, "enhancements: display.overscan, guest px cropped from each side edge at present (top and bottom in proportion), 0 (stock) to 64; ignored with a wide display.aspect") \
    X(PRESENT_FILTER,    CONFIG, "RECOMP_PRESENT_FILTER",    NULL, "enhancements: present.filter, nearest (stock), linear, integer") \
    X(PRESENT_FULLSCREEN, CONFIG, "RECOMP_PRESENT_FULLSCREEN", NULL, "enhancements: present.fullscreen, borderless desktop fullscreen at start (1/0)") \
    X(PRESENT_PACING,    CONFIG, "RECOMP_PRESENT_PACING",    NULL, "enhancements: present.pacing, sleep (default: lowered spin waits sleep) or spin (stock busy-wait, the console-original timing)") \
    X(PRESENT_SURFACE,   CONFIG, "RECOMP_PRESENT_SURFACE",   NULL, "enhancements: present.surface, auto (default), window, internal") \
    X(PRESENT_SHADER,    CONFIG, "RECOMP_PRESENT_SHADER",    NULL, "enhancements: present.shader, none (stock), crt-lottes, crt-lottes-fast, crt-hyllian, fsr, or a .slangp path") \
    X(PRESENT_PROFILE,   CONFIG, "RECOMP_PRESENT_PROFILE",   NULL, "enhancements: present.profile, a shader profile's file name (config/shader-profiles/<name>.toml), or none; RECOMP_PRESENT_SHADER wins over it") \
    X(PRESENT_SHADER_LIB, CONFIG, "RECOMP_PRESENT_SHADER_LIB", NULL, "enhancements: present.shader_lib, the librashader library path (default: beside the executable)") \
    X(HOST_GAMESCOPE,    CONFIG, "RECOMP_HOST_GAMESCOPE",    NULL, "host: 1 when the launcher runs the game under gamescope (Steam Game Mode); present.surface=auto then means internal") \
    X(TEXTURES_PACK,     CONFIG, "RECOMP_TEXTURES_PACK",     NULL, "enhancements: textures.pack, a texture pack file (host-side texture replacements; unset = stock)") \
    X(TEXTURES_FONT,     CONFIG, "RECOMP_TEXTURES_FONT",     NULL, "enhancements: textures.font, the pack's font-atlas entries, on (default) or off") \
    X(TEXTURES_TEXT,     CONFIG, "RECOMP_TEXTURES_TEXT",     NULL, "enhancements: textures.text, the pack's baked-text entries, on (default) or off") \
    X(INPUT_CHORD_DELAY_MS, CONFIG, "RECOMP_INPUT_CHORD_DELAY_MS", NULL, "enhancements: input.chord_delay_ms, ms a host-action chord member is held back from the game after its press, 0 (default) to 100") \
    X(OVERLAY_STATS,     CONFIG, "RECOMP_OVERLAY_STATS",     NULL, "enhancements: overlay.stats, the stats strip (fps, frame ms, pacing, scale, backend) in the window's top right corner (1/0, default 0)") \
    X(OVERLAY_UI_SCALE,  CONFIG, "RECOMP_OVERLAY_UI_SCALE",  NULL, "enhancements: overlay.ui_scale, the menu's and the overlays' size over the automatic scale, 0.75 to 2 (default 1)") \
    X(OVERLAY_UI_FIT,    CONFIG, "RECOMP_OVERLAY_UI_FIT",    NULL, "enhancements: overlay.ui_fit, what the overlay's automatic scale follows: display (default: the output height, never under the OS's UI scale or a handheld's density) or output (the output height alone)") \
    X(MENU,              CONFIG, "RECOMP_MENU",              NULL, "enhancements: menu.enabled, the in-game enhancements menu opened by the menu.open action (F10, guide+a, paddle1) (1/0, default 0)") \
    X(MODS_DESCRIBE,     CONFIG, "RECOMP_MODS_DESCRIBE",     NULL, "mods: json writes every mod's descriptor and resolved settings and bindings as JSON (schema 1) before the game starts, then exits; no mod init runs. Any other value is logged and ignored") \
    X(MODS_DESCRIBE_OUT, CONFIG, "RECOMP_MODS_DESCRIBE_OUT", NULL, "mods: the file RECOMP_MODS_DESCRIBE=json writes (replaced); unset = stdout. A Windows or Proton launcher needs it: the game has no stdout there")
#else
#define RECOMP_ENV_ENHANCE_KEYS(X)
#endif

/* X(ID, TIER, NAME, LEGACY, HELP)
 *   CONFIG: NAME is the variable; LEGACY an older variable, or NULL.
 *   TRACE / DEBUG: NAME is the key in RECOMP_TRACE / RECOMP_DEBUG; LEGACY
 *   the variable it replaces (an alias for the transition window). */
#define RECOMP_ENV_KEYS(X) \
    /* ── config ─────────────────────────────────────────────────────── */ \
    X(PB_EXEC,           CONFIG, "RECOMP_PB_EXEC",           NULL, "pushbuffer executor (draws frames); default on, 0: off") \
    X(PB_BACKEND,        CONFIG, "RECOMP_PB_BACKEND",        NULL, "executor backend: cpu (default), metal, d3d11, null") \
    X(HEADLESS,          CONFIG, "RECOMP_HEADLESS",          NULL, "1: no window (SDL host); the template's Windows fatal-error dialog is skipped too") \
    X(WINDOW_SCALE,      CONFIG, "RECOMP_WINDOW_SCALE",      NULL, "window scale factor (SDL host and the Win32 framebuffer window, 1-8, default 2)") \
    X(FB_FULLSCREEN,     CONFIG, "RECOMP_FULLSCREEN",        NULL, "Win32 framebuffer window: 1 starts in borderless fullscreen (F11 / Alt+Enter toggle)") \
    X(D3D_PRESENT,       CONFIG, "RECOMP_D3D_PRESENT",       NULL, "Win32 framebuffer window: 1 presents through D3D11 instead of GDI (falls back to GDI on failure)") \
    X(CLOSE_KEEPS_RUNNING, CONFIG, "RECOMP_CLOSE_KEEPS_RUNNING", NULL, "Win32 framebuffer window: 1 closing the window only closes it; default ends the process") \
    X(PRESENT_VSYNC,     CONFIG, "RECOMP_PRESENT_VSYNC",     NULL, "vsync: SDL host 0 = off (default on); D3D11 1 = on (default off)") \
    X(WINDOW_QUIT_AFTER, CONFIG, "RECOMP_WINDOW_QUIT_AFTER", NULL, "close the window after this many seconds") \
    X(FB_WINDOW,         CONFIG, "RECOMP_FB_WINDOW",         NULL, "Windows: show the framebuffer window (not beside the d3d11 backend)") \
    X(GAME_FILES,        CONFIG, "RECOMP_GAME_FILES",        NULL, "game files directory; the launcher sets it, the game's main.c has the default") \
    X(HDD_DIR,           CONFIG, "RECOMP_HDD_DIR",           NULL, "emulated hard disk: partition images, saves, caches (default: <data>/xboxrecomp/<title id>)") \
    X(SAVE_DIR,          CONFIG, "RECOMP_SAVE_DIR",          NULL, "root for the title's UDATA/TDATA/CACHE") \
    X(DXBC_CACHE,        CONFIG, "RECOMP_DXBC_CACHE",        NULL, "D3D11 compiled-shader (DXBC) cache: on (default, <hdd dir>/ShaderCache/dxbc), 0/off/false/no: off, or an absolute directory") \
    X(AC97_READY,        CONFIG, "RECOMP_AC97_READY",        NULL, "emulated APU and audio; default on (x86-64 Windows/Proton, macOS/Linux arm64), 0: off") \
    X(VBLANK,            CONFIG, "RECOMP_VBLANK",            NULL, "vblank interrupt: unset, empty or 0 off (default); paced the 60 Hz schedule with the KickOff ack watch; any other value the plain tick, which becomes the schedule when RECOMP_DISPATCH_GATE or RECOMP_FLIP_PACING is on") \
    X(TITLE_KEVENTS,     CONFIG, "RECOMP_TITLE_KEVENTS",     NULL, "1: a KEVENT the title builds itself gets a host event (upstream, default off)") \
    X(FILE_APC_DEFER,    CONFIG, "RECOMP_FILE_APC_DEFER",    NULL, "1: run a file I/O APC at the issuing thread's next alertable wait, as the kernel does; 0: inside the request (inline); default inline unless the title calls xbox_SetDeferredIoApc") \
    X(GUEST_LOCK,        CONFIG, "RECOMP_GUEST_LOCK",        NULL, "1: one guest thread runs guest code at a time, released in kernel calls (upstream, default off)") \
    X(DISPATCH_GATE,     CONFIG, "RECOMP_DISPATCH_GATE",     NULL, "1: the one-CPU DISPATCH gate: device interrupts are posted to the holder and run at its safe points, and queued DPCs run on the guest thread at its raise and release (default off)") \
    X(FLIP_PACING,       CONFIG, "RECOMP_FLIP_PACING",       NULL, "1: the walker and the KickOff ack hold after a flip until the guest vblank reaches the swap's interval, and a frame counter the title moves itself is left to it; edge: D3D's rule, a late frame waits for the next vblank; 0 off; unset is off (a FLIP_STALL completes at once and registered frame counters are bumped on every flip), except in the deterministic mode (RECOMP_DETERMINISTIC), where it is on") \
    X(GUEST_CPUS,        CONFIG, "RECOMP_GUEST_CPUS",        NULL, "one: the title's threads on one host core, as on the console; n: that core; unset or all: every core (default)") \
    X(CPUID_NO_SIMD,     CONFIG, "RECOMP_CPUID_NO_SIMD",     NULL, "1: cpuid hides MMX, FXSR and SSE, for a title whose SIMD paths do not lift correctly (default: reported, as on the console's P3)") \
    X(CONTIG_AVOID_IMAGE, CONFIG, "RECOMP_CONTIG_AVOID_IMAGE", NULL, "0 (or off, false, no): contiguous allocations fill from the bottom through the loaded image's physical range, the old order (default on, any other value: they stay out of it while there is room and never take an image page the title named, as the console's kernel keeps image pages out of its pool)") \
    X(RUMBLE,            CONFIG, "RECOMP_RUMBLE",            NULL, "0: ignore the title's rumble reports (default: forwarded to the host pad)") \
    X(APU_HRTF,          CONFIG, "RECOMP_APU_HRTF",          NULL, "0: no HRTF filter on 3D voices (default on)") \
    X(HEAP_RECLAIM,      CONFIG, "RECOMP_HEAP_RECLAIM",      NULL, "1: on, the heap splits and merges freed blocks (upstream); 0 or empty: off; a game may default it on") \
    X(HEAP_QUARANTINE,   CONFIG, "RECOMP_HEAP_QUARANTINE",   NULL, "=KiB: freed heap blocks held back from reuse, oldest first, up to that much; a larger block is freed at once (8192 suggested); 0 or empty: off") \
    X(XBE_PAGING,        CONFIG, "RECOMP_XBE_PAGING",        NULL, "1: XBE sections not flagged preload load on the title's XeLoadSection, as on the console, and the mapping grows so the heap sits above the image; 0: every section at boot; default: the title's choice (xbox_SetSectionPaging), else off") \
    X(EXT_VMA,           CONFIG, "RECOMP_EXT_VMA",           NULL, "set: honour reservations at fixed addresses above the RAM mirrors (upstream, default off)") \
    X(STORAGE_RAM_CACHE, CONFIG, "RECOMP_STORAGE_RAM_CACHE", NULL, "the cache partition (Z:, Partition3-5) lives in memory, capped at the stock 750 MB, seeded read-only from <hdd>/Cache and never written back; when that seed overflowed the cap, seeds neither opened nor listed are dropped when a write needs room; on by default, 0/false/no/off keeps it on disk at <hdd>/Cache; config storage.ram_cache (default true)") \
    X(SYSTEM_LANGUAGE,   CONFIG, "RECOMP_SYSTEM_LANGUAGE",   NULL, "the dashboard language the title reads (XC_LANGUAGE): en (default, stock), ja, de, fr, es, it, ko, zh, pt; a code the game does not ship (its game.toml [system] languages) gives English; read before the guest starts; config system.language") \
    X(KEYBOARD,          CONFIG, "RECOMP_KEYBOARD",          NULL, "1: keyboard drives pad 1") \
    X(MOUSE,             CONFIG, "RECOMP_MOUSE",             NULL, "1: the mouse drives pad 1 (a stick and buttons); 0: off") \
    X(PAD_DEADZONE,      CONFIG, "RECOMP_PAD_DEADZONE",      NULL, "stick deadzone, 0..32767 (default 0); input.pad_deadzone_left/_right (percent) win over it") \
    X(STEAM_GAME_ID,     CONFIG, "SteamGameId",              NULL, "set by Steam, read only: Steam launched the game; on the XInput backend (Windows, Proton) Steam Input may then stand between the pad and the game (xbox_InputSteamInput: maybe)") \
    X(STEAM_DECK,        CONFIG, "SteamDeck",                NULL, "set by Steam, read only: 1 on a Steam Deck; on the XInput backend Steam Input may then stand between the pad and the game (xbox_InputSteamInput: maybe)") \
    X(STDIO_LOG,         CONFIG, "RECOMP_STDIO_LOG",         NULL, "send stdout/stderr to this file") \
    X(LOG_LEVEL,         CONFIG, "RECOMP_LOG_LEVEL",         "XBOX_LOG_LEVEL", "kernel log level, 0 (error) .. 4 (trace)") \
    X(AUDIO_BUF_SAMPLES, CONFIG, "RECOMP_AUDIO_BUF_SAMPLES", NULL, "audio output buffer size in samples") \
    X(AUDIO_BUF_COUNT,   CONFIG, "RECOMP_AUDIO_BUF_COUNT",   NULL, "audio output buffer count (default 4; XA2_NUM_BUFS_WINE under Wine)") \
    X(AUDIO_THREAD_PRIORITY, CONFIG, "RECOMP_AUDIO_THREAD_PRIORITY", NULL, "audio: APU frame thread priority (normal, highest; Windows only)") \
    X(APU_DSP,           CONFIG, "RECOMP_APU_DSP",           NULL, "audio: 1 (default in builds with XBOXRECOMP_APU_DSP and its device) runs the APU's GP and EP on the DSP56300 JIT, forcing apu_dsp_ack off and ignoring apu_mixdown_all; the stereo output is the GP's mixbuf bins 0/1; 0 or empty: the stub (the default in other builds, where an explicit 1 keeps the stub with a line)") \
    X(CMDLINE,           CONFIG, "RECOMP_CMDLINE",           NULL, "launch-data command line handed to the title") \
    X(FMV_HOST,          CONFIG, "RECOMP_FMV_HOST",          NULL, "play the title's movies through the host player") \
    X(RASTER_THREADS,    CONFIG, "RECOMP_RASTER_THREADS",    NULL, "CPU raster threads, 1..16 (default: cores - 4)") \
    /* ── trace: RECOMP_TRACE=key[=value],... ─────────────────────────── */ \
    X(FLIP_LOG,          TRACE, "flip",            "RECOMP_FLIP_LOG",          "one line per flip, machine-readable") \
    X(PRESENT_TRACE,     TRACE, "present",         "RECOMP_PRESENT_TRACE",     "present-surface choices") \
    X(PRESENT_STATS,     TRACE, "present_stats",   "RECOMP_PRESENT_STATS",     "SDL host present statistics") \
    X(METAL_PROF,        TRACE, "metal_prof",      NULL,                       "Metal flip cost every 300 flips: GPU wait, readbacks, write-back, slot blit, hand-off, GPU time") \
    X(METAL_TRACE,       TRACE, "metal",           NULL,                       "Metal render-target events: each decode sync and stale drop, with target, texture and flip; each new pitch-alias pair once") \
    X(METAL_SHADERS,     TRACE, "metal_shaders",   NULL,                       "Metal shader and pipeline builds: each MSL library and pipeline state with its ms, and a [metal] build line for every flip that built one or uploaded textures for 5 ms or more") \
    X(VSH_TRACE,         TRACE, "vsh",             "RECOMP_VSH_TRACE",         "vertex programs and their batches") \
    X(ASPECT_TRACE,      TRACE, "aspect",          NULL,                       "display.aspect: the 2D batches of each vertex program, texture and placement, once each ([aspect.program]), and the padded target per flip") \
    X(HUD_TRACE,         TRACE, "hud",             NULL,                       "the HUD layer (hud_anchor.h): each site's element, group, kind and shift ([hud.site]), unlisted sites drawn past the 4:3 columns, skewed and skipped calls, the canvas and the lift") \
    X(VSH_ZLOG,          TRACE, "zlog",            "RECOMP_VSH_ZLOG",          "depth clears and z-buffer use") \
    X(TEX_LOG,           TRACE, "tex",             "RECOMP_TEX_LOG",           "texture format survey; =2 also every texture") \
    X(TEX_STATE,         TRACE, "tex_state",       "RECOMP_TEX_STATE",         "texture-state methods seen, in the report") \
    X(BUMP_TRACE,        TRACE, "bump",            NULL,                       "[BUMP] line per distinct BUMPENVMAP stage setup (stage, input, matrix as stored, scale, offset, formats), with the flip; GPU backends") \
    X(TEX_PACK_TRACE,    TRACE, "tex_pack",        NULL,                       "texture pack misses every 600 flips: hash, size, format, count, candidate") \
    X(CLIP_TRACE,        TRACE, "clip",            "RECOMP_CLIP_TRACE",        "near-plane clipping, per triangle") \
    X(ZPASS_TRACE,       TRACE, "zpass",           "RECOMP_ZPASS_TRACE",       "occlusion report writes") \
    X(PB_EXEC_VERBOSE,   TRACE, "pb_verbose",      "RECOMP_PB_EXEC_VERBOSE",   "executor bring-up detail") \
    X(SURF_TRACE,        TRACE, "surf",            "RECOMP_SURF_TRACE",        "=offset: surface methods from that colour offset") \
    X(PB_SEMA_TRACE,     TRACE, "sema",            "RECOMP_PB_SEMA_TRACE",     "every semaphore release") \
    X(PB_UNHANDLED_ALL,  TRACE, "unhandled_all",   "RECOMP_PB_UNHANDLED_ALL",  "list every unhandled method, not ten") \
    X(PB_SCAN,           TRACE, "pb_scan",         "RECOMP_PB_SCAN",           "pushbuffer survey and the periodic [GPU]/[PB] report") \
    X(NV2A_TRACE,        TRACE, "nv2a",            "RECOMP_NV2A_TRACE",        "NV2A poll: a [NV2A] DMA_PUT line per kick (costly on Windows), PCRTC_START, fb checksum, periodic report") \
    X(PB_WRAP_TRACE,     TRACE, "pb_wrap",         "RECOMP_PB_WRAP_TRACE",     "pushbuffer wraps") \
    X(HEAP_TRACE,        TRACE, "heap",            "RECOMP_HEAP_TRACE",        "=va: heap blocks covering that address") \
    X(FILE_TRACE,        TRACE, "file",            NULL,                       "[READ]/[WRITE] records per NtReadFile/NtWriteFile call: caller, buffer, offset, length, status and the first bytes") \
    X(KERNEL_SUMMARY,    TRACE, "kernel_summary",  NULL,                       "the 2 s [KERNEL] summary block: cumulative calls, the ordinal the tick landed on, esp, the six most-called ordinals") \
    X(KERNEL_LOG_BUDGET, TRACE, "kernel_budget",   "RECOMP_KERNEL_LOG_BUDGET", "=n: kernel log lines per call site (200)") \
    X(TRACE_BUDGET,      TRACE, "call_budget",     "RECOMP_TRACE_BUDGET",      "=n: call-trace line budget (400000)") \
    X(TRACE_ARGS,        TRACE, "call_args",       "RECOMP_TRACE_ARGS",        "=n: call trace prints n stack args") \
    X(TRACE_DEREF,       TRACE, "call_deref",      "RECOMP_TRACE_DEREF",       "call trace follows pointer args") \
    X(TRACE_PROFILE,     TRACE, "call_profile",    "RECOMP_TRACE_PROFILE",     "call profile; =n report interval") \
    X(IRQL_TRACE,        TRACE, "irql",            "RECOMP_IRQL_TRACE",        "first IRQL transitions") \
    X(APU_TRACE,         TRACE, "apu",             "RECOMP_APU_TRACE",         "APU register and frame trace; [APU-IRQ] line every 5 s") \
    X(AUDIO_HOST,        TRACE, "audio_host",      NULL,                       "host audio playback vs wall clock; [AUDIO-HOST] starve lines (on with apu)") \
    X(APU_LOCK_TRACE,    TRACE, "apu_lock",        NULL,                       "APU model lock: [APU-LOCK] hold and wait histograms per holder every 5 s") \
    X(APU_RING,          TRACE, "apu_ring",        NULL,                       "looping APU buffers refilled by the title: [APU-RING] lead and stale-lap replays") \
    X(USB_TRACE,         TRACE, "usb",             "RECOMP_USB_TRACE",         "OHCI trace") \
    X(USB_STATS,         TRACE, "usb_stats",       "RECOMP_USB_STATS",         "OHCI summary line every 5 s") \
    X(INPUT_DIAG,        TRACE, "input_diag",      "RECOMP_INPUT_DIAG",        "input chain probe once a second") \
    X(KEY_TRACE,         TRACE, "key",             "RECOMP_KEY_TRACE",         "every key and mouse edge by binding name (first 200, then 1 in 50)") \
    X(MOUSE_TRACE,       TRACE, "mouse",           NULL,                       "mouse model per poll: counts in, stick out (first 200)") \
    X(TITLE_STATS,       TRACE, "title",           NULL,                       "window title also shows FPS and draws, once a second") \
    X(CS_TRACE_CRT,      TRACE, "cs_crt",          "RECOMP_CS_TRACE_CRT",      "CRT critical sections; =all every one") \
    X(CS_WATCH,          TRACE, "cs_watch",        "RECOMP_CS_WATCH",          "=va: critical section at that address") \
    X(KERNEL_WATCH,      TRACE, "kernel_watch",    "RECOMP_KERNEL_WATCH",      "=va: report bridges that change it") \
    X(KERNEL_WATCH_ALL,  TRACE, "kernel_watch_all","RECOMP_KERNEL_WATCH_ALL",  "kernel_watch: every new value") \
    X(PEEK,              TRACE, "peek",            "RECOMP_PEEK",              "=va[:n],...: print guest memory at reports") \
    X(PEEK_CHAIN,        TRACE, "peek_chain",      "RECOMP_PEEK_CHAIN",        "=va,off,...: follow a pointer chain") \
    X(FIND_NAN,          TRACE, "find_nan",        "RECOMP_FIND_NAN",          "scan for NaN matrices at reports") \
    X(FIND_QUAD,         TRACE, "find_quad",       "RECOMP_FIND_QUAD",         "scan for quad vertices once") \
    X(D3D11_VERBOSE,     TRACE, "d3d11_verbose",   "RECOMP_D3D11_VERBOSE",     "D3D11 shader sources") \
    X(D3D11_PX,          TRACE, "px",              "RECOMP_D3D11_PX",          "=x,y[;x,y]: D3D11/CPU draws touching a pixel") \
    X(D3D11_PX_FLIPS,    TRACE, "px_flips",        "RECOMP_D3D11_PX_FLIPS",    "=a[-b]: px only in those flips") \
    X(D3D11_PX_MAX,      TRACE, "px_max",          "RECOMP_D3D11_PX_MAX",      "=n: px line limit (400)") \
    X(D3D11_PX_CONSTS,   TRACE, "px_consts",       "RECOMP_D3D11_PX_CONSTS",   "=a-b,c: px also prints these constants") \
    X(D3D11_PX_VERTS,    TRACE, "px_verts",        "RECOMP_D3D11_PX_VERTS",    "px also prints program and vertices") \
    X(STUB_LOG,          TRACE, "stub",            "RECOMP_STUB_LOG",          "name each unresolved stub reached") \
    /* ── debug: RECOMP_DEBUG=key[=value],... ─────────────────────────── */ \
    X(PB_VSH,            DEBUG, "pb_vsh",          "RECOMP_PB_VSH",            "=0: no vertex-program interpreter") \
    X(PB_FF,             DEBUG, "pb_ff",           "RECOMP_PB_FF",             "=0: fixed-function batches drawn untransformed") \
    X(PB_RC,             DEBUG, "pb_rc",           "RECOMP_PB_RC",             "=0: no register combiners (MODULATE)") \
    X(PB_FAST,           DEBUG, "pb_fast",         "RECOMP_PB_FAST",           "=0: CPU raster without fast paths") \
    X(PB_FAST_AB,        DEBUG, "pb_fast_ab",      "RECOMP_PB_FAST_AB",        "compare fast and slow raster per batch") \
    X(PB_BILINEAR,       DEBUG, "pb_bilinear",     "RECOMP_PB_BILINEAR",       "=0: CPU raster samples nearest, whatever the filter") \
    X(PB_MIPS,           DEBUG, "pb_mips",         "RECOMP_PB_MIPS",           "=0: CPU raster samples mip level 0 only") \
    X(PB_CLIP,           DEBUG, "pb_clip",         "RECOMP_PB_CLIP",           "=0: no near-plane clipping") \
    X(PB_CULL_FLIP,      DEBUG, "pb_cull_flip",    "RECOMP_PB_CULL_FLIP",      "CPU raster culls the other winding") \
    X(PB_VSH_AB,         DEBUG, "pb_vsh_ab",       "RECOMP_PB_VSH_AB",         "compare vertex-program paths per batch") \
    X(PB_VSH_AB_DUMP,    DEBUG, "pb_vsh_ab_dump",  "RECOMP_PB_VSH_AB_DUMP",    "=prefix: pb_vsh_ab differing batches") \
    X(RASTER_TEST,       DEBUG, "raster_test",     "RECOMP_RASTER_TEST",       "draw a known triangle on every clear") \
    X(ZPASS_FIXED,       DEBUG, "zpass_fixed",     "RECOMP_ZPASS_FIXED",       "=n: every occlusion report reads n") \
    X(PB_NULL_DRAW_US,   DEBUG, "null_draw_us",    "RECOMP_PB_NULL_DRAW_US",   "=us: null backend cost per draw") \
    X(PB_INJECT_STOP,    DEBUG, "pb_inject_stop",  "RECOMP_PB_INJECT_STOP",    "=n: stop every nth walk (test hook)") \
    X(PB_MLOG,           DEBUG, "pb_mlog",         NULL,                       "=flip: every method of that flip (subchannel, method, parameter) to stderr as [MLOG]") \
    X(PB_MERGE,          DEBUG, "pb_merge",        NULL,                       "=0: draw every guest batch on its own, no merging of back-to-back triangle batches on a GPU backend") \
    X(FAST_KICK,         DEBUG, "fast_kick",       "RECOMP_FAST_KICK",         "=0: ack loop ticks at Sleep(1)") \
    X(KICK_EVENT,        DEBUG, "kick_event",      NULL,                       "=1: the ack loop sleeps until the guest pokes it instead of polling the KickOff every ~50 us (default off)") \
    X(RUN_AHEAD,         DEBUG, "run_ahead",       NULL,                       "=N: the walker publishes GET as it walks and acks the KickOff flush with up to N segments (1 for any other value but 0) submitted past its walk; D3D's notify NOPs raise the PGRAPH interrupt (default off; docs/env.md)") \
    X(KICK_NAP,          DEBUG, "kick_nap",        NULL,                       "=hybrid|timer|delay|spin: how the ack loop naps between its ~50 us polls (docs/env.md)") \
    X(FORCE_RETURN,      DEBUG, "force_return",    "RECOMP_FORCE_RETURN",      "honour forced returns in the build") \
    X(TRAP_NULL,         DEBUG, "trap_null",       "RECOMP_TRAP_NULL",         "guest page zero faults") \
    X(DSP_ACK,           DEBUG, "dsp_ack",         "RECOMP_DSP_ACK",           "=va,...: zero these words (no APU)") \
    X(APU_DSP_ACK,       DEBUG, "apu_dsp_ack",     "RECOMP_APU_DSP_ACK",       "=auto (default) | va,... | 0: APU DSP acks the GP doorbell (from GPSADDR), these words, or nothing") \
    X(APU_DSP_STATS,     DEBUG, "apu_dsp_stats",   NULL,                       "one [APU DSP] line every 1500 APU frames: GP/EP frames, cycles, time, cap hits; warm-up skipped and late frames as totals (RECOMP_APU_DSP=1)") \
    X(APU_SOLO,          DEBUG, "apu_solo",        "RECOMP_APU_SOLO",          "=voice: mix only this APU voice") \
    X(APU_VOICE_DUMP,    DEBUG, "apu_voice_dump",  "RECOMP_APU_VOICE_DUMP",    "=voice|all: raw samples of one APU voice (all: every voice) to a file") \
    X(APU_VOICE_DUMP_FILE, DEBUG, "apu_voice_dump_file", "RECOMP_APU_VOICE_DUMP_FILE", "=path: apu_voice_dump output, default voice_dump.raw") \
    X(APU_MIXDOWN_ALL,   DEBUG, "apu_mixdown_all", "RECOMP_APU_MIXDOWN_ALL",   "=0: two bins; =2: all at unity") \
    X(POKE,              DEBUG, "poke",            "RECOMP_POKE",              "=va:value,...: write guest words") \
    X(WORKERS,           DEBUG, "workers",         "RECOMP_WORKERS",           "=inline: run worker threads inline") \
    X(GUEST_PRIORITY,    DEBUG, "guest_priority",  NULL,                       "=0: KeSetBasePriorityThread leaves host thread priorities alone and KeQueryBasePriorityThread reads 0; default 1: the title's priorities reach its host threads where they exclude (native Windows, not Wine, guest threads pinned to one core, guest CPU lock off); =2: everywhere (A/B)") \
    X(GUEST_QUANTUM,     DEBUG, "guest_quantum",   "RECOMP_GUEST_QUANTUM",     "=ms: a guest thread that has held the guest CPU (RECOMP_GUEST_LOCK) this long with another waiting hands it over at its next kernel call (default 4; 0: at every kernel call)") \
    X(GUEST_SCHED,       DEBUG, "guest_sched",     NULL,                       "=1: with the guest CPU lock on and guest_priority not 0, the guest CPU follows the title's thread priorities: a release goes to the highest-priority waiter, a holder that outranks every waiter keeps it, the priority query reads the stored value and another guest thread's suspend is counted guest-side (default 0: FIFO, priorities as guest_priority says)") \
    X(CS_MODE,           DEBUG, "cs_mode",         "RECOMP_CS_MODE",           "=single: one lock for every guest lock") \
    X(ASYNC_IO,          DEBUG, "async_io",        "RECOMP_ASYNC_IO",          "asynchronous file I/O") \
    X(UNIMPL_TRAP,       DEBUG, "unimpl_trap",     "RECOMP_UNIMPL_TRAP",       "stop at an unimplemented instruction") \
    X(D3D11_MEMO,        DEBUG, "d3d11_memo",      "RECOMP_D3D11_MEMO",        "=0: no D3D11 memo (state shadows, texture stages, stage-loop views)") \
    X(D3D11_NO_CULL,     DEBUG, "d3d11_no_cull",   "RECOMP_D3D11_NO_CULL",     "D3D11 culls nothing") \
    X(D3D11_CULL_FLIP,   DEBUG, "d3d11_cull_flip", "RECOMP_D3D11_CULL_FLIP",   "D3D11 culls the other winding") \
    X(D3D11_NO_MIPS,     DEBUG, "d3d11_no_mips",   "RECOMP_D3D11_NO_MIPS",     "D3D11 uploads level 0 only") \
    X(D3D11_POINT,       DEBUG, "d3d11_point",     "RECOMP_D3D11_POINT",       "D3D11 point sampling") \
    X(D3D11_NO_RTT,      DEBUG, "d3d11_no_rtt",    "RECOMP_D3D11_NO_RTT",      "D3D11 decodes render targets from memory") \
    X(D3D11_DEBUG_PS,    DEBUG, "d3d11_debug_ps",  "RECOMP_D3D11_DEBUG_PS",    "=name: replace pixel shaders") \
    X(D3D11_DEBUG_PS_SKIP_POST, DEBUG, "d3d11_debug_ps_skip_post", "RECOMP_D3D11_DEBUG_PS_SKIP_POST", "debug_ps spares post passes") \
    X(D3D11_OCC,         DEBUG, "d3d11_occ",       "RECOMP_D3D11_OCC",         "=sync|fixed: D3D11 occlusion mode") \
    X(METAL_NO_MIPS,     DEBUG, "metal_no_mips",   "RECOMP_METAL_NO_MIPS",     "Metal uploads level 0 only") \
    X(METAL_MEMO,        DEBUG, "metal_memo",      NULL,                       "=0: no Metal per-batch memo (c[] by generation, texture stages)") \
    X(METAL_POINT,       DEBUG, "metal_point",     "RECOMP_METAL_POINT",       "Metal point sampling") \
    X(METAL_NO_RTT,      DEBUG, "metal_no_rtt",    "RECOMP_METAL_NO_RTT",      "Metal decodes render targets from memory (implies metal_writeback=always)") \
    X(METAL_PRESENT,     DEBUG, "metal_present",   NULL,                       "=layer|readback: Metal window present, CAMetalLayer from GPU slots (default) or the old texture readback") \
    X(METAL_HOST_DUMP,   DEBUG, "metal_host_dump", NULL,                       "=prefix: Metal host-size frame dumps (render.scale, display.aspect) as <prefix>host_NNNNN.bmp, every fb_dump_flips flip") \
    X(METAL_WRITEBACK,   DEBUG, "metal_writeback", NULL,                       "=lazy|always: Metal writes guest memory when a dump reads it (default) or at every flip") \
    X(METAL_FB_GUARD,    DEBUG, "metal_fb_guard",  NULL,                       "trap title reads of the last Metal present surface; a read switches to metal_writeback=always") \
    X(METAL_OCC,         DEBUG, "metal_occ",       "RECOMP_METAL_OCC",         "=sync|fixed: Metal occlusion mode") \
    X(METAL_SHADER_NONCE, DEBUG, "metal_shader_nonce", NULL, "=text: Metal renames each runtime shader's entry point with this suffix and the PID, so macOS's shader caches miss (cold-cache timing)") \
    X(METAL_MSL_VERSION, DEBUG, "metal_msl_version", NULL, "=major.minor: Metal compiles its shaders at this MSL version instead of 3.0; 0 takes the OS default (much slower on macOS 27); an unknown version is ignored with a warning") \
    X(METAL_SHADER_DUMP, DEBUG, "metal_shader_dump", NULL, "=prefix: Metal writes each runtime shader's MSL to <prefix><kind>_<hash>.metal") \
    X(OCC_DELAY_US,      DEBUG, "occ_delay_us",    NULL,                       "=us: hold every visibility result back this long (test hook)") \
    X(OCC_SETTLE_US,     DEBUG, "occ_settle_us",   NULL,                       "=us: per-frame wait for pending visibility results before settling them at a walk end, semaphore release or flip (8000; 0 none)") \
    X(RT_ALIAS_CHECK,    DEBUG, "rt_alias_check",  NULL,                       "=0: Metal and D3D11 keep render targets whose guest memory the title rewrote (before rt-stale-alias)") \
    X(FB_VA,             DEBUG, "fb_va",           "RECOMP_FB_VA",             "=va: framebuffer window shows that address") \
    X(USB,               DEBUG, "ohci",            "RECOMP_USB",               "emulated OHCI/XID USB: the pad through the title's own XAPI driver (=0 off)") \
    X(USB_PORT,          DEBUG, "usb_port",        "RECOMP_USB_PORT",          "=n: USB pad port") \
    X(USB_HC,            DEBUG, "usb_hc",          "RECOMP_USB_HC",            "=1: pad on the second controller") \
    X(USB_NDP,           DEBUG, "usb_ndp",         "RECOMP_USB_NDP",           "=n: root-hub ports, 1..4") \
    X(PAD_PRESS,         DEBUG, "pad_press",       "RECOMP_PAD_PRESS",         "=mask: pulse these buttons (USB pad)") \
    X(FB_DUMP,           DEBUG, "fb_dump",         "RECOMP_FB_DUMP",           "=prefix: frame dumps (CPU/Metal, window)") \
    X(FB_DUMP_AT,        DEBUG, "fb_dump_at",      "RECOMP_FB_DUMP_AT",        "=flips: fb_dump at these flips (up to 256 ranges)") \
    X(FB_DUMP_FLIPS,     DEBUG, "fb_dump_flips",   "RECOMP_FB_DUMP_FLIPS",     "=n: fb_dump every nth flip") \
    X(FB_DUMP_PROG,      DEBUG, "fb_dump_prog",    NULL,                       "=hash[,hash]: fb_dump the flips around the first draw of these vertex programs (hex, up to 16)") \
    X(FB_DUMP_PROG_BEFORE, DEBUG, "fb_dump_prog_before", NULL,                 "=n: fb_dump_prog flips kept before the trigger, 0..240 (default 30; CPU/Metal)") \
    X(FB_DUMP_PROG_AFTER, DEBUG, "fb_dump_prog_after", NULL,                   "=n: fb_dump_prog flips dumped from the trigger on, capped at 240 (default 60)") \
    X(HUD_CAPTURE,       DEBUG, "hud_capture",     NULL,                       "=dir: the HUD mapping capture (hud_capture.h): capture.jsonl and one BMP per captured guest frame; records and changes nothing") \
    X(HUD_CAPTURE_AT,    DEBUG, "hud_capture_at",  NULL,                       "=from:count: hud_capture's guest frames (default 1:1, count up to 10000)") \
    X(HUD_CAPTURE_TABLE, DEBUG, "hud_capture_table", NULL,                     "=path: a HUD table (TOML) hud_capture loads and hooks itself, so a capture needs no mod; a table a mod registered wins") \
    X(FB_WINDOW_DUMP_EVERY, DEBUG, "fb_window_dump_every", "RECOMP_FB_WINDOW_DUMP_EVERY", "=n: window dump period") \
    X(WINDOW_SHOT,       DEBUG, "window_shot",     "RECOMP_WINDOW_SHOT",       "=flips: SDL window shots") \
    X(WINDOW_SHOT_PREFIX, DEBUG, "window_shot_prefix", "RECOMP_WINDOW_SHOT_PREFIX", "=prefix: window shot files") \
    X(OPTS_AT,           DEBUG, "opts_at",         NULL,                       "=flip:key=value,...: live option requests at those flips, as the in-game menu makes them (scale, filter, fullscreen, aniso, movie, overscan, snap_2d, pacing)") \
    X(MOD_SET_AT,        DEBUG, "mod_set_at",      NULL,                       "=frame:key=value,...: mod settings changed live from the flip tick once the guest reaches that xbox_mod_frame count, as the Mods page does (needs the menu armed)") \
    X(TEX_DUMP,          DEBUG, "tex_dump",        "RECOMP_TEX_DUMP",          "=prefix: textures, first use") \
    X(TEX_DUMP_EVERY,    DEBUG, "tex_dump_every",  "RECOMP_TEX_DUMP_EVERY",    "=n: tex_dump every nth bind too") \
    X(VSH_DUMP,          DEBUG, "vsh_dump",        "RECOMP_VSH_DUMP",          "=prefix: vertex programs, raw, once each (every backend)") \
    X(VSH_CONST_DUMP,    DEBUG, "vsh_const_dump",  NULL,                       "=path: every program batch's constants c[0..191] with its flip, draw and program hash, one file (src/kernel/nv2a_vsh_cdump.h); stops at 256 MB") \
    X(VSH_CONST_FLIPS,   DEBUG, "vsh_const_flips", NULL,                       "=a-b[/n]: vsh_const_dump only at flips a..b, every nth (default every flip)") \
    X(VSH_CONST_PROG,    DEBUG, "vsh_const_prog",  NULL,                       "=hash[,hash]: vsh_const_dump only these programs (hex, up to 16)") \
    X(D3D11_DUMP,        DEBUG, "d3d11_dump",      "RECOMP_D3D11_DUMP",        "=prefix: D3D11 present dump at flips 60k+1") \
    X(FMV_DUMP,          DEBUG, "fmv_dump",        "RECOMP_FMV_DUMP",          "=prefix: two movie frames") \
    X(AUDIO_WAV,         DEBUG, "audio_wav",       "RECOMP_AUDIO_WAV",         "=path: WAV of the audio output") \
    X(AUDIO_WAV_SECS,    DEBUG, "audio_wav_secs",  "RECOMP_AUDIO_WAV_SECS",    "=s: audio_wav length cap") \
    X(AUDIO_THREAD_TC,   DEBUG, "audio_thread_tc", NULL,                       "lets RECOMP_AUDIO_THREAD_PRIORITY=time_critical through (measurement only)") \
    X(WATCH,             DEBUG, "watch",           "RECOMP_WATCH",             "=spec: write watchpoint on a guest address") \
    X(WATCH_RAW,         DEBUG, "watch_raw",       "RECOMP_WATCH_RAW",         "watch also prints the raw frame") \
    X(WATCH_DEPTH,       DEBUG, "watch_depth",     "RECOMP_WATCH_DEPTH",       "=n: watch stack depth (14)") \
    X(WATCH_LEN,         DEBUG, "watch_len",       "RECOMP_WATCH_LEN",         "=bytes: watch range") \
    X(WATCHDOG_SECS,     DEBUG, "watchdog",        "RECOMP_WATCHDOG_SECS",     "=s: dump and exit after s seconds") \
    /* from upstream (merge of origin/main 1409a7d), appended */ \
    X(USB_PADS,          CONFIG, "RECOMP_USB_PADS",          NULL, "emulated USB pads plugged in, 1..4 (default 1)") \
    X(PAD_SCRIPT,        DEBUG, "pad_script",      "RECOMP_PAD_SCRIPT",        "=ms:btn[+btn][:hold],... f<flip>:btn[:flips] or g<poll>:btn[:polls], <clock>:@open:<name>[:btn/period] anchors, <clock>:@end, or =@file: timed pad presses") \
    X(PAD_LIVE,          DEBUG, "pad_live",        "RECOMP_PAD_LIVE",          "=file: press each line appended to it (btn[+btn][:hold], or f<flip>:/g<poll>: on the script's clock)") \
    X(PAD_RECORD,        DEBUG, "pad_record",      "RECOMP_PAD_RECORD",        "=file: write the pads' presses as a pad_script timed by flip or poll (pad_clock), ending in @end") \
    X(PAD_ANCHOR,        DEBUG, "pad_anchor",      NULL,                       "=text[|text...]: pad_record writes an anchor (f<flip>: or g<poll>:@open:<name>) for each open whose path holds one") \
    X(PAD_CLOCK,         DEBUG, "pad_clock",       NULL,                       "=poll: pad_record times its steps by the title's pad polls (g<poll>), with exact stick and analog values; =flip (default) by the walker's flip") \
    X(PAD_QUIT,          DEBUG, "pad_quit",        NULL,                       "once the pad script's last step (its @end, if it has one) has ended, close the window as its close button does (no window: exit hooks, then ExitProcess(0), on a thread of its own)") \
    X(KBM_SCRIPT,        DEBUG, "kbm_script",      NULL,                       "=file: inject key and mouse events on the pad poll (src/input/kbm_script.c); refused with RECOMP_INPUT_SCRIPT") \
    X(PB_REPORT_MS,      TRACE, "pb_report_ms",    "RECOMP_PB_REPORT_MS",      "=ms: [PB] report / fb_dump interval (10000, min 100)") \
    X(DPC_PROF,          TRACE, "dpc",             NULL,                       "[DPCPROF] time per host-run DPC/ISR routine and its waits, under the 600-vblank line") \
    X(PACING_TRACE,      TRACE, "pacing",          NULL,                       "[PACING] flip intervals, vblank gaps, the flip hold (holds, held ms, timeouts, intervals), CPU and spin-wait sites every 600 flips; =all also every flip") \
    X(PROF,              TRACE, "prof",            NULL,                       "=hz (1: 250): sample every thread's pc in-process (Windows, Wine, macOS); report at exit, module offsets for the bench to symbolise (host_prof.h)") \
    X(PROF_DELAY,        TRACE, "prof_delay",      NULL,                       "=s: prof starts sampling after this many seconds (default 0)") \
    X(PROF_OUT,          TRACE, "prof_out",        NULL,                       "=path: prof's report (default recomp-prof.txt in the working directory)") \
    X(MEM_TRACE,         TRACE, "mem",             NULL,                       "=s (bare: 1): a [MEM] sample line (process RAM and GPU memory) every s seconds; the [MEM] exit line prints always (mem_stats.h)") \
    X(GUEST_CPU_TRACE,   TRACE, "guest_cpu",       "RECOMP_GUEST_CPU_TRACE",   "=ms: [GUEST] each hold of the guest CPU (RECOMP_GUEST_LOCK) longer than ms and where it ended (first 50, then one a second)") \
    X(VBLANK_CLOCK,      DEBUG, "vblank_clock",    NULL,                       "=ms: the old millisecond vblank schedule and timer loop (A/B)") \
    X(MENU_INPUT_TRACE,  TRACE, "menu_input",      NULL,                       "the enhancements menu's input: the device each event comes from, focus moves, pointer hits and clicks, tooltip shows and the text field's codepoints (overlay-revamp D3, D4)") \
    X(DETERMINISTIC,     DEBUG, "deterministic",   NULL,                       "virtual guest clock: every clock the title reads (performance counter, system, interrupt and tick time, rdtsc) and every timed wait runs on emulated vblanks, one 60 Hz period each, from a fixed 2004-01-01 epoch (src/kernel/guest_time.h)") \
    X(DET_TRACE,         DEBUG, "det_trace",       NULL,                       "=path: with deterministic and the guest lock, the scheduler's hand-offs, vblank steps and GPU catch-ups, one line each, and a hash of every 64 KB page of guest RAM at each step to path.pages (src/kernel/kernel_guest_det.c)") \
    X(DET_DUMP,          DEBUG, "det_dump",        NULL,                       "=S<n>|C<n>[,...]: with det_trace, all of guest RAM at that step or catch-up to path.<event>.ram (up to 16)") \
    X(DET_SNAP,          DEBUG, "det_snap",        NULL,                       "=<from>-<to>:<va>[:...]: with det_trace, the 64 KB pages at those addresses (guest RAM or the NV2A registers) after every hand-off, step and catch-up while the vblank count is in [from,to], to path.snap (up to 8 pages); in the window the P lines carry the RAM hash and each kernel call adds a K line") \
    X(DET_PROFILE,       DEBUG, "det_profile",     NULL,                       "=path: with deterministic and the guest lock, one S line per vblank step (vblank, flips so far, catch-ups and hand-offs since the last step, wall us split into ring, NV2A, timer, OHCI, APU and state hash, and each live thread's give-up: kind, site and guest call sites) and one W line per wait or yield that spanned a step (kind, how it ended, steps spanned, call sites); for finding what a frame's vblanks are spent on") \
    X(STATE_HASH,        DEBUG, "state_hash",      NULL,                       "=path: a hash of guest RAM at the end of every port-0 pad poll, one line each (poll, vblank, virtual time, hash), and the changed 64 KB pages to path.pages (src/kernel/guest_state_hash.h)") \
    X(STATE_HASH_SKIP,   DEBUG, "state_hash_skip", NULL,                       "=lo-hi[,lo-hi...]: guest ranges state_hash leaves out (hex, end exclusive; up to 16)") \
    X(STATE_DUMP,        DEBUG, "state_dump",      NULL,                       "=poll[,poll...]: with state_hash, all of guest RAM at the end of that poll to path.<poll>.ram (up to 16)") \
    X(MISSING,           TRACE, "missing",         "RECOMP_MISSING_FILES",     "missing game files, one line each and an exit summary; default on, =0 off, =all also absent directories, other trees, probes, every FAILED repeat and NtOpenFile/IoCreateFile successes") \
    X(MISSING_LIST,      DEBUG, "missing_list",    "RECOMP_MISSING_FILES_LIST","=path: the unique missing files with their attempt counts, written once by the first summary") \
    X(IRQ_SAFE_POINTS,   DEBUG, "irq_safe_points", "RECOMP_IRQ_SAFE_POINTS",   "=0: with RECOMP_DISPATCH_GATE, device interrupts run beside the gate holder after a hold-off (default with the gate: posted, run by the holder at its next kernel call, spin-wait yield or gate release; without the gate: off)") \
    X(IRQ_SAFE_MS,       DEBUG, "irq_safe_ms",     "RECOMP_IRQ_SAFE_MS",       "=ms: how long a posted interrupt waits for the holder's safe point before the [IRQ] wait log (default 250; again every second)") \
    X(IRQ_SAFE_FORCE,    DEBUG, "irq_safe_force",  "RECOMP_IRQ_SAFE_FORCE",    "=1: past irq_safe_ms, deliver beside the holder (A/B; reinstates the race)") \
    X(DPC_ON_RAISE,      DEBUG, "dpc_on_raise",    "RECOMP_DPC_ON_RAISE",      "=0: with RECOMP_DISPATCH_GATE, DPCs queued by an interrupt wait for the timer thread's drain (default with the gate: a guest thread runs them on its next raise to DISPATCH or at its gate release, and KeInsertQueueDpc wakes the timer thread; without the gate: off)") \
    X(PRESENT_STALE,     DEBUG, "present_stale",   NULL,                       "=n: a colour surface not drawn or cleared for n flips no longer sets the present size (default 120)") \
    X(TEX_CACHE,         DEBUG, "tex_cache",       NULL,                       "=n: GPU texture-cache entries in use, 1..512 (default 512 on D3D11, 256 on Metal)") \
    X(ZBUF_SLOTS,        DEBUG, "zbuf_slots",      NULL,                       "=n: CPU raster depth/stencil buffers kept, by zeta offset and clip extent, 1..16 (default 8)") \
    /* ── the enhancements layer's keys (RECOMP_ENV_ENHANCE_KEYS, below) ── */ \
    RECOMP_ENV_ENHANCE_KEYS(X) \
    /* ── the game's own keys (RECOMP_ENV_GAME_KEYS, below) ──────────────── */ \
    RECOMP_ENV_GAME_KEYS(X)

typedef enum {
#define RECOMP_ENV_ENUM_(id, tier, name, legacy, help) RENV_##id,
    RECOMP_ENV_KEYS(RECOMP_ENV_ENUM_)
#undef RECOMP_ENV_ENUM_
    RENV_COUNT
} recomp_env_id;

/* Read the environment into the cache. Idempotent; the first lookup calls
 * it. Call it first thing in main so deprecation lines come out early. */
void recomp_env_init(void);
/* Print the reader's notes (deprecated names, unknown keys, help), held
 * since init. A host calls it once stderr is where it should be (after the
 * RECOMP_STDIO_LOG redirect); otherwise they are printed at exit. */
void recomp_env_flush_notes(void);

/* Tests that setenv() a variable after the first lookup re-read with this. */
void recomp_env_reload(void);

/* Override a value from code (NULL clears it). Takes a copy. */
void recomp_env_set(recomp_env_id id, const char *value);

extern const char *recomp_env_value_[RENV_COUNT];
extern volatile int recomp_env_ready_;

/* The value, or NULL when unset: what getenv(old name) returned. */
static inline const char *recomp_env(recomp_env_id id)
{
#if defined(_MSC_VER)
    if (!recomp_env_ready_)   /* volatile: acquire under MSVC's default /volatile:ms */
#else
    if (!__atomic_load_n(&recomp_env_ready_, __ATOMIC_ACQUIRE))
#endif
        recomp_env_init();
    return recomp_env_value_[id];
}

/* Set, non-empty and not starting with '0'. */
static inline int recomp_env_on(recomp_env_id id)
{
    const char *v = recomp_env(id);
    return v && *v && *v != '0';
}

/* atoi / strtoul(,0) / atof of the value, or `def` when unset. */
long recomp_env_int(recomp_env_id id, long def);
unsigned long recomp_env_ulong(recomp_env_id id, unsigned long def);
double recomp_env_double(recomp_env_id id, double def);

/* The variable or list key, for messages. */
const char *recomp_env_name(recomp_env_id id);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_ENV_H */
