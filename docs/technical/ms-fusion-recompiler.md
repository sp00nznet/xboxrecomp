# Microsoft's Own Xbox Static Recompiler (Ficl / Fission)

> Part of the [Documentation Index](../INDEX.md)

White-room analysis of a shipped Microsoft Xbox backward-compatibility package, done
to compare Microsoft's production static recompiler against this project's design.

**No Microsoft code, IR, or output was copied, reused, or lifted into this repository.**
Everything below was derived from publicly distributed retail binaries using standard
PE inspection (`pefile`) and disassembly (`capstone`). This document exists to record
*design decisions and their consequences*, and to identify which ones we can adopt.

Subject: the retail Windows BC package for **Crimson Skies: High Road to Revenge**
(an original Xbox title), version `2607.1523.1.0`, analysed 2026-07-23.

> Extended to all four released titles in [ms-fusion-corpus.md](ms-fusion-corpus.md) —
> build drift, shared modules, and the networking/engine fingerprints a single title
> cannot show.

---

## 1. It is a three-layer nesting doll

`MicrosoftGame.config` identifies as `Xbox360BackwardCompatibil...`, `LaunchArguments.txt`
contains `fusion`, and `SystemPartition/Compatibility/` holds `xefu.xex`. Crimson Skies is
an OG Xbox title, so the shipped stack is:

```
EmuMenu.exe                 WinUI/.NET shell (title picker, BC options)
└─ Emu.exe                  native host  (+ VGPUDX12.dll, dxcompiler.dll, D3D12Core.dll)
   └─ Fusion                Xbox 360 emulator
      ├─ xeo3_*.dll         Xbox 360 PowerPC, AOT-recompiled to x86-64
      │     xboxkrnlcf.bin, xam.xex, hud.xex, huduiskin.xex,
      │     ximecore.xex, XimeDic.xex, Xam.Community.xex, xefutitle.xex
      │     ...and xefu.xex — the OG Xbox emulator, itself a 360 title
      └─ xefu_*.dll         OG Xbox x86-32, AOT-recompiled to x86-64
            xb1krnl.exe      Microsoft's OG Xbox kernel reimplementation
            default.xbe      Crimson Skies itself
```

The source module for each DLL is recoverable from its `FileDescription` version-resource
string. Sizes:

| DLL | source module | guest code | host `.text` |
|---|---|---|---|
| `xefu_c954bd37_…` | `default.xbe` | ~2.6 MB | **21.3 MB** (8.2×) |
| `xefu_69c41281_…` | `xb1krnl.exe` | — | 853 KB |
| `xeo3_b36e2f30_…` | `xam.xex` | — | 10.3 MB Pri / 24.7 MB Fb |
| `xeo3_6a3039f4_…` | `xefu.xex` | — | 2.2 MB Pri / 7.2 MB Fb |
| `xeo3_5fb3687c_…` | `xboxkrnlcf.bin` | 474 KB | 1.1 MB Pri / 2.7 MB Fb |

The `_no.dll` suffix is the **`Fb` (fallback)** tier; the unsuffixed DLL is **`Pri` (primary)**.
Only the PowerPC layer ships both tiers — see §5.

Every module exports exactly four symbols:

```
InitPrecompiledDll        CleanupPrecompiledDll
PrecompiledPointers       PrecompiledSymbolTable  (x86 layer)
                       or PrecompiledImportTable  (PPC layer)
```

`InitPrecompiledDll` returns ABI version `0x2E` (46) in both layers — one shared runtime
contract across two guest architectures.

---

## 2. The build pipeline is embedded in every DLL

Each DLL's `Comments` version-resource string is a dump of the compiler's own argument
object. Verbatim, for `xefu.xex`:

```
CompilerExePath   D:\btsdx\20F914\xbox\emulator\Build\Tools\Ficl\x64\ficompiler.exe
AdditionalPhxArgs []                        <- Phx = Phoenix, MS's optimizing compiler backend
Controls          Common.ctrl.json  x64.ctrl.json  Release.ctrl.json
                  XenonXdk.ctrl.json  GlobalThunks.ctrl.json
                  [Fallback.ctrl.json]      <- present ONLY in the _no / Fb build
                  CallStacks.ctrl.json  xefu_common.ctrl.json  xefu.ctrl.json
LuaScriptFiles    XenonXdk.lua  ficlmcodenames.lua  xefu.lua
EnlightFile               ...\xefuPri\glopt\xefu.json
UpdateEnlightenments      true              <- compiler writes discovered facts BACK
EtlDigestPath             ...\xbox\emulator\Ppc\EtlDigests\
VirtualDispatchTraceFiles []                <- recorded indirect-branch targets
PdbFile                   ...\FusionSymbols\xefuc.pdb
CompilationInfoFile       ...\CompilationInfoPri.json   (vs ...Fb.json)
MaxLocalThreads 56   TotalNodeCount 56   WorkerIndex 0   ApplyGlobalNodeLimit true
CompileOnly []   CompileIntoSeparateModule []   CachePrecompilerOutputs true
```

Read off that list:

- The tool is **`ficompiler.exe`** ("Ficl"), the project is **Fission**, and codegen goes
  through **Phoenix** — a full production optimizing compiler backend, not a source emitter.
- Configuration is **layered JSON control files**: common + arch + flavour + SDK + per-title.
  A single extra control file (`Fallback.ctrl.json`) is the entire difference between the two
  output tiers.
- **Lua scripts** provide per-title hooks (`xefu.lua`) rather than special cases in the compiler.
- **`EnlightFile` + `UpdateEnlightenments: true`** — a persisted per-module analysis database
  that the compiler *updates on every build*. Static analysis reaches a fixpoint across
  successive builds instead of within one run.
- **`EtlDigestPath`** — digested ETW runtime traces are a codegen input.
- **`VirtualDispatchTraceFiles`** — recorded indirect-branch target sets. This is how they
  resolve virtual dispatch statically: they run the game and write down where it went.
- **56-node distributed build**, sharded by `WorkerIndex`, with output caching.

---

## 3. Generated code: x86-32 → x86-64

`_SetEvent@4` (guest `0x1c448a`, 32 guest bytes) → host `0xd51beb`:

```asm
xor    eax, eax
cmp    dword ptr [rsi + r15 + 0x18], ebp    ; guest esi IS host esi
jbe    .done
.loop:
mov    ecx, dword ptr [rsi + r15 + 0x20]
lea    r8d, [rcx + rax*4]                   ; 32-bit lea => exact guest wraparound
mov    dword ptr [r15 + r8], ebp
inc    eax
cmp    eax, dword ptr [rsi + r15 + 0x18]
mov    r10d, ecx                            ; save guest ecx across the poll
movzx  ecx, byte ptr [r12 - 0x3c]           ; preemption flag
jecxz  .nopoll
mov    dword ptr [rsp + 0x20], 0x1d449b     ; resume guest EIP
mov    r8d,  0x1d449d                       ; next guest EIP
mov    r9d,  0x1d                           ; callout kind
call   qword ptr [rip + ...]                ; runtime helper
mov    r10d, ecx
.nopoll:
mov    ecx, r10d                            ; restore guest ecx
jb     .loop
.done:
mov    ecx, esi
mov    dword ptr [r15 + 0x32dd8c], 1        ; guest global => [r15 + disp32]
mov    dword ptr [r14 + r15 - 4], 0x1d44ae  ; push the REAL guest return address
lea    r14d, [r14 - 4]                      ; guest esp -= 4  (lea preserves flags)
jmp    0x180d502b5                          ; direct jump into callee's translation
```

### Register model

| host | role |
|---|---|
| `eax ecx edx ebx esi edi ebp` | guest registers, **mapped 1:1 to the identically-named host registers** |
| `r14d` | guest `esp` |
| `r15` | guest RAM base |
| `r12`, `r13` | emulator state / per-thread flag blocks |
| `r8d`–`r11d` | scratch |
| flags | guest x86 flags **are** host x86 flags — free |

Consequences worth internalising:

1. **Guest memory access is one addressing mode**: `[greg32 + r15 + disp]`. No bounds check,
   no address translation, no helper. Native cost. Keeping guest registers 32-bit makes
   4 GiB wraparound exact for free.
2. **`lea` is used for every stack adjustment** specifically so it cannot clobber guest flags.
3. **Guest `call` = store the true guest return address to the guest stack, then `jmp`.**
   The host `call`/`ret` pair is never used for guest control flow; `rsp` stays available for
   the emulator's own scratch and helper arguments.
4. **Guest `ret` / indirect branch = `jmp qword ptr [r9 + r8*8]`**, where `r8` is the guest
   address. A flat, directly-indexed pointer array. No hash, no search, no compare, no miss path.

### The address map

`.rdata` holds **2,546,633 sorted `(guest_rva, host_rva)` pairs = 20 MB**, covering guest
`0x1000..0x27e466`. Delta histogram: 2,492,006 entries at delta 1, 47,377 at 2, 5,934 at 3,
1,163 at 4. That is **97.6 % of every byte** in the guest code span — a translation entry point
for essentially every guest address, not every guest instruction.

Addresses landing mid-instruction resolve to a **recovery stub** in a separate cold region,
which fixes up state and jumps into the optimized instruction stream. `InitPrecompiledDll`
hands the runtime the array base and count so it can build the flat `guest → host` pointer
table that the indirect-branch `jmp` indexes.

So: **function boundaries are never needed.** There is no function-identification problem,
no unresolved-callee problem, and no inter-unit ABI to get wrong.

### Timing

`InitPrecompiledDll` computes `(0x2BB5C755000000 + freq/2) / freq` from
`QueryPerformanceFrequency` — a 2^24 fixed-point QPC→guest-cycle ratio.
`0x2BB5C755000000 / 2^24 ≈ 733 MHz`, the OG Xbox clock.

### FPU exactness (PPC layer)

`InitPrecompiledDll` in the PPC modules executes `cpuid` and tests `bt ecx, 12` — CPUID.1:ECX
bit 12 is **FMA3**. PowerPC `fmadd` is a single-rounding fused multiply-add, so bit-exact
emulation needs hardware FMA; they detect it and branch to a slower path when it is absent.

---

## 4. The runtime helper surface is tiny

All escapes to the runtime go through slots in `PrecompiledPointers`. Only **~12 slots** are
referenced, and one of them (slot 2) is a generic dispatcher selected by a **kind code in `r9d`**.
Eighteen kind codes exist across the whole 21 MB game module.

Static site counts are **approximate and inflated** — because nearly every guest byte gets an
entry point, one guest instruction can emit its primary translation plus interior recovery
stubs, each re-emitting the sequence.

| kind | ≈sites | argument fingerprint | reading |
|---|---:|---|---|
| `0x1d` | 26.7k | preceded by `movzx ecx,[r12-0x3c]; jecxz`; resume EIP at `[rsp+0x20]`, next EIP in `r8d` | **preemption / interrupt / APC poll**, emitted at loop back-edges |
| `0x45` | 15.5k | `movzx ARG, dx` → `[rsp]`; width `1` or `4` at `[rsp+8]`; result read back via `mov al,[rsp]` | **port I/O read** (`in`), port in `DX` |
| `0x47` | 7.3k | same as `0x45` plus a data value at `[rsp+0x10]`; no result read | **port I/O write** (`out`) |
| `0x1b` / `0x1c` | 14.4k / 14.4k | `movzx ARG, word ptr [r14+r15]` — a 16-bit word popped from the guest stack; the two are always emitted **as a pair** | 16-bit-word-from-stack operation; candidates are `fldcw`/`fnstcw` or a far-return / segment load |
| `0x12`, `0x16`, `0x13` | 8.2k, 3.0k, 2.8k | no args; each followed by `lea r14d,[r14+4]` (guest pop) | services consuming one 32-bit guest stack slot — segment/descriptor or system instructions |
| `0x25` | 2.4k | `mov [rsp], imm` where imm ∈ {`0xE8`, `0xFF`, `0x89`, `0x0F`, `0x8B`, `0x5D`, …} | **these are x86 opcode bytes** — the generic "interpret this instruction" fallback |
| `0x43` | 1.1k | `mov ARG,[r14+r15]` → `[rsp+0x18]` | stack-consuming service |
| `0x1e` | 4.2k | resume EIP at `[rsp+0x20]`, like `0x1d` | second poll/exception variant |
| `0x1f` | 3.5k | mixed; one site continues into `fadd` | x87-related |
| `0x26`, `0x14`, `0x15`, `0x38`, `0x4f`, `0xfe` | 5.1k, 230, 160, 1, 1, 1 | mixed / singletons | — |

The important result is the shape, not the individual codes: **the escape hatch is almost
entirely hardware access and preemption, plus one generic opcode interpreter.** It is not a
long tail of "instructions we could not translate."

---

## 5. Two tiers: `Pri` vs `Fb`

`xboxkrnlcf.bin` is the cleanest pair to compare, since both tiers are small.

| | `Pri` | `Fb` (`_no.dll`) |
|---|---|---|
| map entries | **11,118** | **119,935** |
| guest span | 474,024 B | 488,520 B |
| guest delta histogram | 8 (×2130), 16 (×1111), 12 (×853), 24, 20, 28 | **4 (×118,373)**, 8 (×1337), 12 (×206) |
| entry granularity | branch targets only, ~1 per 42 guest bytes (≈10.6 PPC instructions) | **every single PPC instruction** |
| `.text` | 1,115 KB | 2,737 KB (**2.45×**) |

The same guest address, in both tiers:

```asm
;=== Pri:  guest 0x00011108 -> host 0x1299
mov    qword ptr [rbx], 0x69            ; guest instruction counter / block id
mov    ecx, 0x80051110                  ; 32-bit immediate
mov    qword ptr [rbx - 0x20], rcx      ; guest link register into context
call   0x180001000                      ; DIRECT relative call
mov    ecx, 0x80051114
mov    qword ptr [rbx - 0x20], rcx
call   0x180067785                      ; DIRECT
mov    qword ptr [rbx], 0x6a            ; counter += 1
...

;=== Fb:   guest 0x00011108 -> host 0x6936
mov    qword ptr [rbx], 0x69
jmp    0x180006942                      ; <-- degenerate jmp to the next instruction
movabs rcx, 0x80051110                  ; 64-bit immediate, no folding
mov    qword ptr [rbx - 0x20], rcx
mov    ecx, 0x80051000
mov    r13d, ecx
mov    r12, qword ptr [rbx - 0x10]      ; reload dispatch table base from context
call   qword ptr [r12 + r13*2]          ; INDIRECT through the table
jmp    0x18000696f
jmp    0x18000696f                      ; <-- one terminator per guest instruction
movabs rcx, 0x80051114
...
```

`Pri` fuses guest instructions and resolves calls at build time. `Fb` emits **one
independently-addressable host block per guest instruction**, never folds an immediate, and
routes every call indirectly. The degenerate `jmp`-to-next-instruction sequences are the
signature: no cross-instruction optimization happened at all.

`[r12 + r13*2]` is a nice detail — guest PPC addresses are 4-byte aligned and pointers are
8 bytes, so scale-2 indexing gives the right stride with no shift.

**What the two tiers are for:** `Pri` is fast but can only be entered at addresses the compiler
proved were branch targets. When the runtime must enter at an arbitrary guest address —
exception, debugger, an indirect target the traces never recorded, self-modifying code — it
uses `Fb`, which can start at any instruction and keeps guest state precise at every boundary.

**And note the x86 layer solves the same problem differently.** It ships `Pri` *only*, but its
map is byte-dense (97.6 %) with cold recovery stubs for mid-instruction addresses. Two
architectures, two answers:

- **fixed-width guest (PPC)** → sparse optimized tier + dense unoptimized tier
- **variable-width guest (x86)** → dense map + recovery stubs, single optimized tier

The x86 approach is strictly better where it applies, and x86 is *our* guest architecture.

---

## 6. Their HLE boundary is far lower than ours

`PrecompiledSymbolTable` in the game module is a header plus 3,106 twelve-byte records
`(guest_start, byte_size, name_offset)` over a 128 KB string blob:

```
strings @027c4fe0  size 0x1faeb   version 1   records 3106
```

**2,997 unique names covering 601,843 guest bytes — 23 % of the guest code span.** A sample:

```
001c440b  +30    _CloseHandle@4                001caa45 +1060  _RtlCreateHeap@24
001c4824  +152   _XapiThreadStartup@8          001cae69 +1915  _RtlAllocateHeap@12
001c4b59  +522   _RtlpCutoverTimeToSystemTime@16
001ffcc0  +599   _D3DDevice_DrawIndexedVertices@12
002066b0  +164   _Direct3D_CreateDevice@24
0023519b  +68    _XGIsSwizzledFormat@4
00234bb9  +256   ?swiz2d_8bit@XGRAPHICS@@YGXPBXPAXHH@Z
002370f3  +47    ?DownloadEffectsImage@CMcpxAPU@DirectSound@@QAEJPBXKPAPAU_DSEFFECTIMAGEDESC@@@Z
00228be8  +474   ?decompress_onepass@D3DX@@YAHPAUjpeg_decompress_struct@1@PAPAPAE@Z
```

Group counts: 85 `D3DDevice_*`, 4 `Direct3D_*`, 384 `DirectSound`/`CMcpx*`, 161 `D3DX`,
253 `CXo` (Xbox Live), 142 `CXnIp`, 82 `CXnSock`, 69 `png`, 38 `XGRAPHICS`/`XG*`,
48 `CMiniport`. The only kernel-shaped names are the six `Rtl*Heap` functions — because those
were statically linked into the XBE too.

**All of these are recompiled verbatim. None are replaced.** Microsoft's HLE line sits at the
`xboxkrnl` import boundary and nowhere else; graphics is emulated at the *hardware register*
level, three layers deep:

```
game's recompiled D3D8  ->  NV2A pushbuffer
  ->  xefu.xex (recompiled 360 code) rewrites NV2A -> Xenos
    ->  VGPUDX12.dll: XenosDecoder / Dx12XenosDecoder / DX12EdramManager  ->  D3D12
```

`VGPUDX12.dll` confirms the bottom layer: `XenosDecoder`, `Dx12XenosDecoder`,
`DX12EdramManager::ComputeUpscalingMap`, `ComputeSkipMirrorMap`, `ShaderCompilerCache`,
`LoadShadersFromPath`, `DeleteVersionMismatchedShadersFromPath`, `CollectOverrides`,
`FMT_2_10_10_10_FLOAT_EDRAM`. Shipped alongside: `dxcompiler.dll` (18 MB DXC) for runtime
HLSL→DXIL, `DX12EdramResolveShaders.sbin`, `DX12DirectResolveShaders.sbin`, and a persistent
`XeO3_ShaderCache/{titleId}/V0.1.2_JIT_TVP70003_0.pak` keyed by version string.

The BC "enhancements" ride on the EDRAM manager, driven by `LaunchArguments.txt`:
`aaBoostOn`, `aaBoostTargetMsaa=1`, `scalingResolutions=1280x0 0x480 0x240 0x360`,
`disableAudioOnConstrained`.

---

## 7. Side-by-side with this project

| | Microsoft (Ficl / Fission) | xboxrecomp |
|---|---|---|
| Output | x86-64 machine code via Phoenix | **C source**, one function per guest function |
| Guest registers | host `eax…ebp` 1:1, `r14d`=esp, `r15`=base | **TLS globals** `g_eax…g_edi`; `ebp` local |
| Flags | inherited free | synthesized in C |
| Guest memory | `[greg32 + r15 + disp]` | mapped at original VA, pointer casts |
| Granularity | **byte-level** (x86), 97.6 % of code span | **function-level** |
| Indirect branch | `jmp [table + guest_eip*8]` | `recomp_lookup_manual` → `recomp_lookup` → `recomp_lookup_kernel`, 3-tier fallback per call |
| Untranslated target | recovery stub exists for ~every address | `*_stubs_unresolved.c`, `g_esp += 4` and continue |
| Guest `call` | pushes the **real** guest return address | `PUSH32(esp, 0)` — a **dummy** |
| Function boundaries | **not needed** | `tools/disasm/functions.py`, 560 lines + tests |
| Cold/unprovable code | `Fb` tier, precise at every instruction | unresolved stub |
| HLE line | `xboxkrnl` only; GPU at register level | D3D8 + DirectSound + XAPI in C (~8 kLOC) |
| Feedback loop | enlightenments DB + ETW + dispatch traces, iterated | none |
| Per-title fixups | Lua scripts | hardcoded |
| Build | 56 nodes, sharded, cached | single machine |

### What we should adopt

1. **Flat, directly-indexed dispatch, and an entry point per basic block.**
   `RECOMP_ICALL` currently performs up to three lookups per indirect call, and any target we
   did not classify as a function becomes an unresolved stub that silently corrupts guest state.
   Replace `recomp_lookup*` with one flat `recomp_func_t` array indexed by `(va - code_base)`,
   and emit an entry label at every basic-block head rather than only at function heads.
   A 4 MB code span costs 32 MB of table; Microsoft spends 20 MB without concern.
   This single change attacks the indirect-call problem, the unresolved-stub problem, and our
   dependence on function-boundary detection simultaneously.
   See [indirect-calls.md](indirect-calls.md).

2. **Push the real guest return address.** `PUSH32(esp, 0)` is a latent correctness bug for
   anything that reads its own return address off the stack: `__SEH_prolog`, `_alloca` probes,
   return-address-based caller identification, and any `mov eax, [esp]` idiom. Microsoft pushes
   the true guest EIP because the guest stack must be bit-accurate. Cheap fix, removes a class.
   See [seh-handling.md](seh-handling.md).

3. **Record indirect targets at runtime and feed them back into the next build.**
   This is exactly `VirtualDispatchTraceFiles` + `UpdateEnlightenments: true`: log every
   resolved indirect target to JSON, merge it into the next codegen run, iterate to a fixpoint.
   Directly replaces the guessing in `tools/recomp/analyze_unresolved.py`.

4. **Cache TLS registers into locals per function.** Every `g_eax` touch is a TLS indirection.
   Load the live set at entry; write back at exit and before any call or callout. This will not
   reach 1:1 register mapping, but it is the reachable part of the gap.
   See [register-model.md](register-model.md).

5. **A real interpreter fallback instead of unresolved stubs.** `Fb` is the honest answer to
   "we could not prove this statically." A stub that adjusts `esp` and returns is not.

6. **A per-title script/config file** rather than special-casing titles inside the lifter.

### What we cannot have

- **Phoenix.** No equivalent — but we hand our C to MSVC/clang, which is a fair trade. This is
  not the real gap.
- **1:1 register mapping and free flags.** Inherent to emitting machine code. Remaining in C
  implies a multiple-× slowdown against their approach. Worth knowing as our ceiling; not worth
  abandoning the C backend over, since that backend is why this project is debuggable.
- **Their 2007 assets.** `xb1krnl.exe`, the NV2A→Xenos translator inside `xefu.xex`, the original
  PDBs, the XDK sources. They can recompile the game's own D3D8 verbatim *because* they already
  own a hardware-accurate NV2A consumer. We do not, so our D3D8 HLE is forced, not a shortcut.
  See [d3d-translation.md](d3d-translation.md), [kernel-replacement.md](kernel-replacement.md).
- **A 56-node build farm.**
- **Nested emulation.** Not available and not desirable.

### The framing that matters

Every callee-save / prologue / frame-pointer bug in our lifter is a tax on the decision to emit
C functions. In Microsoft's model there is no inter-unit ABI to get wrong: guest registers live
in host registers, and control flow is a jump through an address table. Adoptions 1 and 2 above
are the cheapest way to buy back most of that tax without changing backends.

---

## Appendix: reproducing this

Package layout, for reference:

```
Content/
  Emu.exe                     ACL-locked by store package protection; not readable
  VGPUDX12.dll                Xenos -> D3D12
  dxcompiler.dll  D3D12Core.dll
  DX12EdramResolveShaders.sbin  DX12DirectResolveShaders.sbin
  LaunchArguments.txt  MicrosoftGame.config
  xefu_*.dll  xeo3_*.dll      the recompiled modules
  EmuMenu/                    WinUI/.NET shell
  Flash/                      360 system images: xam.xex, xboxkrnlcf.bin, hud.xex, ...
  SystemPartition/Compatibility/  xefu.xex, xefutitle.xex
  XeO3_ShaderCache/{titleId}/ V0.1.2_JIT_TVP70003_0.pak
  Game/DefaultPackage.data/   Data0000..Data0025  (packed title content)
```

Everything in this document came from:

- version resources (`Comments`, `FileDescription`) via `pefile` — §1, §2
- export tables and `InitPrecompiledDll` disassembly via `capstone` — §1, §3, §5
- the `(guest, host)` pair array in `.rdata`, bounds read out of `InitPrecompiledDll` — §3, §5
- `PrecompiledSymbolTable` records + string blob — §6
- byte-pattern scan for `41 B9 <kind> FF 15 <disp>` against the `PrecompiledPointers`
  window, then windowed disassembly around each site — §4
- ASCII string extraction from `VGPUDX12.dll` — §6

The map bounds are not exported; read them from the two `lea` instructions in
`InitPrecompiledDll`. For `xefu_c954bd37_…` the array is `0x1457190 .. 0x27C4FD8`;
for `xeo3_5fb3687c_…` `Pri` it is `0x11D6F0 .. 0x133260` and `Fb` `0x2B39D8 .. 0x39DDD0`.
