# Contributors

xboxrecomp exists because people who care about the Xbox keep showing up.
Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what — the
README changelog tells the story release-by-release, but credit lives here.

Reporting a bug counts. Several of the entries below are people who never sent
a patch and still moved the project further than a patch would have, because
they found the wall everyone else was going to hit.

If you've contributed and aren't listed, or a line here is wrong, open a PR
against this file — we want every name right.

---

## Maintainer

### Ned Heller — [@sp00nznet](https://github.com/sp00nznet)
Project creator and maintainer. The x86 disassembler and lifter, XBE parsing,
the kernel/XAPI runtime and HLE layer, the D3D8 and NV2A translation, the XISO
and XMV tooling, and the recompilation pipeline that ties them together.

---

## Contributors

### NoRain211 — [@NoRain211](https://github.com/NoRain211)
A correctness batch across the disassembler, lifter and translator, found by
stress-testing the pipeline against a real title. Almost all of it is the
dangerous kind of bug: the generated C compiles, links, runs, and is quietly
wrong, with no lifter error or warning anywhere.

*Control-flow recovery (#7)*
- **Conditional tail calls skipped the frame bridge** — `jcc` to a known
  function entry is a tail call, but only the unconditional form emitted the
  bridge, so the taken edge jumped into the callee with the caller's frame
  still live. 8,263 call sites across 5,426 functions on the title tested.
- **Fall-through off the end of a function emitted nothing** — a block running
  off its own end into the next function produced no transfer at all; control
  fell out of the generated switch and returned. 3,183 sites.
- **Indirect calls read the target after the return-address push** — `call
  [esp+X]` computed its operand once `esp` had already moved, so the target
  came from the wrong slot. The operand is now snapshotted before the push.
- **Computed jump targets were not owned by the enclosing function**, and
  `--seed-functions` can now be repeated to merge several seed sets.

*Flag computation (#8)*
- **`repe cmpsb` / `repne scasb` folded their result flags to a literal 1**, so
  every `memcmp`/`strcmp`-shaped loop in the CRT reported "equal" regardless of
  input. ZF/CF now derive from the last pair actually compared.
- **`NEG` carry was lost before a dependent `SBB`/`ADC`** — CF was preserved
  only when the consumer was the very next instruction, but real codegen
  separates them with flag-safe instructions, and the borrow silently went to
  zero. That is the standard 64-bit subtract and sign-extend idiom, so the
  corruption lands squarely in integer math.
- **Signed compares were evaluated at 32 bits regardless of operand width**, so
  `cmp al, bl` + `jl` never had the sign bit in the right place.

*x87 and SSE (#9, #10)*
- **Packed SSE was lifted as a scalar `float`** — XMM was modelled as a single
  float, so `movaps`/`movups` transferred 4 of 16 bytes and silently discarded
  the upper three lanes (18,439 moves), and packed arithmetic had no pattern at
  all: 561 operations dropped outright.
- **904 x87 instructions across 28 mnemonics lifted to comments**, desynchro-
  nising the FPU stack from that point on. The reverse forms are the nastiest —
  `fdivr` against 1.0 is a reciprocal, and dropping it turns a vector normalise
  `v/len` into `v*len`.
- **`FNSTCW` / `FNSTSW` were comments only**, so `_control87` could not read
  back rounding or precision mode and every `fcom`-derived parity test read a
  hardcoded `true` (1,326 sites). Both words are now modelled, and x87 state is
  shared rather than reset per function.
- **XMM was declared as a C local in each generated function**, so a value
  written in one lifted block and read in the next was lost to a fresh zeroed
  local. It is guest state now.

*(The runtime half of the SSE work — the `RecompXmm` union and the 28 `XMM_*`
helpers the lift emits — was not in the PRs and was added on integration.)*

*Missing instructions and dispatch (#14, #15, #16)*
- **`XLAT`/`XLATB` lifted to nothing**, so a guest table lookup left `AL`
  unchanged. Now `AL = MEM8(EBX + zero_extend(AL))`, written through `SET_LO8`
  so only the low byte moves, with the `0x67` address-size override using `BX`
  and wrapping the offset at 16 bits.
- **`BSF`/`BSR` were classified as flag setters but never lifted**, leaving an
  unhandled-instruction comment where the result should be. The flag half is
  the subtler fix: ZF was derived by re-reading the source operand at the
  branch, so any flag-preserving write between the scan and the `jcc` silently
  changed the answer. ZF is snapshotted where the scan runs, and the translator
  omits the snapshot when nothing consumes it. A zero source leaves the
  destination untouched, matching the architectural "undefined" instead of
  inventing a value.
- **`recomp_lookup_manual` was consulted on indirect calls only** — a direct
  call or tail jump went straight to the generated body, so a hand-written
  replacement worked through a function pointer and was quietly bypassed by
  every direct caller. Now direct calls and both tail-jump forms route through
  the manual set that `--manual-functions` and `--exclude-manual` already build.

*Flags, vertex layout and audio (#22, #23, #24)*
- **`xor reg, reg` zeroed the register without clearing the carry flag** — the
  lifter emitted the zeroing and nothing else, so `_cf` still held whatever the
  previous instruction left and a following `adc`/`sbb` borrowed a carry the
  hardware had already cleared. Shipped with five conformance cases covering
  byte, high-byte, word and dword zeroing into both `adc` and `sbb`.
- **The FVF position field was tested as bits (#23)** — `fvf & D3DFVF_XYZRHW`
  is a bit test against an *encoded* field, so `D3DFVF_XYZB1` (0x006) tested as
  transformed and took the untransformed path's opposite branch. The attribute
  offset had the matching bug: it assumed 3 or 4 floats and stepped over blend
  weights and normals as if they were not there. Both now come from the
  position field's own value.
- **DirectSound cursors and the mixer disagreed (#24)** — buffers kept a
  `play_cursor` and `status` of their own while the mixer advanced and stopped
  independently, so `SetCurrentPosition` did not seek, `Play` discarded the
  position it was given, and the getters were stale after rendering. The
  fixed-point source position was also too narrow and overflowed past 65,535
  frames. The regression is the notable part: it compiles the real mixer out of
  `apu_core.c` against the real `dsound_device.c` rather than a copy of either,
  so it cannot drift from what actually runs.

*Depth/stencil, A8 and audio (#29, #30, #31)*
- **The depth/stencil state cache keyed on a partial XOR hash (#29)** — it left
  out `STENCILWRITEMASK`, `STENCILFAIL`, `STENCILZFAIL` and `STENCILPASS`, so
  changing one of those alone reused a stale D3D11 state object, and being an
  XOR it could also cancel: moving the stencil reference 0 to 16 while the read
  mask went 0xFF to 0xFE produced the same key. It compares the whole
  zero-initialised descriptor now, keeps the stencil reference separate because
  that is passed to `OMSetDepthStencilState` rather than stored in the
  descriptor, and only updates the cached copy after the state object is
  actually created.
- **Xbox A8 sampled with zero RGB instead of white (#30)** — an alpha-only
  texture is `(1, 1, 1, alpha)`, so every fixed-function and register-combiner
  path drew A8 content black. Carried as one alpha-only flag per texture stage,
  restoring white RGB after the sample without touching alpha. The same PR found
  that a successful programmable vertex-shader setup skipped the fixed-function
  pixel-state refresh, so a texture change could leave the previous draw's pixel
  shader, constants and stale A8 metadata bound; all four draw entry points go
  through one prepare step now. Shipped with a 336-draw WARP regression that
  reads pixels back, across 2D/cube/volume, mip 1, all four stages and both
  pixel paths.
- **XAudio2 failures were reported as success (#31)** — `Start` and
  `SubmitSourceBuffer` results went unchecked, so a backend that could not start
  reported itself active, and a rejected buffer still advanced the ring and the
  accepted-frame count, which eventually reuses storage that is still queued for
  playback. Also balances `CoInitializeEx` when initialisation then fails on
  that thread, without uninitialising on `RPC_E_CHANGED_MODE`, and makes repeat
  initialisation idempotent and repeat shutdown safe after a partial one.

*MMX, blending and pretransformed vertices (#33, #34, #35, #36, #37)*
- **Fifteen MMX forms lost the comparison before them (#34)** — they were
  implemented, but missing from `_EFLAGS_PRESERVE`, so the lifter dropped the
  live comparison and fell back to recomputing `_flags`. `cmp eax, 0; pavgb
  mm0, mm1; sete al` returns 1 on the CPU and returned 0 lifted. The set was
  simply incomplete: signed and unsigned byte saturation, signed word
  saturation, the averages, min/max, sum of absolute differences,
  `CVTPS2PI`/`CVTTPS2PI`, `PINSRW` and `PEXTRW`. No implementation changed.
- **`PADDUSW` and `PSUBUSW` were never lifted (#33)** — they became TODO
  comments while the `MOVQ` loads and stores around them still executed, so the
  store published the unchanged value rather than the saturated one. That is
  the quiet failure mode: no lifter error, no warning, just the wrong number.
- **Float-to-MMX conversion ignored the rounding mode (#35)** — `MMX_CVT_F2I`
  added or subtracted 0.5 and cast, which rounds halfway away from zero
  regardless of MXCSR; 2.5 became 3 under round-to-nearest, and downward,
  upward and toward-zero were all wrong. Its range guard compared against a
  *float* literal for `INT32_MAX`, which rounds up to 2147483648 and admits an
  out-of-range cast. Uses the SSE scalar conversions on x86, which do exactly
  the right thing and leave the host x87/MMX register file alone, with a
  double-precision `nearbyint`/`trunc` fallback elsewhere.
- **Colour blend factors were copied into the alpha fields (#36)** —
  `update_blend_state` put `SrcBlend`/`DestBlend` straight into
  `SrcBlendAlpha`/`DestBlendAlpha`, and D3D11 rejects colour factors there. So
  a guest `SRCCOLOR`, `INVSRCCOLOR`, `DESTCOLOR` or `INVDESTCOLOR` made
  `CreateBlendState` fail with `E_INVALIDARG` and left the *previous* blend
  state bound — a wrong blend rather than a missing one, which is much harder
  to see. The regression reads back the bound descriptor for exactly that
  reason: a stale non-null state cannot produce a false pass.
- **`D3DFVF_XYZRHW` threw RHW away (#37)** — the fixed-function vertex shader
  accepted pretransformed input and emitted clip W = 1, so screen-space
  geometry still landed in the right place while its texture coordinates were
  interpolated affinely. Not a misplaced quad; a subtly wrong texture on a
  correctly placed one. Dividing the reconstructed clip position by RHW
  restores clip W = 1/RHW and leaves post-divide screen XYZ untouched.

*Also raised: stored code pointers (#13).* The gap is real and was found
independently while bringing up Half-Life 2 -- functions reachable only as an
address in a table have no call site, no prologue and no padding boundary, so
nothing else finds them. It is now covered by `_pass_imm_ref_targets` and
`_pass_data_ptr_targets`, which reached the same conclusion from the other
direction.

### DarthSidious666 — [@DarthSidious666](https://github.com/DarthSidious666)
- **Implemented the missing `tools/abi_analysis` (#6)** — the pipeline had a
  hole in it: `tools.recomp` looked for `abi_functions.json`, warned when it
  was absent, and then fell back to `cdecl` / 0 params / `int_or_void` for
  every single function, because the tool that was supposed to produce that
  file did not exist. Recovers calling convention (including thiscall from
  ecx-read-before-write), parameter count from the `ret` immediate, return-type
  hints and frame shape, so the generated signatures are real.
- Also **diagnosed the tail of issue #2**, narrowing it from "recomp crashes"
  to the specific missing tool, and posted a workaround before the PR.
- **D3D8 texture translation (#17)** — 4,096 lines across the D3D8 layer, and
  the largest single contribution to it so far. All 66 Xbox `D3DFMT_*` formats
  mapped to DXGI (33 swizzled, 20 linear, plus the float/16-bit-pair/10-bit and
  DXN/DXT3A/DXT5A/CTX1 extended set), cube textures as a D3D11 `Texture2DArray`
  with full mip chains and per-face unswizzle, volume textures as `Texture3D`
  with 3D Z-order unswizzle, and the software channel conversions for the
  formats whose memory layout does not map straight onto a DXGI equivalent.
  Shipped with `tests/d3d8_smoke`, which compiles the real `d3d8_resources.c`
  against stub device accessors so the format tables, swizzle classification
  and conversions are checkable with no D3D11 device — which is what made a
  change this size reviewable at all. `docs/technical/gap-analysis.md` marks
  the approximate maps that still want in-game validation rather than claiming
  them.
- **Kernel ordinal routing (#25)** — 20 more routes (SMBus, PCI config space,
  IRQL, EEPROM save, semaphores, FP-state save/restore, `KeWaitForMultiple-
  Objects`, and the rest), and more valuably the memory-model corrections
  behind them: the allocator bridges now answer from the *guest* heap instead
  of handing back a 64-bit host pointer for the title to truncate to four
  bytes and dereference, `RtlInitUnicodeString` and `ObReferenceObjectByName`
  write 4-byte guest fields rather than host-width ones, and a 64-bit return is
  split across `g_eax`/`g_edx`. That took the image from 150 to 170 of 371
  ordinals routed. It also closed every ordinal Half-Life 2 was hitting
  unbridged at runtime — `AvGetSavedDataAddress`, `HalReadWritePCISpace`,
  `MmFreeSystemMemory` and `ObfDereferenceObject` — which now log none.
- **Routed all 371 kernel ordinals (#32)** — the remaining ~136 unrouted
  exports used to fall through to a silent return-0, which is the worst kind of
  stub: the title carries on with a plausible answer it never asked for. Real
  implementations where the Win32 mapping is clear, documented stubs where it
  is not, across Dbg, Ex, ExfInterlocked, Fsc, Hal, Interlocked, Io, Kd, Ke,
  Mm, Nt, Ob, Ps, Rtl and the port-I/O ordinals, plus data-export bridges at
  guarded KDATA offsets. The structural piece is a guest-VA to host-HANDLE
  shadow table: a `KEVENT`/`KSEMAPHORE`/`KMUTANT` created through
  `KeInitializeEvent` lives in *guest memory* and is not a handle, and
  `KeSetEvent` and the `KeWaitFor*` pair had been treating the VA as one.
  Also found the audit itself broken and passing — `test_bridge_ordinals.py`
  anchored its regexes on the *name* `stdcall_args_for_ordinal`, the bridge
  file gained a comment mentioning it, the comment matched first, and the whole
  check was silently skipped.
- **Two generator bugs that stop the build (#28)** — `cmovcc` reads CF exactly
  as a `jcc` does, but `_function_needs_cf` scanned only `jcc` and `setcc`, so
  a `cmovb` after an `add` generated `if (_cf)` with `_cf` never declared. And
  a guest function whose recovered name is a reserved C identifier or a Win32
  export collides at compile or link time: Black has a function literally named
  `onexit`, which is C2373 against UCRT's, and Nightfire re-exports shims named
  exactly like the APIs they wrap, which is LNK2005 against `kernel32.lib`.
  Both now take the same `_<addr>` suffix `func_id` already gives duplicate
  names, applied everywhere a name becomes a C token.
- **Reported that the Ghidra pipeline had a version pinned into it (#26)** —
  the runner defaulted to a versioned install path, which dates the script the
  first time anyone updates Ghidra, and they were ten revisions ahead of the
  pin. It takes the newest `ghidra_*_PUBLIC` under `GHIDRA_ROOT` now, and
  accepts `GHIDRA_ROOT` itself being an install for anyone who keeps it
  unversioned. Also confirmed [XboxDev/ghidra-xbe](https://github.com/XboxDev/ghidra-xbe)
  works on current Ghidra despite its version-mismatch warning, and built one.
- The same PR **took Burnout 3 out of the tooling** — hardcoded title strings
  in the parser, disassembler, func_id and translator replaced with a shared
  config, and the Linux default paths made generic.

### dplewis — [@dplewis](https://github.com/dplewis)
- **`ReleaseMutex` reported success for a release it did not perform (#18)** —
  the POSIX shim returned `TRUE` unconditionally, so a thread releasing a mutex
  it did not own got success, and `xbox_NtReleaseMutant` passed
  `STATUS_SUCCESS` back to the guest for a release that did nothing. The guest
  then carried on believing the mutex was free while it was still held. Found
  by reading for a missing `ERROR_NOT_OWNER`, which turned out to be the
  smaller half of the problem. Also set `ERROR_INVALID_HANDLE` on the
  bad-handle path, which had been a bare `FALSE` with no error set.
- **Built the macOS path he had scoped (#20)** — the Darwin half of
  `win32_compat`, `mach/mach.h` for the memory queries with no
  `GlobalMemoryStatusEx`, and honest `TODO`s where the platform has no
  equivalent rather than a silently wrong one: macOS has no
  `MAP_FIXED_NOREPLACE`, and plain `MAP_FIXED` would unmap whatever already
  lives at the address instead of failing, so the note names `mach_vm_map` with
  `VM_FLAGS_FIXED` as the way through. Also replaced `wcslen` with the
  project's own `xbox_wcslen`, which is the one functional change: the CRT's
  operates on 32-bit `wchar_t` and the Xbox `WCHAR` is 16-bit.
- **Restored the POSIX build (#27)** — broken four ways by recent changes.
  ISO C99 dropped implicit declarations, so the missing includes were errors
  rather than warnings; `strtok_s` is MSVC's spelling and POSIX has
  `strtok_r`; `GetFileSizeEx` and the Slim reader/writer locks had no POSIX
  implementation at all. The SRWLOCK note is the good part: an `SRWLOCK` is
  usable straight from `SRWLOCK_INIT` and is by definition taken from several
  threads with nothing else held, so unlike the condition variables — whose
  lazy init is covered by the caller holding the paired critical section — its
  first use genuinely races, and it is serialised accordingly with a plain
  atomic load on the fast path. Also spotted that the FATX geometry constants
  were defined inside the `_WIN32` half and referenced from the POSIX half, and
  hoisted them above the backend split rather than duplicating the 0x4000 that
  Half-Life 2's CRT init requires.
- **Scoped the macOS port (#19)** — an accurate, specific list of what stands
  in the way (`MAP_FIXED_NOREPLACE`, `memfd_create`, `GlobalMemoryStatusEx`,
  SDL2/epoxy) rather than a request, which is the useful kind of issue.

---

## Issue reports and testing

### Tiptup300 — [@Tiptup300](https://github.com/Tiptup300)
- **Found that every documented step of the getting-started guide was broken
  (#1)** — and found it on Linux, which is not the platform any of it had been
  tried on. The report walked the whole pipeline: `tools.xbe_parser` not being
  runnable as a module, step 2 never emitting the JSON that step 3 requires,
  the ABI file that does not exist, and the crash at the end of `tools.recomp`.
  That single issue is the origin of the pipeline fix in `21488f4` — and of the
  repository having a LICENSE file at all, which the README had claimed for
  months without one actually existing.

### SpringierTrain — [@SpringierTrain](https://github.com/SpringierTrain)
- **Asked whether Half-Life 2 could be ported (#12).** It could, and the asking
  is what started it. HL2 is now the toolkit's largest target and its most
  productive one: the carry-flag, `bt`/`bts`, `rep movs`, atomics, per-thread
  TIB, function-boundary and SSE-compare fixes all came out of making that one
  title load a level, and every one of them is in the shared toolkit rather
  than the title.

### M0RSM4LLEO — [@M0RSM4LLEO](https://github.com/M0RSM4LLEO)
- **Reproduced and pinned down the getting-started failures (#2)** with the
  kind of detail that makes a report actionable: exact commands, exact
  tracebacks, tool versions, and the observation that `tools/xbe_parser` was
  the only tool package with no `__main__.py` while every other one had it.
  Kept testing through each fix and reported what broke next, which is how the
  `write_summary` crash at the very end of a full run got found.

---

## A note on AI-assisted contributions

Parts of this project — and some contributions to it — were developed with the
help of AI coding tools. That's welcome here: what matters is that every change
is understood, reviewed, and verified by a human before it lands. If you used
an assistant, just say so in your PR (several contributors have) and make sure
you can stand behind the result.
