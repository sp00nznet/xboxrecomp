# Ghidra Headless Naming Pipeline (Xbox XBE)

Recovers real function names / symbols / decompiled C from the Xbox XBE Xbox
XBE using **Ghidra headless**, then turns them into a `{address: name}`
map the static recompiler can consume to replace `sub_XXXXXXXX` names with
meaningful ones.

The recompiler reads `tools/disasm/output/functions.json` and emits each
function as `func_info.get("name", "sub_<ADDR>")`. Put a meaningful `name` on an
entry and the regenerated C uses it. Addresses are Xbox VAs (image base
`0x00010000`), so everything here is keyed on matching VAs.

## Layout

```
tools/ghidra_naming/
  extract_for_ghidra.py          # build a flat raw image from the XBE for Ghidra
  run_ghidra.sh                  # end-to-end runner (Git Bash): extract + analyze + export
  merge_names.py                 # build ghidra_names.json (and optional --apply)
  ghidra_scripts/
    SetAnalysisOptions.java   # pre-analysis: enable FidDb/RTTI/demangler/etc.
    ExportXbeNames.py            # post-analysis: export the 3 JSONs (Jython)
  work/                          # generated: flat image, Ghidra project, logs, run_headless.bat
  export/                        # generated: functions.json, symbols.json, decompiled.json
  ghidra_names.json              # generated: {address: meaningful_name} for the recompiler
```

## Why a flat raw image (not an XBE loader)

Ghidra ships no XBE loader. There is a community one,
[XboxDev/ghidra-xbe](https://github.com/XboxDev/ghidra-xbe), which warns about a
version mismatch on current Ghidra and works anyway — @DarthSidious666 has run
it against 12.1.3 (#26). This tool does not need it: flattening the image is
version-proof and puts every address exactly where `functions.json` expects it,
which is the property that matters when the names are merged back. The approach
used here is:

1. `extract_for_ghidra.py` uses the repo's `tools/xbe_parser` to read the XBE
   section table and assemble **one flat image** spanning `0x00010000` to the end
   of the last section, each section's raw bytes placed at `(VA - base)`, gaps
   zero-filled (`work/xbe_flat.bin`, ~7.6 MB).
2. Ghidra imports it as a **raw binary**, `x86:LE:32:default`, compiler spec
   `windows` (MSVC), image base `0x00010000`. Because the image starts at the XBE
   base, Ghidra's addresses equal the recompiler's `functions.json` `start`
   values exactly. All 17 sections (incl. `.rdata`/`.data`) are loaded so
   cross-section references (vtables, RTTI, strings) resolve.

## What does the naming

Auto-analysis runs with naming-relevant analyzers forced ON
(`SetAnalysisOptions.java`). The pre-script enables a superset and
silently skips any analyzer the current loader doesn't register:

- **Function ID** — matches code against Ghidra's bundled x86 FidDb databases
  (`vsOlder_x86`, `vs2012/2015/2017/2019_x86`, auto-discovered from the
  FunctionID module's `data/` dir). `vsOlder_x86` fits the XDK-era MSVC CRT.
  **This is the naming win on a stripped retail binary** — it recovers the
  statically linked CRT/runtime helpers (malloc, qsort, sprintf, the 64-bit
  math/`_alldiv` helpers, SEH `_global_unwind2`, `_ftol`, low-level I/O, etc.).
- **Decompiler Parameter ID / Switch Analysis**, **Scalar Operand References**,
  **Aggressive Instruction Finder**, **Shared Return Calls** — better function
  boundaries, signatures, and code coverage.

Note: **Library Identification**, **Demangler Microsoft**, and the **PE RTTI**
analyzers are PE-format–specific and are NOT registered for the *Raw Binary*
loader, so they are skipped (logged as "analyzer not present"). FidDb still
runs and is the dominant name source here. A handful of MSVC-mangled symbols
(e.g. `operator new` = `??2@YAPAXI@Z`) are still recovered via FidDb itself.

### Measured yield (this binary)

- Ghidra discovers ~8,360 functions from the flat image; ~6,287 land on a
  recompiler `functions.json` `start` address.
- `ghidra_names.json` = **134 meaningful (FidDb/CRT) names**, of which **131
  match recompiler function starts**. These are high-confidence library names.
- The bulk of the 22,178 recompiler functions are proprietary Criterion /
  RenderWare / D3D8LTCG game code with **no FidDb signatures**, so they keep
  their `sub_` names. 134 is effectively the FidDb ceiling for this title
  without a custom XDK/RenderWare signature database.

### Optional: function seeding (off by default)

`ghidra_scripts/SeedFunctions.py` can pre-create Ghidra functions at
all 22,178 known entry points before analysis. This **raises** function/address
overlap (~6,287 → ~7,370) but **slightly lowers** FidDb name matches (~131 →
~93) because seeded boundaries perturb FidDb's body hashing. Because the names
are the goal, seeding is **not** used by default. To enable, add
`-preScript SeedFunctions.py` before the `SetAnalysisOptions` pre-script
in `work/run_headless.bat` (or `run_ghidra.sh`).

## Run it (Git Bash)

End to end (fast: functions + symbols, no decompilation):

```bash
tools/ghidra_naming/run_ghidra.sh
```

Then build the name map:

```bash
py -3 tools/ghidra_naming/merge_names.py
```

Optional decompilation (slow; resumable, bounded per run):

```bash
tools/ghidra_naming/run_ghidra.sh decompile 4000     # up to 4000 funcs this run
tools/ghidra_naming/run_ghidra.sh decompile all       # everything (hours)
```

Re-run decompilation without re-analyzing (continues where it left off):

```bash
ANALYZE=0 IMPORT=0 tools/ghidra_naming/run_ghidra.sh decompile 4000
```

### Windows-shell note

`run_ghidra.sh` generates `work/run_headless.bat` and invokes Ghidra's
`analyzeHeadless.bat` through it (single-line, internal log redirection,
`< nul` so a stray `pause` never hangs a background run). If running the batch
by hand from Git Bash, use the MSYS-safe form so `/c` isn't path-mangled:

```bash
cmd //c "call E:\xbe\tools\ghidra_naming\work\run_headless.bat nodecompile"
```

Log: `work/analyze.log`.

## Outputs

- `export/functions.json` — every function:
  `{address, name, signature, calling_convention, param_count, is_thunk, namespace}`
- `export/symbols.json` — symbol table:
  `{address, name, type, namespace, source, primary}`
- `export/decompiled.json` — per-function decompiled C (when enabled):
  `{address, name, decompiled_c}`; `decompiled_progress.json` tracks resume state.
- `ghidra_names.json` — `{ "0x00352560": "name", ... }`, **meaningful names only**.

## merge_names.py

Reads `export/functions.json` + `export/symbols.json`, filters out Ghidra
placeholders (`FUN_*`, `LAB_*`, `DAT_*`, `SUB_*`, `thunk_FUN_*`, `switchD_*`,
`caseD_*`, hex/empty), sanitizes each name to a valid C identifier, de-dupes
collisions by appending `_<addr>`, avoids C keywords, and reports counts by
source (fidb/library, demangled, rtti, symbol). It also prints how many
recovered addresses actually match the recompiler's `functions.json` `start`s.

Apply into the recompiler (writes a `.bak` first) — **left to the human/main
agent**, not run automatically:

```bash
py -3 tools/ghidra_naming/merge_names.py --apply
```

After applying, regenerate the affected recomp C to pick up the new names.
