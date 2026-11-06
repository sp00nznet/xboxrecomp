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

*Display gamma (#46)*
- **`SetGammaRamp` and `GetGammaRamp` were empty stubs** — both took their
  arguments and dropped them, so a title that dims the screen to black for a
  fade, or ramps back up on a load, simply did not. Silent, and invisible until
  you know the fade is missing rather than instant. Implemented as a
  presentation-time transform, which is the part worth having: the ramp is
  applied to the backbuffer on the way to the swap chain and the guest's own
  pixels are snapshotted first and copied back after, so a title that reads its
  backbuffer back still sees what it drew, and a failed `Present` does not leave
  gamma baked into guest memory. Runs on a deferred context so the game's bound
  pipeline survives, and an identity ramp costs no GPU work at all. Shipped with
  a WARP regression that reads displayed pixels back across all three
  presentation entry points, a real DXGI `Present`, and teardown.

*Also raised: stored code pointers (#13).* The gap is real and was found
independently while bringing up Half-Life 2 -- functions reachable only as an
address in a table have no call site, no prologue and no padding boundary, so
nothing else finds them. It is now covered by `_pass_imm_ref_targets` and
`_pass_data_ptr_targets`, which reached the same conclusion from the other
direction.

- **Deterministic x86 differential fuzzing (#62)** — seeded instruction
  sequences and boundary-heavy inputs through the existing native-versus-lifted
  runner, reproducible by seed and case index.
- **`tools/doctor.py`, a bring-up report (#64)** — function recovery,
  identification and ABI artifacts, translation statistics and runtime warnings
  in one read-only pass, with ranked investigation priorities.
- **A Media Foundation WMA-to-PCM backend (#65)** — with a synthetic CC0
  fixture and an injected read-error case, so the failure path is tested rather
  than assumed.
- **An inventory of flat basic-block dispatch (#63)** — measures what a
  byte-indexed x64 dispatch table would cost before anyone writes one.

*v0.12.0 — branches that could not be taken, and fragments that were one function (#110–#116, #120, #131)*
- **`loop`, `loope` and `loopne` were never lifted (#110)** — they neither
  decremented ECX nor branched on anything but a `_flags` variable nothing
  assigns, so every counted loop ran its body exactly once, and the tracked
  comparison was wiped for the jcc after it.
- **The sign of a narrow result was read at 32 bits (#120)** — after an 8- or
  16-bit add, sub, logic op, neg or shift the result is zero-extended, so `js`
  was never taken and `jns` always was.
- **Switch arms past their function's recorded end had no body (#131)** — when
  the function list cuts a function at its own switch, the lifter emits the
  indexed jump as an indirect tail call and the runtime has nothing to dispatch
  to. Each such arm is recovered as an entry of its own, only when it can run
  whole; on Wreckless that is 33 new entries and every existing body
  byte-identical.
- **Opt-in, fail-closed function coalescence (#111–#116)** — a six-PR series
  adding `--coalesce-functions`: explicit JSON owner bounds merge fragments
  that identification wrongly split, and every merge that cannot be proven is
  refused rather than forced. Along the way it made recovered CFG evidence
  authoritative for ownership, kept callback tables reachable only through
  recovered code, refused jump tables whose scale is not a dword stride, and
  made `iret` terminal.

*v0.13.1 — a function list cut in the wrong place (#173–#175)*
- **Arms after their own inline jump table (#173)** — MSVC's hand-written CRT
  `memcpy` puts each table before the arms it indexes, so the function list
  ended at the table and the arms, which branch back into the body, had no
  owner. The function is now extended over its tables when every path stays
  inside.
- **Gap aliases spanned the whole gap (#174)** — an alias entry with no
  enclosing body ran to the next function start measured before the other
  aliases existed, so DOA3's 25 C++ initialisers each lifted the same ~100 KB,
  and one ran past its section into data. Each now ends after its own body.
- **Calls skipped the hand-written wrapper (#175)** — renaming a wrapped body
  to `sub_X_gen` renamed every reference to it too, so direct calls and the
  dispatch table bypassed `recomp_manual.c`'s `sub_X`.

*v0.14.0 — Dead or Alive 3's missing callbacks, and a differential tester (#176, #182, #183)*
- **Game callbacks static discovery missed (#176)** — DOA3 hands the engine
  callbacks that sit outside every bounded table, or in gaps of unlisted code.
  They are recovered from reachable immediates and tail branches, and a weak
  immediate must start on a code boundary. That one rule rejected all the
  false starts review found on Wreckless.
- **Callbacks and tails the disassembler missed (#182)** — a closed-CFG proof
  replaces the instruction budget, late table aliases join the tail closure, and
  out-of-phase sweep bytes realign on seeds. Each fix cleared a stop the
  runtime hit on DOA3.
- **x87 and SSE forms that lifted to the wrong value (#183)** — `sahf`,
  `fcomi`, tbyte loads and stores, `ficom`, the trig 2^63 limit, partial
  `fprem`, `cvtss2si` rounding, `rcpss`/`rsqrtss` estimates and the double lanes
  of SSE2 scalar ops. All of them were found with `tools/recomp/fpdiff.py`, which
  runs a snippet lifted and on the CPU and compares the two.

*v0.17.0 — #197 split into its parts, and the flag cost answered (#276-#280, #309-#311)*
- **Flags across calls, at +0.07% instead of +32.7% (#311)** — a helper that
  answers in ZF or CF lost them at the call. #197 published flags at every
  boundary and made generated C a third larger; this version publishes only
  where the far side reads them: 86 sites in 45 of DOA2U's 15,339 functions.
- **Narrow immediates read at full width (#278)**, **OF and PF with no rule
  after `adc`, `sbb`, `neg` and shifts by one (#276)**, **unmasked shift
  counts, narrow `mul` writing EDX, and `fnsave` (#277)** — each checked
  against the CPU through fpdiff's new integer harness, nearly 3 million
  vectors for the flag consumers alone.
- **Dead bytes lifted as code (#279)** — a table after a tail jump became a
  `jo` on `_flags`.
- **A stale general-purpose EBP across calls (#280)**, found in a DOA3
  geometry helper.
- **A lone switch arm before a null slot (#310)** and **`wbinvd` (#309)**.

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
- **Noticed that `tools.abi_analysis` was missing from the getting-started
  guide** — "an instruction that's omitted (I don't know if it's crucial or
  not but I run it to make sure)". It is crucial: without
  `abi_functions.json`, `tools.recomp` warns once and then falls back to
  `cdecl` / 0 parameters / `int_or_void` for every function in the game. The
  recompile succeeds and the signatures are all wrong, which is the quiet
  failure shape this project keeps running into. Also the person answering
  most of the newcomer questions in the Discord, which does not show up in a
  commit log anywhere.

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
- **Filled in the Darwin `TODO`s left behind by #20 (#39, #40)** — the honest
  placeholders from #20 became implementations. `IsDebuggerPresent` now reads
  `P_TRACED` from `KERN_PROC_PID` on macOS and `TracerPid` from
  `/proc/self/status` on Linux instead of answering "no" everywhere;
  `SecureZeroMemory` uses `memset_s`; and `GlobalMemoryStatusEx` assembles the
  Darwin answer from `hw.memsize`, the Mach VM statistics and `vm.swapusage`.
  The best of them is `anon_map_fd`, which on macOS had been returning a literal
  `0` — a valid file descriptor, and specifically *stdin*, so every file mapping
  on that path was quietly backed by the wrong thing. Now `shm_open` with an
  `O_EXCL` unique name, unlinked immediately.
- **`InterlockedCompareExchange64` and the waitable-timer handles (#38)** — the
  64-bit compare-exchange the CRT's own atomics reach for, and the timer objects
  a title uses to pace itself.
- **A 32-bit x86 oracle that does not need Windows (#52)** — the conformance
  suite proves the lifter correct by running each snippet as real x86 and
  comparing, which had meant a 32-bit MSVC and therefore Windows; everywhere
  else the suite skipped, and a skip proves nothing. A `linux/386` container now
  supplies the toolchain and the CPU while the lifting stays on the host, with
  the MASM snippets rewritten into the GAS Intel dialect and the bytes recovered
  from `objdump` instead of `/FAc`. Two details make it trustworthy rather than
  merely green: what is substituted is the toolchain and never the comparison,
  and the corpus and XBE phases — which genuinely need PE linking — report as
  *skipped*, never as passed. The sharpest find is in the harness: the native
  side deliberately runs the x87 at 53-bit precision, and musl's i386 `libm`
  needs extended precision for its argument reduction, so leaving that setting
  in force made `cos(100.0)` come back as -1.27e16. The control word is restored
  before the lifted side runs.
- **Scoped the macOS port (#19)** — an accurate, specific list of what stands
  in the way (`MAP_FIXED_NOREPLACE`, `memfd_create`, `GlobalMemoryStatusEx`,
  SDL2/epoxy) rather than a request, which is the useful kind of issue.
  And closed the loop on it: a fresh macOS install of v0.15.0 ran
  Burnout 3 through boot, the EA, Criterion and RenderWare logos to its title
  screen, with the keyboard working.

- **The Xbox memory model on a POSIX host (#60)** — and five bugs found by
  making it actually run. The sentinel bug: `for (i = 0; try_bases[i] != 0 || i
  == 0; i++)` stopped *at* the zero rather than using it, so the "let the OS
  choose" fallback never ran — invisible on Windows, fatal on arm64 macOS where
  every fixed base sits inside `__PAGEZERO`. `MapViewOfFileEx` used bare
  `MAP_FIXED`, which silently unmaps whatever occupies the range while Win32's
  contract is to fail, so with an OS-chosen base the 28 mirrors landed straight
  through the process's own libraries: the SIGSEGV was not a failed mapping but
  a successful one on top of something live. `VirtualFree(ptr, 0, MEM_RELEASE)`
  returned TRUE without unmapping. Shutdown released none of five regions. And
  `MmGetPhysicalAddress` had two implementations that disagreed — the bridge
  translated, the kernel returned its argument unchanged — so the answer a
  title got depended on which dispatch path it took.
- **Docker and macOS setup for the conformance suite (#59)** — two images,
  because no single one does both halves on Apple Silicon: `link.exe` never
  finishes under 32-bit emulation, and Rosetta cannot execute 32-bit x86 at
  all. Build on amd64, execute on i386.
- **`test_unmangled_names_are_rejected` asserted a fact about Windows (#61)** —
  `onexit` is a Microsoft CRT name and free everywhere else, so the negative
  control demanded a compile failure that could not happen off MSVC.

*v0.17.0*
- **POSIX HANDLEs as table indices (#241)** — a handle was the object pointer,
  so `KeWaitForSingleObject`'s cast of a guest KEVENT locked a mutex inside
  guest memory and Burnout 3 faulted before its title screen. Making handles
  checkable then exposed two silent bugs: priority calls that never did
  anything, and a multiple-object wait that timed out on a bad handle instead
  of failing.
- **`RECOMP_RASTER_THREADS` on POSIX (#242)** — documented, and silently one
  thread: 110 → 278 Mpx/s on an M4.

### GTTeancum — [@GTTeancum](https://github.com/GTTeancum)
Fourteen fixes in two days, found by driving a real title through the pipeline
and chasing each wrong answer back to its cause. Every one arrived with a
regression that fails without the fix — several compile the lifter's own
output and sweep it against x86's definitions, and the kernel ones dispatch the
real thunk through synthetic guest memory, so they need no game files.

*x87 (#47, #49)*
- **`FIST`/`FISTP` ignored the guest's rounding mode** — they lifted to
  `llrint`, which rounds by the *host's* mode, while the guest's control word
  said something else. The era's CRT `_ftol` sets round-toward-zero and then
  converts, so the truncation it is asking for silently became round-to-nearest:
  the 255.5 that a colour-packing path expects to floor to 255 became 256, and
  every channel of white wrapped to 0. Now honours all four RC modes, with
  ties-to-even done properly, and stores the integer-indefinite value on a
  masked invalid conversion instead of whatever the cast happened to produce.
- **`FXAM` was not implemented at all, and the status word was rebuilt from
  scratch at every read** — `fnstsw` derived C0/C2/C3 from the comparison
  result alone, so a classification instruction contributed nothing and a title
  asking "is this a NaN, a zero, an infinity?" got the last *compare* back
  instead. The condition bits are shared x87 state now, written by `fxam`,
  `ftst` and the `fcom` family alike, which also means they survive a call the
  way the hardware's do. Denormals are classified as normals with the reasoning
  written down: a binary64 subnormal *is* a normal extended-precision value, so
  that is not an approximation.

*Flags and control flow (#41, #44, #48, #50)*
- **`INC`/`DEC` destroyed the carry flag they are defined to preserve** — the
  whole point of `inc` over `add reg, 1` is that CF survives it, and the lifter
  dropped it, so a `jb`/`jae` after one read a stale or absent carry. Their own
  flags were no better: `js`, `jl` and friends re-read the destination at the
  branch, so a `mov` in between changed the answer, and OF and PF were not
  modelled at all. Now snapshotted at the instruction, with OF derived from the
  one input value that can overflow at each width.
- **`SAR` shifted at 32 bits regardless of operand width** — every narrow read
  arrives zero-extended, so `(int32_t)` on an 8- or 16-bit operand never saw a
  sign bit and the shift was a logical one wearing an arithmetic cast. `sar al,
  1` on 0x80 gave 0x40 where x86 gives 0xC0 — a negative number halved into a
  positive one, which is how a fixed-point divide or a signed average goes
  wrong without ever faulting. The count is masked to five bits as x86 does,
  and CF is taken from the sign-extended value so a count at or past the
  operand width still reports the sign bit.
- **A classic `push ebp; mov ebp, esp` prologue pushed an uninitialised C
  local** — the generated `ebp` was only seeded from the caller's frame for
  *frameless* functions, and a function with a real prologue reads it on its
  very first statement. So the guest's saved-frame chain got an indeterminate
  word, which the epilogue then pops back and a frame walker may follow.
- **Comparison snapshots were discarded at any control-flow join whose
  predecessors compared different registers** — the operands differ but the
  runtime slots they save into do not, so a shared consumer can use whichever
  path actually ran. The merge is deliberately narrow: same operation, same
  width, and arithmetic states still excluded, because those reconstruct their
  operands.

*Disassembly (#42)*
- **A bare immediate could split an instruction the sweep had already
  decoded** — an integer constant that happens to land in an unclaimed code gap
  is weak evidence, and treating it as a function entry carved the real
  instruction stream in half. It defers to the existing decode now unless the
  target independently probes as a prologue, a constant stub or a vcall thunk,
  which keeps the out-of-phase-sweep recovery that made the pass worth having.

*Kernel ABI (#51, #53, #54, #55)*
- **`NtQueryDirectoryFile` was declared with nine arguments and has ten** — the
  missing `FileInformationClass` meant every argument after it was read one slot
  early, the search mask and restart flag came from the wrong places, and the
  bridge popped 36 bytes where the guest had pushed 40. A four-byte stack leak
  per call, which is the kind that runs for a while and then does not. The
  enumeration also returned the host's `.` and `..`, which FATX does not have.
- **`KfRaiseIrql` and `KfLowerIrql` are `__fastcall`** — their argument arrives
  in `CL`, and the bridge was reading it off the stack. The cleanup table
  already said zero bytes, so the value read was whatever sat at that address.
- **Counted object names were read as NUL-terminated (#53)** — the XDK passes a
  directory name with a trailing wildcard and `Length` shortened to exclude it,
  without writing a NUL at the new end, so the path picked up the wildcard and
  whatever followed it in guest memory. The same PR made
  `ObjectAttributes->RootDirectory` real rather than always `NULL`, which is
  what lets a title open a save file relative to the directory handle it just
  enumerated.
- **`MmGetPhysicalAddress` returned the virtual address unchanged (#55)** — for
  the contiguous arena, which is the virtual *window* onto physical RAM, that
  hands a DMA consumer an address with the high bit still set. Returns the
  offset within the window now, and leaves identity mapping alone everywhere
  else.

*Kernel state (#43, #45, #56)*
- **The pending kernel-dispatch slot was a single process-wide global (#43)** —
  two threads resolving a kernel import at the same time raced, and the loser
  invoked the *other* thread's service, popping that one's argument count off
  its own stack. Thread-local now: a one-word change, and worth finding.
- **`IdexChannelObject` was exported as an opaque self-pointer (#45)** — it is
  a structure, and guest file-close code walks `DeviceQueue.DeviceListHead` at
  +0x28, where it found nulls. Host-backed synchronous I/O never enqueues guest
  IRPs, so the honest answer is a correctly formed *empty* circular list. Also
  gave it storage of its own; the old 16-byte slot would have overlapped the key
  exports as soon as anything was written into it.
- **The EEPROM advertised mono audio with AC3 (#56)** — `XC_AUDIO` was set to
  `0x00010001`, and in that field 1 means *mono*, not stereo. So a title asked
  the console what it was plugged into and was told one channel plus an encoded
  output path that does not exist. Reports plain stereo PCM now, which is what
  the host actually plays.

### andeecollard — [@andeecollard](https://github.com/andeecollard)
- **`jbe` and `ja` after `and`/`or`/`xor` were folded to constants (#57)** —
  those instructions do clear CF, so `jb` and `jae` after one really are 0 and
  1, but `jbe` is CF|ZF and `ja` is !CF && !ZF, and ZF is whatever the result
  was. Collapsing all four meant `and eax, eax; jbe` never branched and `and
  eax, eax; ja` always did — wrong exactly when the result is zero and right
  the rest of the time, so it survives ordinary traffic and then takes the wrong
  arm on the empty list, the null handle, the zero count. The lifter already
  spelled both conditions out correctly two branches away, on the CF-tracked
  path.
- **The generated `ebp` local was declared without an initialiser** — reached
  the same conclusion as #48 from the other direction, and the two were merged
  together: #48 seeds it from the caller's frame, this initialises the
  declaration, and the fix wants both.
- Also **found the `SAR` operand-width and shift-count bugs independently of
  #50**, which landed first. #50's body is the one in the tree because it takes
  CF from the sign-extended value as well; the docstring explaining why any of
  it matters is this PR's.
- The tests are the part to copy: each is paired with a *negative control* that
  feeds the identical harness the pre-fix expression and requires it to fail, on
  the grounds that a sweep which passes against both spellings is testing
  nothing.

- **Rotates ran at 32 bits whatever the operand was (#69)** — every narrow read
  in the lifter arrives zero-extended, so a byte rotate happened inside a
  32-bit word: the bits that should wrap at bit 7 landed in bits 31..8 and the
  store discarded them. `ror al, 2` on 0x01 gave 0x00 where x86 gives 0x40, and
  `rol al, 16` — the identity, since the count is masked to five bits and
  *then* reduced modulo the width — gave zero. Same defect class as the `sar`
  width bug next to it, which was found while the rotates beside it were
  missed. Also fixed `ROL32(val, 0)`, which evaluated `val >> 32`.
- **`bts`/`btr`/`btc` reported the bit they left, not the bit they found
  (#70)** — all four bit-test instructions copy the tested bit into CF, but
  only `bt` leaves it alone. The carry condition was rebuilt at the consumer by
  reading the bit a second time, which for the other three reads back what the
  instruction had just written: `jb` after `bts` was always taken, after `btr`
  never, after `btc` exactly backwards. That is the test-and-set idiom — *did
  I claim this, or was it already taken?* — reading its own answer. MSVC emits
  it for lock acquisition and for the character-map loops behind
  `strpbrk`/`strspn`/`strcspn`.
- **SF after a compare was computed with signed overflow (#71)** — `js` came
  out as `(int32_t)(_fas - _fbs) < 0`, and that subtraction overflows for
  exactly the inputs the sign flag is being asked about. `cmp 0x80000000, 1`
  leaves 0x7FFFFFFF on the hardware so SF is 0; in C it is `INT_MIN - 1`, and
  from -O1 the compiler folds `a - b < 0` into `a < b` and answers 1. The
  emitted program's meaning changed with the optimisation level. Subtracting
  unsigned at the operand's own width and taking the top bit is SF exactly,
  with no undefined case.
- **`popfd` was in the set of instructions that preserve EFLAGS (#72)** — next
  to `pushfd`, which belongs there. `popfd` replaces every flag, so `cmp eax,
  ebx; popfd; je` resolved the branch from the comparison the restore existed
  to discard. The file already knew: the `neg`/`sbb` peephole four hundred
  lines away carries an explicit `!= "popfd"` guard that the main tracking loop
  never got.
- **`shld`/`shrd` ignored x86's count rules, and a zero count wrote (#75)** —
  the count is masked to five bits and a masked count of zero must leave the
  destination alone. The emitted expression built `src >> (32 - cnt)`, so a
  count of zero shifted by the full width: undefined in C, and where the host
  reduces the shift amount modulo the width it returns `src` whole, landing
  `dst | src` for an instruction that must not write at all. `_lift_shift`
  states the rule for `shl`/`shr`/`sar` next door; the double-precision pair
  never got it.
- **`movsd` is two instructions and the dispatcher picked by name (#76)** —
  the string `MOVSD` copies a dword `[esi]` to `es:[edi]`; the SSE2 one moves a
  scalar double in or out of an xmm register. The string branch runs first and
  matched on the mnemonic, so `movsd xmm0, qword ptr [eax]` walked `esi` and
  `edi` and touched neither operand the instruction names. Silent: a load that
  never happens and a register that keeps its old value.
- **The result-setter family rebuilt its condition at the consumer (#77)** —
  `and`/`or`/`xor`, `add`/`sub`, `adc`/`sbb`, `neg` and the shifts all write
  their destination, and the jcc reading their flags can be blocks later, so
  `and eax, 0x0F; mov eax, 0x99; jne` asked about 0x99. `inc`/`dec` already
  published `_fa` at the write for exactly this reason — two instructions out
  of fourteen — and this extends that rule to the rest.

All seven found by differential fuzzing against an independent x86 core, and
every one paired with the negative control described above.

*v0.12.0 (#89, #117, #121, #124, #126, #129, #130)*
- **Guest buffers bounds-checked before the host touches them (#89)** — the
  file and info exports passed a guest pointer and a guest length straight to
  the host, so a bad buffer walked the host past the end of guest memory and
  crashed inside the kernel implementation. It now fails with
  `STATUS_ACCESS_VIOLATION`, which the title has a branch for.
- **An untranslated instruction says so at runtime (#117)** — it used to become
  a bare comment, which the compiler reads as nothing, so the instruction
  vanished and the failure surfaced somewhere downstream. `RECOMP_UNIMPL`
  names the site the first time it is reached.
- **`repe cmps`/`scas` produce CF (#124)** — without it `memcmp` and
  `std::string::compare` never answered "less", and a zero-count compare
  reported "unequal", breaking sorted lookups.
- **`frndint` rounds under the guest's control word (#126)** — it used the
  host's mode, so the CRT's `floor` and `ceil` both rounded to nearest and
  JSRF read its animation tables out of range.
- **ADPCM's reserved header byte is not validated (#121)** — hardware accepts
  any value there, and rejecting it silenced about 4% of JSRF's audio blocks.
- **The returning-body probe follows a switch (#129)** — a function reached
  only by address that opens with a switch was never detected.
- **Indirect calls name their site (#130)** — per-site target sets recorded at
  runtime become exact-match guarded direct calls on the next generation.

### fearkov — [@fearkov](https://github.com/fearkov)
A bring-up batch on *Shin Megami Tensei: Nine* and DDS9, each item a place
where the runtime stopped one step short of something a title needed and said
nothing about it.

- **`RtlNtStatusToDosError` answered 317 for every status it did not know
  (#80)** — 317 is `ERROR_MR_MID_NOT_FOUND`, an honest default for an unmapped
  failure and the wrong answer entirely for a status that is not one. A
  resource loader that starts an asynchronous read and marks the object as
  loading only on `ERROR_IO_PENDING` never marked it, so the poll that finishes
  the load reported "not started" on every frame and the title sat in its first
  boot state forever with input, audio and rendering all working.
- **`NV097_DRAW_ARRAYS` was not handled by the pushbuffer executor (#81)** —
  and the gap is silent: `BEGIN_END` arrives, `END` arrives, and in between
  comes a run description rather than the index list the draw path wants, so
  every batch is dropped with `idx_count == 0` and the report says `draws 0`. A
  title submitting geometry every frame looked exactly like one submitting
  none. Decoding the run into indices turns the same seconds of the same title
  into 30,541 draws — checked against the pixel count rather than asserted.
- **The AC'97 channel reset had to complete on the write (#82)** — MSVC
  hoisted the load out of the wait loop, so the title reads the control
  register exactly once, a few instructions after writing it, and spins forever
  on that one value. A thread clearing the bit afterwards is racing a window a
  few instructions wide and loses. Trapping the write instead — read-only page,
  single-step, mask RR out — is the only version that is there in time.
- **The watchdog could not read the registers a title hangs on (#83)** — the
  peek accepted only addresses below 64 MB, which reads as "RAM" but is not the
  question: every aperture this runtime maps is just as dereferenceable.
  Peeking `0xFD800044` printed nothing at all, not a value and not an error —
  so the design note saying *"run the title and the watchdog sample will name
  the register"* was not true for the case it was written for.
- **The emulated APU was unreachable (#84)** — `src/apu/` is a working ~2,500
  line extraction of xemu's MCPX APU that nothing in the tree could call.
  `apu_hook_handle_mmio` sits under a comment reading *"called from VEH in
  main.c"* and no `main.c` called it; `g_apu_state` was never assigned; and
  `xbox_apu` never linked `xaudio2_8`, which stayed invisible for as long as
  nothing referenced the archive. Three independent gaps, each sufficient
  alone.
- **USB enumeration stopped one step short (#85)** — six faults, each fatal on
  its own. A driver starting a fresh reset writes `SetPortReset` and
  `ClearPortResetStatusChange` in the same word, and the write-1-to-clear line
  ran *after* the handler set PRSC and wiped the bit that write had just
  raised, so the port reset forever while looking connected, enabled and
  powered the whole time. Plus: descriptors live in the contiguous window that
  the bounds check rejected, no frame clock, a control data stage that
  restarted every descriptor, only the control list walked, and a done queue
  never retired.
- **The DVD device open and the media check behind it (#74)** — a title
  checking its media opens `\Device\CdRom0` itself, the bare device, and the
  path table carried only the form with the trailing separator. The open
  failed, the title read that as "no disc" and exited through
  `HalReturnToFirmware` before drawing a frame.
- **Cross-compiling the runtime with MinGW (#73)** — two macro collisions, 51
  errors. `KernelMode`/`UserMode` are ordinary words that the Windows SDK uses
  as struct member names, so object-like macros rewrote those declarations to
  `WINBOOL 0;`; enum constants coexist with them. And the `__debugbreak` guard
  tested `_MSC_VER` where it needed `_WIN32`, since MinGW is neither and
  declares a real one.
- **A loop head lost its exit test because the back edge had no state yet
  (#86)** — blocks are lifted in address order, so the predecessor on a back
  edge sits *after* the block it reaches and has no out-state on a single pass.
  The join correctly refuses to guess, the `jcc` at the top falls back to
  `_flags` — a variable nothing ever assigns — and the branch compiles as never
  taken. In the middle of a function that costs a little accuracy; at the top
  of a counted loop it removes the loop's only exit. In DDS9's XMV row padding
  the loop stored eight bytes and advanced `edi` by sixteen forever, walked off
  the framebuffer, and took the process with it. Settling the state to a fixed
  point before emitting fixes it; `sub eax, ecx` and `dec eax` are different
  setters that agree on the one thing a `jz` is asking.
- **`__SEH_prolog` detection required the four-push form (#87)** — the second
  byte marker is `lea ebp, [esp+0x10]`, and that offset counts the slots the
  helper pushed before it: the three-push form lands on `0x0C` and was
  undetectable. Silent, because "not found" is indistinguishable from a CRT
  that has no `__SEH_prolog`, so every SEH function kept its caller's stale
  `ebp` — the first frame-relative store landed in the caller's frame and the
  epilogue cut the stack back to it. A title can also link more than one:
  DDS9 carries both forms, and returning the first match meant which one won
  depended on nothing but the lower address.

*v0.12.0 — twenty-four PRs (#90–#100, #102–#109, #118, #119, #122, #123, #125)*
- **A join reached by `test X, X` and `cmp X, 0` inherited no flags (#122)** —
  the two leave every flag identical, but the snapshot reconstructs them
  differently, so the join refused to merge and the branch compiled dead. In
  SMT: Nine's video decoder that fired the IDCT's DC-only shortcut for every
  coefficient group and smeared every picture horizontally.
- **`rcl` and `rcr` were never lifted (#104)** — emitted as comments, which
  silently broke MSVC's 64-bit divide helper and with it every `long long`
  division. Plus a differential test of every MMX helper against the host's own
  instructions (#105).
- **USB brought to a working pad (#90, #91, #96, #107, #118)** — four root-hub
  ports, a 4 ms frame tick, the done-queue handshake, an interrupt held off
  while the guest is at raised IRQL, a watchdog that asks whether the driver
  acknowledged rather than whether the status repeated, and a `GET_REPORT`
  answered instead of stalled.
- **The NV2A path (#97, #99, #102, #103, #108)** — primitive numbers one too
  low, GET running ahead of the executor, a ring wrap dropping a submission, no
  YUV sampler, no alpha blend, and a window showing the frame being drawn
  rather than the one finished.
- **`KeQuerySystemTime` at the console's resolution (#119)**, asynchronous
  reads answered the way they were asked (#93), and a crash report that says
  when the stack is gone (#95).
- **Bring-up instruments** — `RECOMP_WATCH` (#92), `--force-return` (#100),
  `RECOMP_DSP_ACK`/`RECOMP_POKE` (#98), `RECOMP_PAD_PRESS` (#94),
  `RECOMP_KEYBOARD` (#106), `RECOMP_TEX_DUMP_EVERY` (#123), raised-IRQL
  holder tracing (#125), and the `--split 250` guidance (#109).

### HEROTRUTH — [@Heromachine](https://github.com/Heromachine)
- **The NV2A flip methods were matched 0x18 too low (#101)** — the executor
  compared against hand-typed offsets instead of the header's constants, so the
  frame counter counted the wrong method, `WAIT_FOR_IDLE` was swallowed as a
  flip, and the real flip methods fell through to the unhandled path.

### BearddOddity — [@BearddOddity](https://github.com/BearddOddity)
A bring-up batch on *X-Men Legends*, a title that links the XDK's own D3D,
DirectSound and USB stack. Developed with Claude Code.

- **`XBOX_THREAD_LOCAL` tested `_WIN32`, not the compiler (#157)** — MinGW targets
  Windows but is GCC, which ignores `__declspec(thread)` with only a warning,
  so on that host every kernel thread-local was one process-wide variable.
- **`NtCurrentThread()` widened as unsigned on x64 (#157)** — `DuplicateHandle`
  never recognised it, and the CRT retried forever.
- **The kernel call counter wrapped after 2^31 calls (#157)** — and "log the first
  N" became "log everything": about 1 FPS some minutes into a level.
- **`STATUS_CONFLICTING_ADDRESSES` had no DOS error (#157)** — 487 is the only
  answer on which the CRT heap tries another address when it grows.
- **A short IN packet without bufferRounding is DATA UNDERRUN (#157)** — the OHCI
  model reported success, and XAPI followed a dead TD link.
- **Documented** — four ways function detection misses functions, lifter
  gaps, template and build traps, the ABI checker's standing reports, two
  Ghidra scripts for headless triage, and a documentation index by symptom.
- **Memory never came back (#158)** — a small request after a large free took
  the whole block, `NtFreeVirtualMemory` handed a 32-bit guest slot to the
  host's `VirtualFree`, and `MmFreeContiguousMemory` gave contiguous memory to
  a heap that did not own it, so a title that frees one scene and loads the
  next ran the 64 MB window dry. All of it behind `RECOMP_HEAP_RECLAIM`.
- **A reservation above the RAM mirrors was refused (#158)** — X-Men Legends
  reserves `0x76000000` and checks it got exactly that; under `RECOMP_EXT_VMA`
  it does, and the CRT heap-grow loop that ran 161 million kernel calls is gone.
- **KEVENTs a title builds by writing the header (#160)** — with no
  `KeInitializeEvent` there was no shadow, every wait failed at once and the
  title retried forever; `RECOMP_TITLE_KEVENTS` gives them a host event, and
  `RECOMP_GUEST_LOCK` runs one guest thread at a time to rule a race in or out.
- **Two joins that read a flag nobody set (#159)** — swapped-operand float
  compares, and a result snapshot meeting a compare snapshot, both fell back to
  the never-assigned `_flags`; each edge now computes the join's condition.
  Found on a racer whose clamp always fired and zeroed its wheels' torque.
- **Diagnostics (#161)** — a guest path hook, an `[EXIT]` line when a title
  ends itself, and an opt-in direct-call profile for diffing two runs.
- **The pushbuffer executor (#162)** — a DMA-style walk (opt-in,
  `RECOMP_PB_WALK=dma`), fixed-function transform and lighting,
  anti-aliased surfaces, render back ends, and bounds on every vertex fetch.
  Making the walk opt-in came from its own Burnout 3 run, which found D3D's
  fence wait sleeping on an interrupt the runtime never raises.

*v0.17.0*
- **Case-insensitive file names on POSIX (#248)**, **read-only truncation
  denied (#245)**, **delete logging (#246)** and **`RECOMP_WAIT_LOG` (#247)**.
- **Texture filtering in the CPU rasteriser (#253)** and **strip winding
  (#249)**, both opt-in, with costs measured per path.
- **The conformance suite skipped silently on Visual Studio 18 (#250)**, and
  **every `RECOMP_*` switch on one page (#251)**: 51 had been undocumented.
- **`movsx` from `bp`/`sp` pinned by a test (#244).**

### Vlad Yanhursky — [@vyanhursky](https://github.com/vyanhursky)
Five fixes extracted from a *Def Jam* bring-up fork, each with a fixture that
fails on the old code and a real-CPU conformance case where one applies.

- **`pushad`/`popad` were lifted as TODOs (#167)** — a routine that saved
  every register with them returned with its working registers in its
  caller's state.
- **`rep movs` into the hardware aperture used host `memcpy` (#170)** — whose
  vector loads skip the element-sized accesses trapped device memory needs.
  Device, crossing, overlapping and backward copies now go element by element.
- **A failed indirect call popped the caller's arguments twice (#169)** — the
  failure path rewound over them and the caller's `add esp, N` removed them
  again, leaving ESP 4+N too high.
- **Closing a directory mid-search leaked the host search (#171)** — and a
  later handle with the same value continued the old cursor.
- **Manual entry hooks (#168)** — `sub_XXXXXXXX_enter()` in the manual file
  runs at the top of the generated function on every call, direct ones
  included, and boundary repair leaves that start alone.

*v0.14.0*
- **COMISD/UCOMISD compared the float lane (#178)** — so 1.0 and 2.0 as
  doubles could compare equal.
- **Parity `setcc`/`cmovcc` readers weren't lifted (#180)** — including the
  `pe`/`po` spellings.
- **Guest-built events in multiple-object waits (#181)** — a KEVENT the title
  built by writing its header now resolves in `KeWaitForMultipleObjects` as it
  already did in the single-object wait.
- **The C fixtures run under MSVC (#179)** — 25 tests that skipped on a
  Visual Studio-only machine now execute. Running them at `/O2` exposed a `neg`
  lift that was undefined for `0x80000000`.
- **FPREM's completion bit (#177).**
- **The POSIX directory-search leak (#201)**, found at the same time as #190
  and fixed the same way. The fix itself came in with #190; #201's standalone
  fixture, which covers live searches running side by side and restarts, is
  merged.
- **Testing other people's PRs on a Def Jam port.** The Darwin mapping fix
  (#185) was checked against the port's own regression suite. Careful test
  notes on #186, #192 and #195 found the missing `g_eflags` in the interrupt
  snapshot, `cmp; pushfd` dropping OF, and gradual starvation that the audio
  reports missed.

*v0.15.0*
- **Portable controller mapping rules (#203):** named bindings and a text
  format for them, remapping, radial deadzones, trigger thresholds and merge
  rules, rebuilt on #191's key table as asked. `RECOMP_KEYBOARD` is the first
  adopter, checked byte for byte against v0.14.0 over 224k states.
- **The `g_eflags` gap in the interrupt snapshot**, from the #186/#192 test
  notes, is fixed in v0.15.0.

*v0.17.0*
- **The disasm step went from 516 s to 92 s on Def Jam (#255, fixing #254)**
  — v0.14.0's callback probes re-decoded every gap from scratch, 25 million
  Capstone decodes; output is byte-identical.
- **Def Jam's audio clicks and holes (#261, #264)** — a shared page read as
  RAM played stray bytes (386-422 clicks in 330 s, now 1), and DirectSound's
  own sequencer restart left about 1,300 one-slice silences (now 1).
- **Executor speed-ups (#272, #273, #274)** — vertex programs decoded once,
  constant uploads off the general dispatch, combiner shaders compiled once
  per text.
- **The README's ports table (#256).**

### Leonardo Valdes Arteaga — [@eolandro](https://github.com/eolandro)
- **The runtime did not build with GCC on Linux (#172)** — the VEH code
  needed real `EXCEPTION_POINTERS`/`CONTEXT` shapes and `EXCEPTION_SINGLE_STEP`,
  and the view-length helpers were declared only for macOS. Found and fixed on
  Debian with GCC 12.

### apexJCL — [@apexJCL](https://github.com/apexJCL)
macOS, Linux and Steam Deck hosts, brought up on *BLiNX 2* and *Burnout 3*,
split from one fork into topic PRs with an index (#184).

- **Macs could not reuse an address after a failed view (#185)** — the
  `mach_vm_map` placeholder stayed mapped when the `mmap` over it failed, so
  every later view there was refused.
- **Large images were overlapped by the stack and heap (#189)** — the toolkit
  region, stack and heap now sit above the highest section. The contiguous
  arena became a page allocator that honours `MmAllocateContiguousMemoryEx`'s
  range; D3D's request for the low 16 MB used to fail once the bump pointer
  passed it. An opt-in 4 GB placeholder (`RECOMP_WIN_PLACEHOLDER`) keeps the
  aperture mappable under Proton.
- **Kernel objects and files (#190)** — guest thread objects with TLS sized from
  `TlsDataSize`, FATX overwrite and rename, partition images on POSIX, and a
  once-per-file report of missing game files.
- **Devices on POSIX hosts (#191)** — trapped MMIO on arm64 from the faulting
  instruction, one fault dispatcher for APU, AC'97 and OHCI, an SDL2 window and
  audio device, and one keyboard map on every host.
- **The APU (#193)** — physical addresses above 64 MB, pitch resampling
  (44.1 kHz material had played 8.8% fast), one trap at a time, device-paced
  output, and a DSP doorbell ack found from GPSADDR by default.
- **`in`/`out` lift to port I/O (#194)** — they were `RECOMP_UNIMPL`. The video
  encoder's field pin at 0x80C0 is modelled, which is what D3D's vblank ISR
  polls.
- **`oPos.w == 0` vertices were dropped (#188)**, and **a Proton runner for the
  Windows ctests (#187)**.
- **Diagnostics for audio reports (#195)**: `RECOMP_AUDIO_WAV` writes
  exactly what the device is given, and `RECOMP_AUDIO_HOST` lines host
  starvation up against that file. `RECOMP_APU_TRACE` adds per-voice
  statistics, `RECOMP_APU_SOLO` mixes a single voice, and `RECOMP_APU_RING`
  catches a streamed ring replaying a stale lap. These answer the two
  questions every audio report starts with: did the device run dry, and which
  voice is it.

*v0.15.0*
- **Lifter accuracy and discovery past switch tables (#186):**
  - `movsx r32, bp/sp` sign-extends.
  - `pushfd`/`popfd` go through a `g_eflags` image, with OF and PF rebuilt;
    `cmp; pushfd` had pushed OF clear.
  - `cpuid` answers as the P3.
  - x87 trig reduces with the hardware's 66-bit pi.
  - `pop esp` leaves esp at the popped value.
  - Offset frames own their frame.
  - `RECOMP_STUB_LOG` names each unresolved stub the first time it runs.
  - Functions after embedded switch tables are found.

  It was rebased onto v0.14.0 and cut down to exactly what was still
  missing.
- **Interrupts, DPCs and pacing (#192):** a one-CPU DISPATCH gate, IRQs at
  safe points, a 60 Hz vblank and flips held to it, all behind switches that
  are off by default. The Burnout 3 2-flip hang turned out to be a stale
  glue address, and was traced before the merge.
- **`Partition1\CACHE` in the save dir (#206)** and **`xinput1_4` for
  mingw builds (#221).**
- **Reviews that changed other people's PRs:**
  - the `XC_MAX_OS` DVD region offset in #216;
  - the vsync-off busy loop in #215;
  - the clip lerp NaN in #211;
  - the CLEAR_RECT rule in #212 and #222, each tested on Burnout 3 and
    BLiNX 2 before suggesting it.

*v0.17.0*
- **The gap sweep cut alias bodies apart (#262)** — a vtable-only method lost
  three of its four switch arms, and loop tails became calls.
- **`RECOMP_VBLANK=0` gave the paced schedule (#263)** with the dispatch gate
  on, and a Finder-launched macOS title ran out of file descriptors at its
  ~247th open.

### nworkers — [@nworkers](https://github.com/nworkers)
Found on *Jet Set Radio Future*.

- **`lock xadd` had no flags (#198)** — so `InterlockedDecrement; jne` never
  branched, every COM `Release` destroyed its object, and the heap's free list
  was corrupted by the double free. `FCMOVcc` was a comment, so float min/max
  never moved. `stmxcsr`/`ldmxcsr` were unimplemented, so a per-frame
  rounding-mode check read garbage. MXCSR is now modelled, and
  `cvtss2si`/`cvtsd2si` round by its RC field.
- **Branches out of alias bodies (#199)** — a vtable-only dispatcher branched
  past its own `ret` into a gap the disassembler never claimed, and each call came
  back 12 bytes short on the stack.
- **Vtable methods that never reach a `ret` in their own gap (#236)** — 103 of
  JSRF's methods were refused by the closed-body proof: guarded vcall tails,
  dispatchers whose switch arms are other methods, and error paths that leave
  by a `jmp` to a helper nobody had found. Three of them stopped the attract
  demo. Each refusal was measured and sorted by shape before any code changed.
  Along the way: MSVC's byte index table, parked after a dword jump table,
  was read as one more arm, and v0.15.0 had accepted eight such data words as
  function entries.

### LeandroSSB — [@LeandroSSB](https://github.com/LeandroSSB)
Found while bringing up *Black* (Criterion, XDK 5849).

- **The sequencer's sample-limit interrupt (#205).** DirectSound programs SSL,
  resets XGSCNT and waits for ISTS bit 3. That interrupt never came, so
  Black re-ran its audio setup forever. XGSCNT is now a real 48 kHz sample
  counter, not the host clock divided by 100.
- **Vertex-constant tracing under `RECOMP_VSH_DUMP` (#205).** This is the
  tool that found D3D reloading `c[190]`/`c[191]` every frame and ruled out
  every other channel. It also confirmed the `oPos.w == 0` diagnosis that #188
  had fixed meanwhile: Black's frontend went from 0 pixels to its boot scene
  at 42 fps.
- **A mouse and keyboard pad (#205)**, opt-in with `RECOMP_MKB` on Windows:
  raw-input mouse movement becomes stick deflection, WASD and buttons merge on
  top of a real pad, and F9 toggles capture.

*v0.17.0*
- **The MMX and SSE float helpers as intrinsics (#258)** and **`RECOMP_CS_MODE=guest`
  (#260)**, both split out of #222, the second with the review's three notes
  fixed by keeping the classification in the lock's own bytes.

### Koen Hendriks — [@koenhendriks](https://github.com/koenhendriks)
Found while bringing up *SSX 3* (PAL); see
[SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp).

- **The pushbuffer executor (#210, #211, #212):**
  - attributes with no array read their current value;
  - the fog-plane and eye-position constants;
  - the colour clamp, and mirror and border addressing;
  - clipping in clip space and at the near plane, instead of after the
    title's viewport scale;
  - render-to-texture into swizzled surfaces, for SSX 3's shadow maps;
  - the CLEAR_RECT rule (the rect alone, as xemu does), which @apexJCL's fork
    and #222 have since adopted.
- **A scalable framebuffer window, fullscreen and a D3D11 presenter (#215).**
- **The host time zone in the EEPROM settings (#216)**, so saves stop being
  stamped UTC.
- **Four root-hub ports (#214).** The first pad used to arrive as player 3;
  it's now on controller port 1.
- **Rumble from the XID output report (#213).**
- **Entry stubs (#207)**, opt-in: an unresolved target inside a function
  enters a copy of it.

### Matt Graham — [@MattSG](https://github.com/MattSG)
- **PVIDEO overlay scanout (#220).** Bink's Xbox player draws movies through
  the NV2A overlay, so they never appeared. The overlay is now scanned out at
  its scale and colour key, and STOP clears the pending banks D3D waits on.
- **Voice processor fixes (#217):**
  - ADPCM reads used to overrun the decode buffers;
  - VOICE_LOCK locks one voice rather than a whole frame of mixing;
  - volume changes ramp across the frame;
  - HRTF.

*v0.17.0*
- **The `tools.diff` family (#288, #293, #295, #296, #290, #289)** — trace
  comparison, a deterministic fuzzer against Unicorn, record decoders, local
  replay capture, register provenance and VA resolution — and the runtime
  rings that feed it (#302, #303).
- **Three kernel answers that were wrong (#291, #292, #294)** — the bare
  `\Device\CdRom0`, a directory opened without the directory flag, and DVD
  SCSI commands that reported success over the title's own heap bytes.

### Madiyar — [@kaskabayev](https://github.com/kaskabayev)
- **XDK register constants cut `.text` functions (#234).** D3D's NV2A
  constants equal `.text` addresses, and the immediate-reference pass took
  them as function starts. On *NFSU1* that split a jump-table switch.
  `--skip-library-immediates` stops it.
- **NOTICE entries for the xemu-derived NV2A core, NV2A state and APU
  headers (#233).**

### bertibertig — [@bertibertig](https://github.com/bertibertig)
Found on *SSX 3* (PAL) on Linux.

- **The POSIX file backend ignored `RootDirectory` (#237).** XAPI's save code
  opens the save directory, then each file in it by its bare name
  (`Data.ssx`). The Win32 backend joined the name to the handle's path; the
  POSIX one sent it through the path table alone, so overwriting a save
  failed with `STATUS_OBJECT_PATH_NOT_FOUND` and the game said "Unable to
  save". Came with a cross-platform test that fails 3 of 9 checks without
  the fix.

### Tzeentchnet — [@Tzeentchnet](https://github.com/Tzeentchnet)
- **Reported that `XC_VIDEO` sat in the wrong half-word (#301)** — XAPI reads
  `raw >> 16 & 0x5F`, so every title saw 0. The report came with the EEPROM
  bit layout, two independent sources and the policy questions to settle
  first; the fix followed it directly.
- **Extra thunks that weren't ordinals (#297)**, **mono HRTF voices copied
  to both channels (#298)**, and **torn framebuffer snapshots (#300)**.
- **SDL controllers that couldn't hotplug (#305)**, **a pre-snapshot backend
  readback (#285)**, and **`XBOXRECOMP_BUILD_D3D8` (#304)** for embedders
  without libepoxy.
- **`PMADDWD` overflow (#284)**, **the MMX test on ARM64 (#283)** and **the
  unimplemented-instruction report's wording (#299)**.

### maxiumpro — [@maxiumpro](https://github.com/maxiumpro)
Found on *Kung Fu Chaos*; see
[kungfuchaos-recomp](https://github.com/maxiumpro/kungfuchaos-recomp).

- **Thread priorities never took effect (#267)** — `Ke{Set,Query}BasePriorityThread`
  passed the guest object's address to Windows as a thread handle, so every
  call failed.
- **Kung Fu Chaos in the ports table (#271).**

### Raajik — [@Raajik](https://github.com/Raajik)
Found bringing up *SSX On Tour* on Linux.

- **`NtSetTimerEx` never armed the timer (#265)** — it returned success, so
  the game's 60 Hz frame clock never ticked, saves never finished and the
  intro stayed on its first frame. Synchronization timers now auto-reset, and
  the POSIX shim honours a timer's period.

### Silas Godoy — [@sigods](https://github.com/sigods)
- **Trapped MMIO on Linux x86-64 (#252)** — #191 decoded faults only on
  arm64, so a title there saw no USB controller and no APU. Found on *Otogi*.

### christianbemerson — [@christianbemerson](https://github.com/christianbemerson)
- **A guest `isatty` replaced libc's (#281)**, crashing Linux startup.
- **`RECOMP_FULLSCREEN` in the SDL presenter (#282).**

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

### jv36 — [@jv36](https://github.com/jv36)
- **Found that a guest function named `isnan` could not be compiled**, while
  bringing up *FIFA Street 2*. The C99 `<math.h>` classification names are
  function-like *macros*, so the name does not collide — it expands. `void
  isnan(void);` becomes `void (fpclassify(void) == FP_NAN);` and the compiler
  reports a bad parameter declarator inside a system header, naming neither
  the guest function nor the clash, which is why it cost a day rather than a
  minute. Reported with the fix already worked out and the sibling names
  enumerated: `isinf`, `isfinite`, `isnormal`, `signbit`, `fpclassify`. That
  list is what turned it from one patch into the right one — chasing it back
  showed the generation-time guard in `_func_ident` was the *weaker* of the
  project's two reserved-name lists, so every name the Ghidra-only merge
  filter would have caught was reaching the generated C by the IDA path.

### soldieg — [@soldieg](https://github.com/soldieg)
- **Traced *Black*'s black screen to its vertex program (#127).** With the same
  build, the software pushbuffer path rejected every triangle as behind the
  eye. `RECOMP_VSH_DUMP` showed the active program writing `oPos.w` from
  `c[191].w`, and the trace showed that constant as zero. That is the
  `oPos.w == 0` case #188 and #205 both went on to fix. The issue is also the
  most careful runtime-boundary report the tracker has. It kept every
  experiment that didn't hold, such as the event-sync and DSP-ack runs, out of
  the list of fixes.

### LukeWarm
- **Reported that the Linux build could not be found from the docs** — the
  README documents Linux and macOS, and `docs/GETTING_STARTED.md`, which is
  the guide people are actually pointed to, said "Windows 11" as a hard
  requirement and never mentioned either. Now carries a platform matrix that
  says what genuinely differs rather than implying one OS.

### ranzo312-cyber — [@ranzo312-cyber](https://github.com/ranzo312-cyber)
- **The *Darkwatch* bring-up report (#127).** It measured three runtime bugs
  and attached a patch for each:
  - guest callbacks that run inside a kernel call (APCs, inline DPCs,
    `KeSynchronizeExecution`) wrote over the caller's registers;
  - contiguous blocks were handed out inside the image's physical range, so
    XPP's descriptors resolved to the image and the pad never enumerated;
  - `lahf` emitted `_cf` undeclared.

  All three are fixed in v0.15.0 (#235 and its follow-up). The report kept
  its one unproven workaround clearly apart from the fixes.

---

## A note on AI-assisted contributions

Parts of this project — and some contributions to it — were developed with the
help of AI coding tools. That's welcome here: what matters is that every change
is understood, reviewed, and verified by a human before it lands. If you used
an assistant, just say so in your PR (several contributors have) and make sure
you can stand behind the result.
