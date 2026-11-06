# Reuniting Burnout 3 with xboxrecomp

> Part of the [Documentation Index](../INDEX.md)

Burnout 3 was the first static recompilation of an original Xbox title with no
prior recompiler to lean on. The core of xboxrecomp was written while getting it
to boot, then extracted into a reusable toolkit and hardened against Halo,
Crimson Skies and Steel Battalion. The two have since diverged. This is the plan
to bring Burnout 3 back onto the shared toolkit — and, more importantly, an
honest account of why it is a *reconciliation*, not a *swap*.

## The one-line finding

**The toolkit halves reunited cleanly; the runtime halves did not.** After the
extraction, xboxrecomp's `tools/` and Burnout 3's `tools/` diverged in one
direction (xboxrecomp got every fix, the fork kept one feature — now upstreamed),
but the *runtime libraries* diverged in ways that make the kernel a shared-*code*,
per-title-*data* problem rather than a swap. Two independent walls: the two
kernels drive the game loop in incompatible ways (**Phase 1, now reconciled**),
and the kernel export-ordinal table is XDK-version-specific — Burnout 3's XBE
(XDK 5849) expects a different ordinal mapping than Halo (3911) and Crimson
(5659), so the routing cannot be shared as-is (**Phase 2, the next gate**).

## What is already done

**Toolkit (`tools/`): reunited.** Burnout 3's fork had exactly one thing
xboxrecomp lacked — `--exclude-manual`, which reads `recomp_manual.c` for the
functions it defines by hand and skips generating their bodies. That is now
upstreamed (`manual_scan.py` + the `--exclude-manual` flag). Proven end to end:
xboxrecomp's full pipeline on Burnout 3's XBE — correct export table, call-target
realignment, transcendentals, real return addresses, flat dispatch — generates
22,893 / 23,010 functions, 0 failed, excluding 117 hand-written overrides and
wrapping 2. **Burnout 3's recompiled code can be produced entirely by
xboxrecomp's tools today.**

The x87 transcendentals were also ported directly into Burnout 3's lifter fork
as an interim fix (its own commit on the `linux-port` branch), which measurably
eliminated its FP exceptions (10 → 0). That is redundant once the toolkit swap
lands, but it is a real correctness fix in the meantime.

## Why the runtime is not a swap

Measured, file by file. Every library diverged substantially — no library is a
clean stale copy:

| library | BO3 vs xboxrecomp | nature of divergence |
|---|---|---|
| `src/kernel` | 17/17 files differ | **architectural** (threading model) |
| `src/d3d` | 9/12 files differ | co-tuned with BO3's nv2a pipeline |
| `src/nv2a` | game-coupled | 2 BO3-only bridge headers |
| `src/audio` | 768 diff-lines | independent evolution |
| `src/input` | 488 diff-lines | independent evolution |

### The hard gate: two opposite threading models

This is the blocker, and it is not staleness — it is two valid designs that each
co-evolved with their game layer.

- **Burnout 3**: `xbox_PsCreateSystemThreadEx` creates a **real Win32 thread**
  (`CreateThread`). Its `main.c` frame-pump drives the game tick on a claimed
  worker-stack slice (`xbox_worker_stack_alloc`, 16 × 256 KB), and its watchdog
  samples the game thread through `xbox_thread_debug_handle`. Recent commits —
  "Make game_frame_pump the only frame driver", "Give the host loop a stack so it
  can drive the game tick" — built this. The game loop *requires* real concurrency.

- **xboxrecomp**: `bridge_PsCreateSystemThreadEx` runs the start routine
  **synchronously** through `RECOMP_ICALL` ("Must run synchronously…"). This is
  what Halo and Crimson need; their loops are not built on a worker frame-pump.

Swapping Burnout 3 onto xboxrecomp's kernel would replace real threads with
synchronous execution and its frame pump would never be driven. Swapping the
other way would break Halo. **Neither kernel is a superset of the other.**

There is a second, sharper reason the kernel cannot be swapped, and it is the
opposite of what an earlier draft of this document claimed. **The kernel export
ordinals are XDK-version-specific, and Burnout 3's table is correct *for Burnout
3*.** Its `default.xbe` was built with XDK 5849 (2004); Halo 2276 with 3911
(2001), Crimson Skies with 5659 (2003). 50 of Burnout 3's 147 imports map to a
different ordinal than xboxrecomp's table — e.g. ordinal 49 is
`HalRequestSoftwareInterrupt` for Burnout 3 but `HalReturnToFirmware` for the
older XDKs, a +1 shift through that region. This was measured, not assumed: an
actual Phase 2 kernel swap built and ran Burnout 3 on xboxrecomp's kernel — the
dispatch contract matched, threading worked (SPAWN mode, first call spawned a
real thread and the entry returned), zero "no bridge" warnings — and then the
game called ordinal 49 during *engine setup*, where it needs a software
interrupt, xboxrecomp routed it to `HalReturnToFirmware`, and the process exited.
Burnout 3's own kernel routes that ordinal to the benign software interrupt and
the game boots; the routing it was tuned to is the routing its XBE expects.

So the kernel is not "Burnout 3 has bugs xboxrecomp fixed" — it is two correct
tables for two different XDKs, plus xboxrecomp being ahead on the *code* (the
correctness fixes, the audit tooling, the Phase 1 threading). A real kernel swap
therefore needs xboxrecomp's routing to be **per-title data**, derived from the
title's own XBE import analysis, rather than one hardcoded ordinal switch. That
is the actual Phase 2 work, and it is larger than a library swap.

## The plan

Sequenced so each step is independently testable and nothing regresses a working
title.

**Phase 0 — toolkit. Mostly done.** `--exclude-manual` upstreamed; xboxrecomp's
tools generate Burnout 3's code. To *retire* the `tools/` fork — point Burnout 3's
regen at xboxrecomp's `tools/` and delete its copy — two small loose ends remain,
both low-risk:

- Burnout 3's `gen_dangling_stubs.py` is **redundant** with xboxrecomp's built-in
  `recomp_stubs_unresolved.c` (the translator emits stubs for unresolved call
  targets inline; the realignment fix shrinks that set further). It is dropped,
  not ported.
- Burnout 3's `recomp_types.h` is a stale copy that predates flat dispatch, so it
  lacks the `recomp_dispatch_init` declaration xboxrecomp's generated code
  references. Replace it with xboxrecomp's template (re-applying any Burnout 3
  game-local additions). `merge_names` and `xdk_names.json` already work with
  xboxrecomp's `func_id`; those stay as Burnout 3 config.

Completing Phase 0 is a regen + full rebuild (the giant chunks are slow) and does
not touch the runtime, so it is safe — just not instant.

**Phase 1 — threading reconciliation. Done.** xboxrecomp's kernel now supports
both driving models, defaulting to the one Halo and Crimson Skies use so they are
untouched.

- `XBOX_WORKER_STACK_*` region + `xbox_worker_stack_alloc`/`free` (verbatim from
  the Burnout 3 fork): a guest stack for the host's own thread to call recompiled
  code from, which is what a host-tick-driven title needs to pump its frame tick.
- `xbox_thread_debug_handle` + `g_game_thread`, recorded when the bridge spawns a
  thread, so a host watchdog can sample the game thread.
- `xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN)`: in SPAWN mode every
  `PsCreateSystemThreadEx` is a real thread so the title's entry can return and
  the host can drive. Default is `INLINE` — the first call runs the game in place,
  the historical behavior.

All of it is additive: a default-model title calls none of it, so the region is
unused address space and the code is never reached. Verified: Halo builds and
runs **byte-identically** (same exit, same milestones, same
`render_cameras.c:458`), the allocator passes a standalone correctness check (16
distinct slices, exhaustion, free/reuse, aligned stride), and Burnout 3's exact
`main.c` threading API compiles against xboxrecomp's headers. What remains is
Phase 2 wiring it up under a real swap — including confirming empirically whether
Burnout 3's init thread must genuinely run concurrently (SPAWN) or whether INLINE
suffices.

**Phase 2 — kernel swap: routing done, threading-execution next.** Two parts.

*Routing (done).* Rather than refactor the kernel's three hardcoded ordinal
tables, a single translation layer maps a title's ordinals into the kernel's
canonical space before any routing decision. `xbox_kernel_set_ordinal_remap`
takes `map[title_ordinal] = canonical_ordinal`; the init loop applies it right
where it reads the ordinal, so the data-export, bridge, and arg-size lookups all
see the canonical number. `tools/kernel_audit/gen_ordinal_remap.py` builds the
map by matching each function the title imports to the ordinal the kernel's own
table gives that name. Measured: Halo (XDK 3911) and Crimson (5659) remap
**nothing** — their XDK is the kernel's canonical one, so they are byte-identical
(verified: Halo still exits 3 at `render_cameras.c:458`). Burnout 3 (XDK 5849)
remaps 47 ordinals and leaves 3 its parser could not name as identity. With the
remap installed, **Burnout 3 no longer exits during engine setup** — ordinal 49
now resolves to `HalRequestSoftwareInterrupt` (which the kernel benignly stubs),
`xbe_entry_point` returns normally, and the title enters its host loop and starts
loading crash-mode HUD textures on xboxrecomp's kernel. Zero "no bridge" warnings
of consequence, zero FP exceptions.

*Threading execution (next).* One layer deeper: the init worker
`PsCreateSystemThreadEx` spawns runs but does not advance the game state (zero
kernel calls from the worker stack region). xboxrecomp's `bridge_thread_main` and
Burnout 3's `xbox_recomp_thread_wrapper` set the new thread up almost identically
(esp = stack top, push ctx2/ctx1/dummy-return, call the routine), so this is a
narrow integration difference in how the spawned routine runs under the swapped
kernel, not a routing problem. That is the next thing to chase — the routing gate
is now open.

The kernel *code* is shared, the ordinal *table* is per-title data, and Halo and
Crimson are untouched. Regenerate `gen/` with xboxrecomp's tools when this lands
(gen reaches the kernel only through `RECOMP_ICALL` by ordinal, so it does not
hard-depend on names — but the two should move together).

**Phase 3 — nv2a / d3d.** The hardest, most coupled. Burnout 3's game speaks to
its nv2a through two local bridge headers (`d3d_device_snapshot.h`,
`render_input_snapshot.h`) that xboxrecomp's nv2a does not expose. This is
Burnout 3's proven end-to-end pushbuffer pipeline and the last thing to risk.
Options, in order of preference: (a) upstream Burnout 3's snapshot interface into
xboxrecomp's nv2a; (b) keep Burnout 3's nv2a local indefinitely and swap only the
rest. Decide after Phase 2.

**Phase 4 — audio / input.** Peripheral, not on the frame-pump or render path.
Diff, reconcile, swap. Lowest risk, do last or opportunistically.

## The honest summary

"Get Burnout 3 running on the new xboxrecomp" is the right goal, and the toolkit
half is done. The runtime half turned out to have two gates, and taking each one
in turn is what made the shape of the problem clear rather than guessed:

- **Threading (Phase 1, done).** Two driving models, each correct for its title.
  Reconciled behind a per-title mode with Halo verified byte-identical.
- **Routing (Phase 2, the next gate).** The kernel export ordinals are
  XDK-specific. An actual swap built, linked, and ran Burnout 3 on xboxrecomp's
  kernel — proof the code and threading are compatible — and then exited during
  engine setup because ordinal 49 means different things in XDK 5849 and XDK 3911.
  The fix is to drive the kernel's ordinal routing from per-title data, so one
  kernel serves every XDK.

The correction worth stating plainly: an earlier pass here called Burnout 3's
kernel "shifted" and "buggy", with "50 misroutings xboxrecomp fixes". That was
backwards. Burnout 3's routing is correct *for Burnout 3's XBE*; the 50
differences are two XDKs, not one bug. xboxrecomp is ahead on kernel *code*, not
on the routing table — and the reunification has to keep both titles' tables, not
pick a winner. None of this throws away the worker-stack frame-pump Burnout 3
earned the hard way; it keeps it, and now keeps its ordinal table too.
