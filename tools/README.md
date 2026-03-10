# Recompilation Toolchain

Python tools that transform an Xbox executable (XBE) into compilable C source code. Run them in sequence — each tool's output feeds the next.

## Prerequisites

```bash
pip install capstone    # x86 disassembly engine
```

Python 3.10+ required. On Windows, use `py -3` instead of `python3`.

## Pipeline Overview

```
default.xbe
    │
    ▼
┌─────────────────────────────────────┐
│  1. xbe_parser                       │  Parse XBE headers, sections, imports
│     py -3 -m tools.xbe_parser        │  Output: section map, kernel imports
│     game_files/default.xbe           │
└─────────────┬───────────────────────┘
              │
              ▼
┌─────────────────────────────────────┐
│  2. disasm                           │  Disassemble .text, find functions
│     py -3 -m tools.disasm            │  Output: functions.json, xrefs.json
│     game_files/default.xbe           │
│     --text-only                      │
└─────────────┬───────────────────────┘
              │
              ▼
┌─────────────────────────────────────┐
│  3. func_id                          │  Classify: CRT, RenderWare, XDK, game
│     py -3 -m tools.func_id           │  Output: classified function database
│     game_files/default.xbe -v        │
└─────────────┬───────────────────────┘
              │
              ▼
┌─────────────────────────────────────┐
│  4. recomp                           │  Translate x86 → C source code
│     py -3 -m tools.recomp            │  Output: gen/*.c, dispatch, headers
│     game_files/default.xbe           │
│     --all --split 1000               │
└─────────────┬───────────────────────┘
              │
              ▼
        gen/recomp_*.c
        gen/recomp_dispatch.c
        gen/recomp_funcs.h
        gen/recomp_stubs.c
```

## Tool Details

### 1. xbe_parser — XBE File Parser

Reads an Xbox executable and extracts all metadata needed by downstream tools.

```bash
py -3 -m tools.xbe_parser game_files/default.xbe
```

**What it extracts:**
- Base address (always 0x00010000 for retail XBEs)
- Entry point (XOR-decoded from header)
- XDK version (build number)
- Certificate info (title name, allowed media types)
- Section table (name, VA, size, raw offset for each section)
- Kernel thunk table (ordinal imports)
- TLS directory
- Library versions (statically linked XDK libs)

**Output format:** Human-readable text to stdout. Key values for your project:

```
Entry Point:   0x001D2807
XDK Version:   5849
Sections:
  .text    VA=0x00011000  Size=2863616  RawOff=0x1000
  .rdata   VA=0x0036B7C0  Size=289684   RawOff=0x2BD7C0
  .data    VA=0x003B2360  Size=3904988  RawOff=0x302360
  ...
Kernel Imports: 147 ordinals
```

Use these values to configure `xbox_memory_layout.h` in your runtime.

---

### 2. disasm — Disassembler & Function Detector

Performs static analysis on the .text section to find all functions.

```bash
py -3 -m tools.disasm game_files/default.xbe --text-only
```

**Options:**
| Flag | Description |
|------|-------------|
| `--text-only` | Only analyze .text section (skip library sections) |
| `--output-dir DIR` | Output directory (default: `tools/disasm/output/`) |
| `--verbose` / `-v` | Show progress during analysis |

**How it works:**
1. **Linear sweep** — disassembles every byte, looking for function prologues (`push ebp; mov ebp, esp` or `sub esp, N`)
2. **Recursive descent** — follows call/jump targets to discover more functions
3. **Cross-reference analysis** — builds call graph, identifies callers/callees
4. **String detection** — finds ASCII/Unicode strings referenced by code

**Output files:**
| File | Contents |
|------|----------|
| `functions.json` | Array of `{address, size, instructions, calls, callers}` |
| `xrefs.json` | Cross-reference database (call graph) |
| `strings.json` | Discovered string references |
| `stats.json` | Summary statistics |

**Function detection accuracy:**
- Typically finds 95%+ of functions on first pass
- Remaining ~5% are callback functions only reachable via indirect calls
- The recompiler handles unknown targets at runtime via the ICALL dispatch

---

### 3. func_id — Function Identifier

Classifies functions into categories to help you understand what you're looking at.

```bash
py -3 -m tools.func_id game_files/default.xbe -v
```

**Options:**
| Flag | Description |
|------|-------------|
| `-v` / `--verbose` | Show identification details |
| `--output-dir DIR` | Output directory (default: `tools/func_id/output/`) |

**Classification strategies:**

| Strategy | What It Detects |
|----------|----------------|
| **CRT identifier** | C runtime: malloc, free, memcpy, strcpy, printf, math functions |
| **RW identifier** | RenderWare engine: RwCamera*, RpWorld*, RwTexture*, RwStream* |
| **Immediate scanner** | Functions that reference known constants (vtable addresses, magic numbers) |
| **Vtable scanner** | Virtual method tables (arrays of function pointers) |
| **Stub classifier** | Empty/trivial functions (ret, xor eax,eax; ret) |
| **Clustering** | Groups similar functions by instruction patterns |

**Output:** Annotated function database with categories:

```
0x00011000  CRT     _mainCRTStartup
0x00011240  GAME    resource_loader
0x000636D0  GAME    physics_force_apply
0x001DD910  RW      RwCameraBeginUpdate
0x0034D530  RW      renderware_main_render (79KB!)
```

**Why this matters:** Knowing which functions are CRT vs RenderWare vs game code helps you prioritize. CRT functions usually "just work" in recompiled form. RenderWare functions may need manual overrides. Game functions need the most attention.

---

### 4. recomp — x86 to C Static Recompiler

The core tool. Translates every x86 instruction in every function into equivalent C code.

```bash
py -3 -m tools.recomp game_files/default.xbe --all --split 1000
```

**Options:**
| Flag | Description |
|------|-------------|
| `--all` | Recompile all detected functions |
| `--split N` | Split output into files of N functions each |
| `--func ADDR` | Recompile a single function (hex address) |
| `--output-dir DIR` | Output directory (default: `tools/recomp/output/`) |
| `--verbose` | Show per-function progress |

**Output files:**
| File | Contents |
|------|----------|
| `gen/recomp_0000.c` ... `recomp_NNNN.c` | Recompiled function bodies (split by --split) |
| `gen/recomp_dispatch.c` | Binary-search dispatch table (ICALL resolution) |
| `gen/recomp_funcs.h` | Forward declarations for all recompiled functions |
| `gen/recomp_stubs.c` | Stub functions for unresolvable targets |

**How it translates:**

```asm
; Original x86                    ; Generated C
push ebp                          PUSH32(esp, g_seh_ebp);
mov ebp, esp                      ebp_local = esp;
sub esp, 0x10                     esp -= 0x10;
mov eax, [ebp+8]                  eax = MEM32(ebp_local + 8);
add eax, [ebp+0xC]                eax += MEM32(ebp_local + 0xC);
mov [ebp-4], eax                  MEM32(ebp_local - 4) = eax;
mov esp, ebp                      esp = ebp_local;
pop ebp                           POP32(esp, g_seh_ebp);
ret                               esp += 4; return;
```

**What it handles:**
- All standard x86 instructions (MOV, ADD, SUB, CMP, TEST, JCC, CALL, RET, etc.)
- x87 FPU (FADD, FMUL, FSTP, etc.) via C `float`/`double`
- SSE/SSE2 (MOVSS, ADDSS, MULSS, COMISS, etc.) via C `float`
- MMX (basic operations, packed byte/word/dword)
- String operations (REP MOVSB, CMPSB, STOSB, SCASB)
- Indirect calls → RECOMP_ICALL() macro dispatch
- Switch/jump tables → C switch statements
- Stack frame simulation (PUSH/POP via simulated ESP)

**What needs manual attention:**
- **Indirect calls** — 90% resolve automatically via the dispatch table. The other 10% need investigation (corrupted vtables, function pointer arrays).
- **Self-modifying code** — not supported (Xbox games rarely use this).
- **Inline assembly** — translated mechanically, but semantics may need review.
- **Large functions** — very large functions (50KB+) may have issues with branch targets spanning too far. The recompiler handles this, but edge cases exist.

---

### 5. xmv — Xbox Media Video Demuxer

Extracts video/audio streams from Xbox XMV container files.

```bash
py -3 -m tools.xmv game_files/Video/intro.xmv
```

XMV is the Xbox's proprietary video container format used for FMV sequences, boot videos, and cutscenes. This tool splits the container into separate video and audio elementary streams that can be converted with FFmpeg or played with Media Foundation.

---

## After Recompilation

Once you have the generated C files:

1. **Create a game project** with CMake
2. **Link against xboxrecomp** (the runtime libraries)
3. **Add recomp_types.h** from `templates/runtime/` (or use the template as reference)
4. **Build** — MSVC compiles the generated code into a native .exe
5. **Run** — it will crash. That's normal.
6. **Debug iteratively** — see [docs/pipeline/06-debugging.md](../docs/pipeline/06-debugging.md)

The generated code is intentionally verbose and mechanical — it's not meant to be pretty, it's meant to be correct. Each function maps 1:1 to the original Xbox binary.

## Regeneration

If you regenerate the recompiled code (after fixing the recompiler or re-running with new options), you'll need to **re-apply manual patches** to the gen files. Keep a list of your patches — this is the most error-prone part of the workflow.

Recommended workflow:
1. Keep manual overrides in a separate file (`recomp_manual.c`)
2. Use `#if 0` / `#endif` to disable gen functions that have manual replacements
3. Track gen patches in a document (see CLAUDE.md in the burnout3 repo for an example)
