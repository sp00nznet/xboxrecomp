# Ficl/Fission Codegen Teardown — and What It Means For Us

> Part of the [Documentation Index](../INDEX.md)

Companion to [ms-fusion-recompiler.md](ms-fusion-recompiler.md), which covers the shipped
package layout, the build pipeline, and the HLE boundary. This document is the **codegen
teardown**: what the two translators actually emit, established with IDA Professional 9.1
(`idalib`, headless) and Hex-Rays on top of the earlier `pefile`/`capstone` work.

**No Microsoft code, IR, or output is copied or reused in this repository.** This is a
design-level teardown of publicly distributed retail binaries.

The headline is not a list of tricks. It is this:

> **Microsoft ships two completely different recompiler architectures in one product, and the
> one they chose for a PowerPC guest is the one we chose for an x86 guest.**

Everything below is the evidence, then what follows from it.

---

## 1. The measurement that reframes everything

Same toolchain (`ficompiler.exe`), same runtime ABI (`InitPrecompiledDll` returns version
`0x2E` in both), same four exported symbols. Completely different output shape:

| | `xeo3_*` (PowerPC → x64) | `xefu_*` (x86-32 → x64) |
|---|---|---|
| IDA functions found | **2,246** | **66** |
| `.text` | 1,142,784 B | 856,064 B |
| bytes inside functions | 1,127,639 (98.67 %) | 853,059 (99.65 %) |
| largest single function | 25,561 B | **832,546 B** |
| size distribution | log-normal, mode 256–511 B | one giant blob + CRT scaffolding |
| `.pdata` RUNTIME_FUNCTIONs | **1,600** | **46** |

`xefu_69c41281` (`xb1krnl.exe`) is **one 832,546-byte host function** plus 65 pieces of CRT
scaffolding (`__report_gsfailure`, `capture_previous_context`, `InitPrecompiledDll`, …).
The 41 MB game module is the same shape at scale — its `.pdata` is 76 entries whose largest
five ranges are **16,387,345**, 1,192,616, 940,880, 639,689 and 603,236 bytes. A full IDA
pass over that module independently agrees: **105 functions** for 21 MB of `.text` (against
2,246 for the PPC `xboxkrnlcf` at a fifth the size), the flat-arena signature at scale.

Note the correct reading of that `.pdata`: unwind info covers **99.996 %** of `.text`, but as
~76 enormous ranges. It is not "no unwind info" — it is "the whole translated arena is a
handful of gigantic host functions sharing one stack frame." IDA's 832,546-byte function
matches `xb1krnl`'s largest `.pdata` range byte for byte, so RUNTIME_FUNCTION == host function
and the correspondence is exact.

So:

- **PowerPC guest → normal host functions.** Real `call`/`ret`, real frames, real unwind info,
  one host function per guest function.
- **x86 guest → one flat code arena.** No per-guest-function frames, no host `call`/`ret` for
  guest control flow, entry only by jumping to a guest-address-derived location.

---

## 2. The PowerPC translator, from Hex-Rays

A 264-byte function, decompiled (annotations added):

```c
__int64 __fastcall sub_180001AFC()
{
  _RCX = *(_QWORD *)(_RBX - 32);            // rbx = GUEST CONTEXT BLOCK; [rbx-32] = LR
  *(_QWORD *)(_RBX + 88) = _RCX;            // [rbx+88] = some guest GPR
  _RDX = *(unsigned int *)(_RBX + 8);       // [rbx+8]  = guest SP (r1)
  __asm { movbe [rdx+r15-8], ecx }          // r15 = GUEST MEM BASE; movbe = byte-swap store
  __asm { movbe rcx, [rbx+70h] }
  *(_QWORD *)(_RDX + _R15 - 24) = _RCX;
  ...
  v6 = *(_QWORD *)(_RBX + 8) - 112LL;       // stwu r1, -112(r1)
  *(_QWORD *)(_RBX + 8) = v6;
  *(_QWORD *)(_RBX - 32) = 2147816540LL;    // LR = 0x8005111C, a GUEST address
  result = sub_18003B951();                 // DIRECT host call

  v9  = *(_DWORD *)_RBX == 0;               // [rbx] = guest r3
  v10 = *(int *)_RBX < 0;
  *(_BYTE *)(_RBX - 64) = v10;              // CR0.LT  as its own byte
  *(_BYTE *)(_RBX - 62) = v9;               // CR0.EQ
  *(_BYTE *)(_RBX - 63) = !v10 && !v9;      // CR0.GT
  *(_BYTE *)(_RBX - 61) = *(_BYTE *)(_RBX + 20);  // CR0.SO, copied from XER
  if ( !v10 )
  {
    ...
    result = (*(__int64 (**)(void))(*(_QWORD *)(_RBX - 16) + 2 * _RCX))();
    //        ^ [rbx-16] = dispatch table base, indexed by guest address * 2
  }
  ...
}
```

Four things to take from this:

1. **Guest GPRs live in memory**, in a context block addressed off `rbx`, at both positive and
   negative offsets. PowerPC has 32 GPRs; x86-64 does not have enough registers to map them, so
   there is no attempt to.
2. **`movbe` on every single guest memory access.** PowerPC is big-endian; `movbe` byte-swaps in
   the load/store itself, at zero extra cost. This is the whole endianness story — there is no
   swap helper, no shadow representation.
3. **PowerPC CR0 is decomposed into four separate bytes** (`LT`, `GT`, `EQ`, `SO`) at
   `[rbx-64 .. -61]`. They do not model a condition register; they materialise the boolean
   outcomes. A later branch-on-condition is then a plain byte test. This happens in the
   **optimized** tier, not just the fallback.
4. **The guest link register is written with the real guest return address** (`0x8005111C`)
   before every call — the same discipline as the x86 path pushing the real return address onto
   the guest stack. The value is never used for control transfer (the host `call` does that);
   it is written because guest code reads it.

`[rbx+8]` = guest SP, `[rbx]` = guest r3, `[rbx-16]` = dispatch table base, `[rbx-32]` = LR,
`[rbx+20]` = XER, `[rbx-64..-61]` = CR0 bits. Scale-2 indexing on the dispatch table
(`base + 2*guest_addr`) works because PowerPC addresses are 4-byte aligned and pointers are
8 bytes — the shift is free.

---

## 3. The x86 translator, for contrast

From [ms-fusion-recompiler.md §3](ms-fusion-recompiler.md), restated as the same four points:

1. **Guest registers are host registers, 1:1.** `eax`→`eax`, `esi`→`esi`, … with `r14d` = guest
   `esp`, `r15` = guest memory base, `r12`/`r13` = state blocks, `r8d`–`r11d` scratch.
2. **No endianness work at all.** Same-endian guest.
3. **Guest flags are host flags**, free. `lea` is used for every stack adjustment specifically so
   it cannot clobber them.
4. **The guest return address is pushed onto the guest stack**, and `ret` becomes
   `jmp qword ptr [table + guest_eip*8]`.

Plus the structural consequence: with no host `call`/`ret` for guest control flow, there are no
per-guest-function frames, hence 46–76 RUNTIME_FUNCTIONs for megabytes of code.

### Side by side

| | PPC → x64 | x86-32 → x64 |
|---|---|---|
| guest GPRs | **memory**, context block at `rbx` | **host registers**, 1:1 |
| guest SP | `[rbx+8]` | `r14d` |
| guest return address | `[rbx-32]` (LR) | pushed on the guest stack |
| condition flags | CR0 → 4 separate bytes | host x86 flags, free |
| endianness | `movbe` on every access | nothing needed |
| guest memory base | `r15` | `r15` |
| host control flow | real `call`/`ret`, 2,246 functions | `jmp` only, ~1–5 giant functions |
| entry granularity | `Pri` branch targets / `Fb` every instruction | ~every byte + recovery stubs |
| output tiers | **two** (`Pri` + `Fb`) | **one** |

The two-tier split exists only where entry granularity is cheap to vary. For a fixed-width
guest you can build a sparse optimized tier and a dense unoptimized one. For a variable-width
guest you cannot cleanly separate them, so they built a single tier that is dense *and*
optimized, with cold recovery stubs bridging mid-instruction addresses.

---

## 4. Where our design actually sits

| | MS PPC path | MS x86 path | **xboxrecomp** |
|---|---|---|---|
| guest arch | PowerPC (fixed-width, big-endian, 32 GPRs) | x86-32 | **x86-32** |
| guest GPRs | memory context block | host registers | **memory (TLS globals)** |
| flags | materialised booleans | host flags, free | **synthesized in C** |
| host control flow | real functions, `call`/`ret` | flat arena, `jmp` | **real C functions, `call`/`ret`** |
| entry granularity | branch targets / every instruction | ~every byte | **function starts only** |
| tiers | two | one | one |

**We built the PowerPC-shaped translator for an x86 guest.** Every structural choice we made
matches the column where Microsoft was *forced* into it by the guest architecture, not the
column that corresponds to our actual guest.

That is not a condemnation — it is the direct consequence of emitting C, and emitting C is why
this project is debuggable, portable, and hackable by one person. But it explains the bug
classes we keep hitting, and it says exactly which of them are inherent and which are not.

### What is inherent to emitting C

- **Guest registers in memory.** C has no way to pin `eax` to `eax`. Our TLS globals are the
  same technique Microsoft uses for PPC, where it is unavoidable. For an x86 guest it costs us
  the single biggest performance factor, and there is no fix short of a native backend.
- **Flag synthesis.** Same reason. We cannot inherit host flags from C.

### What is *not* inherent — and is where the wins are

- **Function-granular entry points.** Nothing about C requires this. It is the root of
  `tools/disasm/functions.py` (560 lines + tests), the unresolved-stub problem, and every
  indirect-call miss.
- **Real host `call`/`ret` for guest calls.** Also not required. Microsoft's PPC path does it and
  it is fine *because they have accurate function boundaries from PDBs and traces.* We do not,
  so we inherit the failure mode without the mitigation.
- **A single tier.** Nothing stops us emitting a precise variant for the functions that need one.
- **No feedback loop.** Nothing stops us recording indirect targets at runtime and feeding them
  back, which is exactly `VirtualDispatchTraceFiles` + `UpdateEnlightenments`.

---

## 5. Reframed next steps

The earlier doc listed six adoptions ranked by cost. With the teardown in hand the ranking
changes, because two of them turn out to be the same change and one turns out to be nearly free.

### Already done

- **Push the real guest return address.** Both Microsoft translators write the true guest return
  address even though neither uses it for control transfer, because guest code reads it.
  Done in `7fbcc79`; `tools/recomp/test_call_retaddr.py` guards it.

### Tier 1 — the one architectural change worth making

**Make the translation unit a basic block, not a function, and dispatch every inter-block edge
through one flat table indexed by guest VA.**

This is the C-compatible form of Microsoft's x86 architecture. Each guest basic block becomes a
small C function; a flat `recomp_func_t` array indexed by `(va - code_base)` replaces the
three-tier `recomp_lookup_manual` → `recomp_lookup` → `recomp_lookup_kernel` chain; inter-block
transfers go through it. `Fb`'s 119,935 entries for 488 KB of guest code — one per guest
instruction — is the proof that this density is affordable. A 4 MB guest code span costs ~32 MB
of table; they spend 20 MB in the game module without comment.

What it buys, all at once:

- indirect calls resolve or provably do not, with no fallback chain and no silent stub
- computed jumps, jump tables, and mid-function targets all work
- function-boundary detection stops being load-bearing — `functions.py` becomes a naming and
  grouping aid rather than a correctness dependency
- the callee-save / prologue / frame-pointer bug class shrinks, because there is far less
  inter-unit ABI to get wrong

What it costs: a C call per basic block instead of per guest function. At ~10–20 guest
instructions per block that is real but bounded overhead, and it should be measured rather than
guessed. Trampoline the dispatch (return to a driver loop) rather than nesting calls, or the
host stack grows without bound on long call chains.

This subsumes both item 1 and item 5 from the earlier list. It is a significant change and
should go behind a flag with A/B output so it can be compared against the current translator on
a title that currently boots.

### Tier 2 — cheap, independent, do regardless

- **Materialise flag outcomes as booleans instead of recomputing conditions.**
  Microsoft decomposes CR0 into four bytes in the *optimized* tier. Our lifter already threads
  `flag_state` through basic blocks; the lesson is that storing a small fixed set of boolean
  outcomes is cheap enough that a production optimizing compiler does it by choice.
- ~~**Record indirect-branch targets at runtime, merge into the next build.**~~ **Done.**
  `RECOMP_ICALL_FEEDBACK` + `src/kernel/icall_feedback.c` +
  `tools/recomp/icall_feedback.py`, feeding the existing `tools/disasm
  --seed-functions` input. Cumulative across runs, so it converges instead of
  reporting only the last run. See
  [indirect-calls.md](indirect-calls.md#target-feedback-measure-instead-of-guess).
  This also produces the data needed to *evaluate* the Tier 1 change: once the
  observed target set stops growing, the residual `unresolved` count is the real
  size of the problem that block-level dispatch would eliminate.
- **Cache TLS registers into locals per function**, writing back at exits and before calls. The
  reachable part of the register-model gap.
- **Per-title script/config file** instead of special cases in the lifter (`xefu.lua`).

### Explicitly not worth pursuing

- **A native x64 backend.** It is the only way to get 1:1 registers and free flags, and it would
  cost the debuggability that makes this project tractable. Know it as the ceiling; do not chase it.
- **`movbe` / endianness machinery.** Same-endian guest. Free win we already have.
- **`r15`-relative guest addressing.** We map guest memory at its original VA and use pointer
  casts, which is strictly better than a base register. Already optimal.
- **Matching their HLE boundary.** They recompile the game's own D3D8/D3DX/DirectSound/XAPI
  because they own a hardware-accurate NV2A consumer from 2007. We do not. Our D3D8 HLE is
  forced, not a shortcut. See [d3d-translation.md](d3d-translation.md).

---

## 6. The GPU layer, for reference

`VGPUDX12.dll` is hand-written C++ (11,358 functions, 1 RUNTIME_FUNCTION per 354 bytes — normal
code), not recompiler output, so it is a reference for *our* `src/d3d` rather than for the
lifter. Class names recovered from RTTI:

```
Xe2mVirtualGPUDevice          XenosDecoder / Dx12XenosDecoder
DX12EdramManager              DX12SurfaceManager::HostUpscaledMemory
DX12CommandListRunner         DX12GuestPipelineState
DX12PSOPreloader<AsyncCachedPSO>   DX12PSOPreloader<EdramAsyncPSO>
DX12TextureTranslator         ShaderCompilerCache / HostShaderComplex
SbinManager                   MemoryFileReader / PendingIOData
IVGPUBuffer / IVGPUTexture2D  (WRL RuntimeClass, FtmBase)
```

Two ideas there are worth stealing independently of the recompiler work:

- **`DX12PSOPreloader<AsyncCachedPSO>`** — pipeline states are precompiled asynchronously from
  the persisted shader cache, so first-encounter draws do not hitch. Our shader translation
  compiles on demand.
- **`OverrideGuestTextureFormats` / `GetOverrideMap` / `CollectOverrides`** — a data-driven
  per-title override table for guest texture formats, keyed by title. Cleaner than the
  per-title special-casing we accumulate in C.

Also visible: `ComputeUpscalingMap`, `ComputeSkipMirrorMap`, `ComputeMipResolvePitches`,
`ScalingResolution`, `DualResolveInfo` — the EDRAM manager is where BC resolution and MSAA
enhancement live, driven by `LaunchArguments.txt` (`aaBoostOn`, `aaBoostTargetMsaa=1`,
`scalingResolutions=...`).

---

## Appendix: how to reproduce

IDA Professional 9.1 headless via `idalib`, Python 3.11 (`import idapro` must be first):

```
py -3.11 deep.py <module> struct        # function count, coverage, size histogram
py -3.11 deep.py <module> pick 260:400  # decompile functions in a size range
py -3.11 deep.py <module> rtti          # RTTI-shaped symbols
```

Copy the DLL out of the package first — `idalib` writes its database next to the input.

Unwind-info density is faster to get from `pefile` directly: `.pdata` size / 12 is the
RUNTIME_FUNCTION count, and each entry is `(start_rva, end_rva, unwind_rva)`. That alone
distinguishes the two architectures without loading IDA.

Modules analysed:

| module | source | note |
|---|---|---|
| `xefu_69c41281_00027bcf.dll` | `xb1krnl.exe` | smallest x86 module; the 832 KB single function |
| `xefu_c954bd37_…dll` | `default.xbe` | 41 MB; 76 RUNTIME_FUNCTIONs, largest 16.4 MB |
| `xeo3_5fb3687c_001748c4.dll` | `xboxkrnlcf.bin` | PPC `Pri`; 2,246 functions |
| `xeo3_5fb3687c_001748c4_no.dll` | `xboxkrnlcf.bin` | PPC `Fb`; same 1,600 unwind entries, 2.45× code |
| `VGPUDX12.dll` | — | hand-written C++; GPU reference |
