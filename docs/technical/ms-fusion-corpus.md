# Ficl/Fission Across Four Titles — a Corpus

White-room analysis of **all four** Microsoft Xbox backward-compatibility packages released
to date, extending the single-title teardown in [ms-fusion-recompiler.md](ms-fusion-recompiler.md)
into a corpus. The point of a corpus is to separate *what the recompiler does* (constant across
titles) from *what a game is* (the per-title differences), and to find things a single title
cannot show — build drift, shared modules, and the networking/engine fingerprints.

**No Microsoft code, IR, or output was copied, reused, or lifted into this repository.**
Everything below is derived from publicly distributed retail binaries using standard PE
inspection (`pefile`) and disassembly (`capstone`): version resources, the four exported
symbols, the `PrecompiledSymbolTable`, and the `.rdata` address map. Subjects analysed
2026-07-23, package version `2607.x`.

The four titles are the only ones Microsoft has shipped through this pipeline as of writing.

---

## 1. The four modules

Each package's OG-Xbox game module is `xefu_<hash>…dll`; the source is recoverable from its
`FileDescription` version string. All figures are for that game module.

| | **Fuzion Frenzy** | **Blinx** | **Crimson Skies** | **Conker L&R** |
|---|---|---|---|---|
| Build tree (`btsdx`) | `20F919` | `20F914` | `20F90F` | `20F917` |
| Build-path id | `4D530856` | `4D530013` | `4D530851` | `4D530051` |
| Module version | 1.0.0.42 | 1.0.0.42 | 1.0.0.42 | 1.0.0.42 |
| Exports | Init/Cleanup/Pointers/SymbolTable | — | — | — (identical 4) |
| Symbol records | 1,482 | 1,784 | 3,106 | 3,228 |
| Named guest bytes | 278 KB | 357 KB | 602 KB | 676 KB |
| String blob | 0xD38B | 0x11B59 | 0x1FAEB | 0x1F7B9 |
| Map coverage | 97.9% | 97.7% | 97.6% | 98.0% |
| Map delta shape | `1»2»3»4` | `1»2»3»4` | `1»2»3»4` | `1»2»3»4` |
| Networking (Live) | **none** | **none** | 485 fns | 615 fns |

Symbol-table figures are exact (parsed directly). Map coverage is robust; the absolute entry
count is a per-run lower bound from a longest-contiguous-run scan — the exact count is read from
`InitPrecompiledDll`'s bounds (2,546,633 for Crimson, per the single-title report), which
validates the ~97.6–98.0 % density measured here across all four.

**Every structural property is identical.** Same 4-export ABI, same byte-dense single optimized
tier, same address-map delta histogram (overwhelmingly delta 1 — a translation entry point at
essentially every guest byte). The recompiler is uniform across the catalog; nothing is
special-cased per title. Confirmed on four titles, not extrapolated from one.

---

## 2. Build drift — visible only across the corpus

Every title carries a **distinct** `btsdx` build number, and they increase monotonically in the
order the games were compiled: `20F90F` (Crimson) < `20F914` (Blinx) < `20F917` (Conker) <
`20F919` (Fuzion Frenzy). The Fission toolchain was versioned as the catalog was processed; each
title got whatever build was current. A single teardown cannot see this — you need the corpus.
(The game module's own version resource is the authority here: the earlier single-title report
read `20F914` from the *emulator-layer* `xefu.xex` path, which is a different module than the
game's `xefu_c954bd37`.)

It shows up in the output. The recompiled OG-Xbox kernel module `xefu_69c41281_00027bcf.dll`
(source `xb1krnl.exe`) is **byte-identical** across Crimson, Blinx, and Fuzion Frenzy
(md5 `4c0c3550…`) but **Conker ships a distinct build of it** (md5 `f2b36346…`). Same source
module — the filename hash `69c41281` is the source identity and is shared by all four — but
Conker's `20F917` compiler produced different x86-64 from the same input. So the recompiled
output is deterministic per compiler build and shared when identical; it is not identical across
builds. (The recompiled kernel is stable across builds `20F90F`/`20F914`/`20F919` and changes
only in `20F917`, so the codegen is not strictly monotonic in the build number.)

---

## 3. Networking is a clean binary fingerprint

The `PrecompiledSymbolTable` names the game's own statically-linked XDK library functions.
The Xbox Live / socket classes (`CXo*`, `CXnIp`, `CXnSock`) are present or absent exactly as
the game's multiplayer capability dictates:

| | CXo | CXnIp | CXnSock | total | game |
|---|---:|---:|---:|---:|---|
| Crimson Skies | 261 | 142 | 82 | **485** | Xbox Live co-op/vs |
| Conker L&R | 385 | 149 | 81 | **615** | Xbox Live multiplayer |
| Blinx | 0 | 0 | 0 | **0** | single-player |
| Fuzion Frenzy | 0 | 0 | 0 | **0** | local (couch) multiplayer |

This is directly useful to *this* project as a triage signal. A title with zero `CXnIp` cannot
hit the XNet bring-up problems Halo did — there is no network stack linked in to initialise. You
can read a title's recomp hazards off its Fission symbol groups before writing a line of bring-up
code: no networking, no XNet DHCP spin; heavy `CXnIp`, budget for the transport layer.

---

## 4. Engine fingerprints in the graphics/audio mix

First-match group counts over the named functions (each name buckets once, in listed order):

| group | Fuzion Frenzy | Blinx | Crimson | Conker |
|---|---:|---:|---:|---:|
| `XGRAPHICS` (XG* helpers) | 217 | 251 | 22 | 56 |
| `D3DX` | 34 | 323 | 287 | 16 |
| `D3DDevice_` | 68 | 91 | 85 | **10** |
| `Direct3D_` | 3 | 1 | 4 | 0 |
| `DirectSound` | 85 | 249 | 384 | 387 |
| `CMcpx` (audio miniport) | 135 | — | — | — |
| `CMiniport` | 42 | 50 | 48 | 4 |
| `png` | — | 69 | 69 | — |
| `Rtl*Heap` | 17 | 17 | 19 | 20 |

Reading the differences:

- **Conker is the outlier**: only 10 named `D3DDevice_` and 16 `D3DX`, yet it is the only title
  with C++ STL symbols in the table (`?do_widen@?$ctype@D@std@@…`, `?do_narrow@…`). Rare's engine
  is more C++-abstracted — fewer direct XDK graphics-helper calls statically linked/named, more of
  its own class machinery. That build-style difference survives all the way into the recompile.
- **XGRAPHICS-heavy**: Blinx (251) and Fuzion Frenzy (217) lean on the XG swizzle/matrix helpers;
  Crimson and Conker route more through D3DX or their own code.
- **Every title recompiles its own DirectSound/audio miniport verbatim** (85–387 functions). The
  only kernel-shaped names anywhere are the `Rtl*Heap` set (17–20), because those are statically
  linked into the XBE. This confirms across four titles what the single-title teardown found:
  Microsoft's HLE line sits at the `xboxkrnl` import boundary and nowhere else — the game's D3D8,
  D3DX, DirectSound, XAPI, XGRAPHICS and XNet are all recompiled, not replaced.

- **Named coverage scales with size and library linkage**: 1,482 (Fuzion Frenzy, a 2001 launch
  title) → 3,228 (Conker). All land ~20–26 % of the guest code span named; the remainder is
  anonymous game code, still fully recompiled under the 97–98 % byte-dense map.

---

## 5. What the corpus is worth to this project

1. **The symbol tables are a per-title HLE-surface map, for free.** Before starting a recomp we
   can dump a title's Fission symbol groups and know which XDK subsystems it linked — networking,
   which audio path, how much XGRAPHICS — i.e. which of our HLE layers it will exercise.

2. **They are also a cross-title XDK library signature source.** The named library functions
   (`_RtlAllocateHeap@12`, `_Direct3D_CreateDevice@24`, `_XGSwizzleRect`, …) are the *same code*
   in every game that linked the same XDK. Combined with a game's own XBE, `(name, guest_start,
   size)` yields a byte signature per library function that transfers to any XBE — a FLIRT-style
   name-recovery database sourced from Microsoft's own symbols. See
   [ms-fusion-adoption-plan.md](ms-fusion-adoption-plan.md).

3. **The byte-dense map is a coverage oracle.** For any guest address it says "this is code",
   which we can use to validate our own function/boundary detection: a start our detector misses
   that Microsoft treats as an entry point is a detector gap worth investigating.
   `tools/fusion/coverage_oracle.py` grades it, and needs the donor title's original Xbox
   `default.xbe` to grade *against*. That XBE lives in the package's SVOD container, which is
   not sealed — see [svod-extraction.md](svod-extraction.md) for the format and the reader.

---

## Appendix: reproducing this

Per game module (`xefu_<hash>…dll`), using `pefile` + `numpy`:

- **Version resources** (`FileDescription` = source `default.xbe` path + title ID; `Comments` =
  the `ficompiler` argument dump when present) via `pefile`'s `FileInfo`.
- **Exports**: the four `*PrecompiledDll` / `Precompiled*` symbols; `PrecompiledSymbolTable` and
  `PrecompiledPointers` are exported, so their RVAs come straight from the export table.
- **`PrecompiledSymbolTable` layout** (calibrated against Crimson's report ground truth of 3,106
  records / version 1 / string blob `0x1FAEB`):

  ```
  offset  field
   +0x00  u32  string_blob_rva  (| 0x80000000 flag; RVA = value & 0x0FFFFFFF)
   +0x04  u32  version          (= 1)
   +0x08  u32  string_blob_size
   +0x0C  u32  record_count
   +0x10  u32  0
   +0x14  u32  0
   +0x18  record_count × { u32 guest_start; u32 byte_size; u32 name_offset }
  ```
  `name_offset` indexes the string blob at `string_blob_rva`.

- **Address map**: a sorted `(guest_rva, host_rva)` u32 pair array in `.rdata`. Detect it as the
  longest run whose guest column is monotonic with small deltas; coverage = `count / guest_span`.
  For the exact entry count, read the array bounds from the two `lea`/`mov` instructions in
  `InitPrecompiledDll` rather than scanning.

The four game modules live in each package's `Content/` directory. `Emu.exe` is ACL-locked by
store package protection and was not read; nothing here required it.
