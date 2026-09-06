# xboxrecomp

```
 #   #  ####    ###   #   #         #####   ###    ###   #       ###
 #   #  #   #  #   #  #   #           #    #   #  #   #  #      #
  # #   ####   #   #   # #            #    #   #  #   #  #       ##
  # #   #   #  #   #   # #            #    #   #  #   #  #         #
 #   #  #   #  #   #  #   #           #    #   #  #   #  #         #
 #   #  ####    ###   #   #           #     ###    ###   #####   ###

 Static Recompilation Toolkit for Original Xbox Games
```

> Turn any Xbox game binary into a native Windows executable. No emulation. No interpreter. Just raw, recompiled C.

**[Join the sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — the
community hub for sp00nznet's recomp projects, where ps3recomp development
happens in the open. Good place to ask questions, show a port you are working
on, or find out what people are stuck on before you duplicate the effort.

**Title-agnostic.** The runtime, kernel layer, D3D8 abstraction, NV2A translator, and the Python pipeline (parser → disasm → func_id → abi_analysis → recomp) all derive per-title layout and behavior from the XBE itself. *Burnout 3: Takedown* was the reference title the toolkit was built against, so many docs use its metrics as examples — see `docs/candidate-games.md` for ports in progress.

### Recent Changes

**Current version: v0.7.1 — _"Non-Local"_ (September 2026).**
See the [Changelog](#changelog) for what landed and when.

---

## What Is This?

This is a complete toolkit for **statically recompiling original Xbox (2001-2005) games** from their retail XBE executables into native Windows programs.

Static recompilation takes the raw x86 machine code from an Xbox binary and translates every function — every `mov`, every `jmp`, every `call` — into equivalent C source code. That C code compiles with MSVC into a native x86-64 `.exe` that runs on modern Windows. The game's original logic executes directly on your CPU, not through an interpreter or JIT compiler.

**This is the first *public* static recompilation toolkit for the original Xbox.**
Microsoft got here first: their internal Ficl/Fission recompiler shipped Xbox
back-compat on the 360. We have since studied it — see
[Microsoft's Own Recompiler](docs/technical/ms-fusion-recompiler.md).

The technique has been proven on other platforms — [N64Recomp](https://github.com/N64Recomp/N64Recomp) showed MIPS-to-C was viable, [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) brought it to Xbox 360's PowerPC — but nobody had tackled the OG Xbox until now. Its x86 architecture makes it both easier (same instruction set family as the host) and harder (variable-length instructions, complex addressing modes, x87 FPU stack) than MIPS or PPC targets.

### Why Not Just Use an Emulator?

Emulators are great. Cxbx-Reloaded and xemu do incredible work. But static recomp offers some unique advantages:

- **Native performance** — recompiled code runs at full speed, no interpretation overhead
- **Moddability** — the output is human-readable C code; you can patch, extend, and improve the game
- **Portability** — the C output can target any platform with a C compiler (ARM, RISC-V, WebAssembly...)
- **Preservation** — a self-contained native binary is the ultimate form of game preservation
- **Understanding** — the process forces you to deeply understand the game at the machine code level

## The Pipeline

```
         YOUR XBOX DISC
              |
              v
    +-------------------+
    |  1. Extract XBE   |     Extract default.xbe from the disc image
    +-------------------+
              |
              v
    +-------------------+
    |  2. Parse XBE     |     Read headers, sections, kernel imports
    +-------------------+     tools/xbe_parser/
              |
              v
    +-------------------+
    |  3. Disassemble   |     Find functions, build control flow graphs
    +-------------------+     tools/disasm/
              |
              v
    +-------------------+
    |  4. Identify      |     Classify: CRT, RenderWare, D3D, game code
    +-------------------+     tools/func_id/
              |
              v
    +-------------------+
    |  5. Lift to C     |     Translate x86 instructions to C statements
    +-------------------+     tools/recomp/
              |
              v
    +-------------------+
    |  6. Build Runtime  |    Kernel shim, D3D translation, memory layout
    +-------------------+     templates/runtime/
              |
              v
    +-------------------+
    |  7. Compile & Run  |    MSVC builds native .exe — game runs!
    +-------------------+
```

## Runtime Libraries

Following the [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) pattern (which does the same for Xbox 360 via Xenia), xboxrecomp provides link-time libraries extracted from [xemu](https://github.com/xemu-project/xemu) and purpose-built compatibility layers. Your recompiled game links against these — no emulator needed at runtime.

| Library | Source | What It Does |
|---------|--------|-------------|
| **xbox_kernel** | Custom | Xbox kernel → Win32 (120 of the kernel's 371 ordinals routed, 119 with dedicated bridge functions: memory, file I/O, threading, sync, crypto, HAL, EEPROM, SMBus) |
| **xbox_d3d8** | Custom | D3D8 → D3D11 graphics: **4-stage multi-texture** FFP pipeline, **NV2A register combiner** pixel shaders, **programmable vertex shaders** (NV2A microcode → HLSL), **hardware T&L lighting** (8 lights), **vertex fog**, DrawPrimitiveUP ring buffer, texture unswizzling, 20+ format conversions |
| **xbox_dsound** | Custom | DirectSound → software mixer (IDirectSound8/IDirectSoundBuffer8) |
| **xbox_apu** | xemu *(LGPL-2.1+)* | MCPX APU audio (256-voice processor, ADPCM/PCM, envelopes, HRTF, waveOut output) |
| **xbox_nv2a** | xemu *(regs, LGPL-2.1+)* + Custom | NV2A GPU (register handlers, MMIO interception, push buffer parsing, PGRAPH → D3D11 translation) |
| **xbox_input** | Custom | Xbox gamepad → XInput |
| **xbox_video** | Custom | FMV playback: Media Foundation decode onto a D3D8 texture, plus a window on the guest framebuffer. For titles whose video is a container Windows already decodes, the emulated decoder does not have to work for the video to be watchable — and the title still decides when it plays |

### Building the Libraries

```bash
cd xboxrecomp
cmake -S . -B build
cmake --build build --config Release
```

This produces 6 static libraries in `build/src/*/Release/`. Link your game project against `xboxrecomp` (umbrella target) or individual libraries.

**This repo builds libraries only — there is no game `.exe` here, and building it will never produce one.** The executable is built by *your* game project, which lives in its own directory and links these libraries. Start it by copying [`templates/new-game/`](templates/new-game/): it has the `CMakeLists.txt` that produces the `.exe` and the `main.c` that boots the guest. See [Getting Started, Step 6](docs/GETTING_STARTED.md#step-6-create-your-game-project).

### Integration Pattern

Your recompiled game provides two callback functions that the kernel bridge calls to resolve function addresses:

```c
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);        // Auto-generated dispatch table
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);  // Hand-written overrides
```

The recompiler output (`tools/recomp`) generates these automatically. The xboxrecomp libraries handle everything else — memory layout, kernel calls, graphics, audio, and input.

### Architecture

```
┌─────────────────────────────────────────────────┐
│              Your Game (.exe)                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ recomp/  │ │ manual   │ │ game-specific    │ │
│  │ gen/*.c  │ │ overrides│ │ loaders/formats  │ │
│  └────┬─────┘ └────┬─────┘ └────────┬─────────┘ │
│       │             │                │            │
│       └──────┬──────┘────────────────┘            │
│              │ recomp_lookup() / ICALL dispatch    │
├──────────────┼────────────────────────────────────┤
│              │   xboxrecomp libraries             │
│  ┌───────────┴──────────┐                         │
│  │    xbox_kernel        │  Memory layout, file    │
│  │    (kernel_bridge.c)  │  I/O, threading, sync   │
│  └───────────┬──────────┘                         │
│              │                                     │
│  ┌───────┐ ┌┴──────┐ ┌────────┐ ┌──────┐ ┌─────┐│
│  │xbox_  │ │xbox_  │ │xbox_   │ │xbox_ │ │xbox_││
│  │d3d8   │ │dsound │ │apu     │ │nv2a  │ │input││
│  │D3D8→  │ │DSound→│ │MCPX APU│ │NV2A  │ │XPP→ ││
│  │D3D11  │ │mixer  │ │(xemu)  │ │(xemu)│ │XInput│
│  └───────┘ └───────┘ └────────┘ └──────┘ └─────┘│
├──────────────────────────────────────────────────┤
│  Windows 11: D3D11, XInput, waveOut, Win32 API   │
└──────────────────────────────────────────────────┘
```

## Quick Start

### Prerequisites

- **Windows 11/10** (D3D11 backend) — or **Linux** (OpenGL backend; `tools/linux/install_deps.sh`)
- **Python 3.10+** with `capstone` (`pip install capstone`)
- **Visual Studio 2022** (MSVC compiler)
- **CMake 3.20+**
- An original Xbox game disc image (you must own the game)

### Step-by-Step

The condensed version. [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) is
the long one, and the one to read if a step here does not go as written — it
explains *why* each flag is there, which is what you need when your title
behaves differently from the example.

```bash
# 1. Clone this repo
git clone https://github.com/sp00nznet/xboxrecomp.git
cd xboxrecomp

# 2. Extract default.xbe from your Xbox disc image
#    (Use xdvdfs, extract-xiso, or similar tool)
mkdir game_files
# copy default.xbe and game data into game_files/

# 3. Parse the XBE — learn what you're working with
#    --json is NOT optional: step 4 reads the section layout back out of it.
#    The name matters too. Step 4 looks for <xbe stem>_analysis.json beside the
#    XBE, so keep it there and keep the suffix.
py -3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json
#    Output: section map, kernel imports, entry point, XDK version

# 4. Disassemble — find all functions
py -3 -m tools.disasm game_files/default.xbe --text-only
#    Output: tools/disasm/output/ (functions.json, xrefs.json, strings.json)
#    --text-only does what it says: only .text. A title with code in its XDK
#    library sections (D3D, DSOUND, XPP...) needs them named explicitly, e.g.
#    --extra-sections XIPS,DOLBY. Drop --text-only to take every code section.

# 5. Identify library functions
py -3 -m tools.func_id game_files/default.xbe -v
#    Output: tools/func_id/output/ (CRT, RenderWare, vtables classified)

# 6. Recover calling conventions and parameter counts
py -3 -m tools.abi_analysis game_files/default.xbe -v
#    Output: tools/abi_analysis/output/abi_functions.json
#    Skipping this still "works", but every function falls back to
#    cdecl / 0 params / int-or-void, so the generated signatures are guesses.

# 6b. Optional: real names instead of sub_XXXXXXXX, if you have Ghidra.
#     FidDb recognises the statically linked CRT/XDK helpers and names a few
#     hundred of them. Do it BEFORE step 8: the recompiler emits whatever name
#     is on the functions.json entry, so the names reach the generated C,
#     crash traces and ABI reports. See docs/GETTING_STARTED.md step 4.5.
XBE=game_files/default.xbe tools/ghidra_naming/run_ghidra.sh
py -3 tools/ghidra_naming/merge_names.py --apply

# 7. Create your game project — this is what becomes the .exe
#    The toolkit is a library; the executable lives in your own project.
cp -r templates/new-game ../mygame        # Windows cmd: xcopy /E /I templates\new-game ..\mygame
#    Then edit:
#      ../mygame/CMakeLists.txt  -> project name, XBOXRECOMP_DIR path
#      ../mygame/src/main.c      -> YOUR_GAME_ENTRY_POINT / XBE path from step 3

# 8. Lift to C — the big one
#    --gen-dir writes the generated code into your game project, where the
#    template's CMakeLists globs src/recomp/gen/*.c. Without it the output
#    lands in this repo (src/game/recomp/gen/) and nothing compiles it.
py -3 -m tools.recomp game_files/default.xbe --all --split 1000 --gen-dir ../mygame/src/recomp/gen
#    Output: recomp_0000.c ... recomp_dispatch.c, recomp_funcs.h (millions of
#    lines of C), plus recomp_types.h — the runtime register model the
#    generated code includes. You do not supply that one; if the build says
#    "Cannot open include file: 'recomp_types.h'", this step did not finish.

# 9. Build and run — from the game project, not from xboxrecomp
cd ../mygame
cmake -S . -B build
cmake --build build --config Release
build\Release\your_game_recomp.exe          # named after project() in your CMakeLists
```

### What To Expect

The first time you run a recompiled game, **it will crash**. That's normal. The process is iterative:

1. **Boot** — get past the entry point (usually straightforward)
2. **Stub** — identify and stub out functions that touch hardware you haven't implemented yet
3. **Fix ICALLs** — indirect calls (vtable dispatches, function pointers) are the hardest 10%
4. **Add runtime** — implement kernel functions, D3D calls, and input as the game needs them
5. **Debug** — use the ICALL trace ring buffer, memory access logging, and your debugger
6. **Iterate** — each crash teaches you something about the game. Fix it and move on.

With Burnout 3 (the first game recompiled with this toolkit), the process from "empty repo" to "game boots and renders textured 3D tracks" took about two weeks of iterative development.

## Repository Structure

```
xboxrecomp/
├── README.md                    # You are here
├── CMakeLists.txt               # Top-level build (builds all runtime libs)
├── tools/                       # The recompilation toolchain (Python)
│   ├── xbe_parser/              # XBE file format parser
│   ├── disasm/                  # x86 disassembler + function detector
│   ├── func_id/                 # Library function identifier
│   ├── abi_analysis/            # Calling convention / param recovery
│   ├── recomp/                  # x86 -> C static recompiler
│   ├── debug_symbols/           # Debug-build symbol recovery
│   ├── symbols/ ghidra_naming/  # Optional symbol-name recovery
│   ├── xiso/ xmv/               # Disc image and video container tools
│   └── fusion/                  # MS Ficl/Fission study tooling
├── src/                         # Runtime libraries (C, link-time)
│   ├── kernel/                  # xbox_kernel - Xbox kernel → Win32
│   ├── d3d/                     # xbox_d3d8   - D3D8 → D3D11 graphics
│   ├── audio/                   # xbox_dsound - DirectSound compat
│   ├── apu/                     # xbox_apu    - MCPX APU emulation (xemu)
│   ├── nv2a/                    # xbox_nv2a   - NV2A GPU emulation (xemu)
│   ├── input/                   # xbox_input  - Gamepad → XInput
│   └── video/                   # xbox_video  - FMV playback + framebuffer window
├── include/xbox/                # Public umbrella header (xboxrecomp.h)
├── templates/                   # Starter templates for new projects
│   ├── new-game/                # ** Copy this to start a game project **
│   │   ├── CMakeLists.txt       # Builds the game .exe, links xboxrecomp
│   │   └── src/main.c           # Host entry point: loads XBE, boots guest
│   └── runtime/                 # Runtime shim templates
│       ├── recomp_types.h       # Register model + ICALL macros
│       ├── xbox_memory.h        # Memory layout helpers
│       └── kernel_stubs.h       # Kernel function stub templates
└── docs/                        # Documentation
    ├── pipeline/                # Step-by-step pipeline guides
    ├── technical/               # Deep technical documentation
    ├── formats/                 # Xbox file format references
    └── runtime/                 # Runtime implementation guides
```

## Documentation

### Start Here
- **[Getting Started Guide](docs/GETTING_STARTED.md)** — End-to-end walkthrough from XBE to running game
- **[Decompilation Guide](docs/DECOMP.md)** — Using this as a function splitter instead: one byte-exact `.s` per function, with signatures and the call graph. You never run the recompiler
- **[Tools Reference](tools/README.md)** — Detailed usage for every pipeline tool
- **[Runtime Libraries](src/README.md)** — Architecture, build instructions, integration guide

### Per-Module API Reference
- [xbox_kernel](src/kernel/README.md) — Memory layout, file I/O, threading, sync, crypto, EEPROM, SMBus (11,128 LOC)
- [xbox_d3d8](src/d3d/README.md) — D3D8 interface, register combiners, vertex shaders, texture unswizzle (8,838 LOC)
- [xbox_dsound](src/audio/README.md) — DirectSound buffers, 3D audio, mixbins (573 LOC)
- [xbox_apu](src/apu/README.md) — MCPX APU voice processor, mixer, MMIO (4,168 LOC)
- [xbox_nv2a](src/nv2a/README.md) — NV2A GPU registers, push buffer, PGRAPH→D3D11 (4,892 LOC)
- [xbox_input](src/input/README.md) — Gamepad state, vibration, button mapping (360 LOC)

### Pipeline Guides
- [Extracting and Parsing XBE Files](docs/pipeline/01-xbe-parsing.md)
- [Disassembly and Function Detection](docs/pipeline/02-disassembly.md)
- [Function Identification](docs/pipeline/03-function-id.md)
- [x86 to C Lifting](docs/pipeline/04-lifting.md)
- [Building the Runtime](docs/pipeline/05-runtime.md)
- [Iterative Debugging](docs/pipeline/06-debugging.md)

### Technical Deep Dives
- [The Register Model](docs/technical/register-model.md) — Why global registers work and how the stack is simulated
- [Memory Layout Reproduction](docs/technical/memory-layout.md) — CreateFileMapping, mirror views, and address space tricks
- [Indirect Call Dispatch](docs/technical/indirect-calls.md) — The RECOMP_ICALL problem and how to solve it
- [D3D8 to D3D11 Translation](docs/technical/d3d-translation.md) — Bridging Xbox's graphics API to modern DirectX
- [NV2A Shader Translation](docs/technical/nv2a-shaders.md) — Register combiners and vertex microcode to HLSL
- [D3D8LTCG Device Context](docs/technical/d3d8ltcg-device-context.md) — Device field map, PB ring management, stub calling conventions
- [Xbox Kernel Replacement](docs/technical/kernel-replacement.md) — Mapping Xbox kernel ordinals to Win32
- [SEH and Exception Handling](docs/technical/seh-handling.md) — Structured exception handling in recompiled code
- [Lessons Learned](docs/technical/lessons-learned.md) — What worked, what didn't, mistakes to avoid
- [Gap Analysis vs xemu](docs/technical/gap-analysis.md) — What's implemented, what's missing, prioritized roadmap
- [Microsoft's Own Recompiler](docs/technical/ms-fusion-recompiler.md) — White-room analysis of Ficl/Fission: pipeline, address map, HLE boundary
- [Ficl/Fission Codegen Teardown](docs/technical/ms-fusion-codegen-teardown.md) — IDA/Hex-Rays teardown of both their translators, and how it reframes our roadmap
- [Burnout 3 Reunification](docs/technical/burnout3-reunification.md) — bringing the origin title back onto the extracted toolkit: what's done, and the threading gate that makes the runtime a merge not a swap

### Xbox Formats
- [XBE File Format](docs/formats/xbe.md) — Xbox executable format reference
- [Xbox Kernel Exports](docs/formats/kernel-exports.md) — All 366 kernel functions documented

## How It Works

The interesting parts each have their own document rather than a summary here,
so there is one place to keep correct:

- **[The Register Model](docs/technical/register-model.md)** — why the guest
  registers are globals (and thread-local), how the guest stack is simulated,
  and why every recompiled function is `void f(void)`.
- **[Memory Layout](docs/technical/memory-layout.md)** — reproducing the Xbox
  address space with `CreateFileMapping` + 28 mirror views, and why
  `VirtualAlloc` cannot do it (mirrors must alias the same physical pages, not
  copy them).
- **[Indirect Call Dispatch](docs/technical/indirect-calls.md)** — `call [eax+0x10]`
  with no compile-time target. The hardest part of any bring-up.
- **[NV2A Shader Translation](docs/technical/nv2a-shaders.md)** — register
  combiners and vertex microcode to HLSL, both translated at runtime and cached.
- **[SEH and Exception Handling](docs/technical/seh-handling.md)** — how
  `__SEH_prolog`/`__SEH_epilog` are detected per title and bridged.

## Games That Work Well As Targets

Based on our experience with Burnout 3, the best candidates for Xbox static recomp share these traits:

| Factor | Easier | Harder |
|--------|--------|--------|
| **Engine** | RenderWare (shared patterns) | Custom engine (unique quirks) |
| **Threading** | Single-threaded | Multi-threaded with sync |
| **GPU usage** | Standard D3D8 calls | NV2A push buffer microcode |
| **Code size** | Small .text section | Large with LTCG |
| **Online** | Offline only | Xbox Live dependent |
| **PC port** | No PC version (worth the effort!) | Good PC port exists |

See [docs/technical/candidate-games.md](docs/technical/candidate-games.md) for a detailed list of promising targets.

## Projects Using This Toolkit

- **[Burnout 3: Takedown](https://github.com/sp00nznet/burnout3)** — The origin title and most mature target. 22,097 functions lifted. An earlier build was playable to the main menu at 60fps, but leaned on hand-written menu and render scaffolding; that is being replaced with genuinely recompiled code, and the honest bring-up currently reaches engine/RenderWare init. Treat the old "playable" claim as retired until the recompiled path gets back there.
- **[Xbox Dashboard](https://github.com/sp00nznet/xboxdashboard)** — The original Xbox system shell (build 3944); the toolkit on system software rather than a game. Nothing renders yet: the earlier "green orb at 60fps" was the project's own scaffolding drawing a disc, and has been retired along with the fake scene root and hand-rolled asset loader around it. What runs is the dashboard's own code — full init chain, its own D3D8 sizing and allocating its own 640x480 surfaces, its own NV2A pushbuffer, its own `default.xip` read. Its UI is driven by a **VRML97 + JavaScript scene engine** (text→bytecode compiler + stack-machine VM + node-class reflection registry), which is the piece still to come online.
- **[Wreckless: The Yakuza Missions](https://github.com/sp00nznet/wreckless)** — Xbox launch title (2002). Custom engine, 3,407 functions, boots through CRT init into game main. Debugging early gameplay crash.
- **[Blood Wake](https://github.com/sp00nznet/bloodwake)** — First-party Microsoft naval combat (2001). Stormfront Studios custom engine. 4,608 functions, 367K lines of C generated (99.1% success). Project scaffolded, working toward first build.

## How You Can Help

This is an emerging field. Here's how you can contribute:

1. **Try it on a new game** — Pick an Xbox exclusive, follow the pipeline, and see how far you get. Even partial results teach us about the toolchain's gaps.
2. **Improve the lifter** — Coverage is good but unquantified; the honest signal is that an unhandled instruction lifts to a bare `/* mnemonic */` comment, so grepping generated output for those finds the gaps. Segment prefixes and the rarer x87/SSE forms are where they cluster.
3. **Document Xbox formats** — Every game has its own asset formats. Document what you discover.
4. **Build runtime components** — Better D3D8 emulation, audio, networking — the runtime layer is where most per-game work happens.
5. **Share your findings** — Write up what you learn. The Xbox modding/preservation community benefits from every discovery.

Not sure where to start, or want to sanity-check an idea first? Ask in the
[Discord](https://discord.gg/CRpzGWZFcu) — several of the people working on
ports and on the lifter are there.

## Dependencies

The toolchain is intentionally lightweight:

```
Python 3.10+
capstone        # x86 disassembly  (pip install capstone)
pytest          # test suite only  (pip install pytest)
```

That's it for the core pipeline — no IDA, no Ghidra, no proprietary tools. Just the standard library + Capstone. (An *optional* `tools/ghidra_naming` helper can use headless Ghidra purely to recover symbol names; it is never required to produce a working build.)

### Running the tests

```
py -3 -m pytest tools/       # unit tests
py -3 -m tools.conformance   # differential: lifted C vs the real CPU
```

The unit tests are fast and need no game files. The conformance suite goes
further: it assembles each snippet with MSVC, lifts the resulting bytes, then
runs the lifted C *and the original instructions* over the same inputs and
requires them to agree. Because we target x86 and run on x86, the host CPU is
the oracle — no model to be wrong. See
[Conformance Testing](docs/technical/conformance-testing.md). It needs a 32-bit
MSVC, and is skipped rather than failed where there isn't one.

If you fix a lift, add the case.

The runtime libraries (C) use:
- MSVC (Visual Studio 2022) or MinGW-w64
- Windows SDK (D3D11, DXGI, XInput, waveOut)
- CMake 3.20+
- No external dependencies — all hardware emulation code is self-contained

## FAQ

**Q: Is this legal?**
A: This project provides tools and documentation. You must own a legitimate copy of any game you recompile. No copyrighted game code or assets are included in this repository.

**Q: How is this different from an emulator?**
A: Emulators interpret or JIT-compile code at runtime. Static recompilation translates the entire binary ahead of time into native C code that compiles to a regular `.exe`. There's no CPU emulation at runtime — the recompiled functions execute directly.

**Q: Can I use this on Xbox 360 games?**
A: No. Xbox 360 uses PowerPC (big-endian, different ISA). See [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for Xbox 360 static recompilation. This toolkit is specifically for the original Xbox's x86 code.

**Q: How long does it take to get a game running?**
A: It depends on the game's complexity. Burnout 3 went from zero to "boots and renders 3D tracks" in about two weeks. Simple games might be faster; complex ones with custom engines could take longer. The toolchain handles the mechanical translation — the real work is building the runtime shims and debugging indirect calls.

**Q: Why C output instead of direct x86-64 binary translation?**
A: C is portable, debuggable, and the compiler optimizes it for you. You can read the output, set breakpoints in it, and modify individual functions. Direct binary translation would be faster to run but impossible to debug or modify.

## License

**MIT** — see [LICENSE](LICENSE). Third-party components keep their original
licence:

| Component | Licence | Copyright |
|---|---|---|
| the MCPX APU sources in `src/apu/` | LGPL-2.1-or-later | espes; Jannik Vogel; Matt Borgerson |
| `src/nv2a/nv2a_regs.h` | LGPL-2.1-or-later | espes; Jannik Vogel |
| everything else | MIT | sp00nz and contributors |

The APU and the NV2A register definitions were extracted from
[xemu](https://github.com/xemu-project/xemu) and are that project's work, not
ours. LGPL-2.1 expressly permits linking them from MIT or proprietary code, so
a recompiled game is unaffected; what it asks is that the notices stay, the
source stays available, and users can relink against a modified library.
[LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) is the verbatim licence text —
shipping it alongside those files is a requirement, not a courtesy.

Not every file under `src/apu/` and `src/nv2a/` is xemu-derived. See
[NOTICE](NOTICE) for the exact list, each with the copyright it actually
carries — including algorithms we implemented ourselves but learned from xemu,
credited there even where no licence obligation attaches.

## Contributors

xboxrecomp is built by more than one person. See
**[CONTRIBUTORS.md](CONTRIBUTORS.md)** for who did what — including the people
who never sent a patch and still moved the project further than a patch would
have, by finding the wall everyone else was about to hit.

Thank you, all of you.

## Credits

Built with [Claude Code](https://claude.ai) (Anthropic) — proving that AI-assisted systems programming can tackle problems previously considered impractical.

Human contributors are credited in [CONTRIBUTORS.md](CONTRIBUTORS.md); the
third-party code we build on is credited in [NOTICE](NOTICE).

## Changelog

Versions start at v0.1.0 with the initial public release; earlier entries were
reconstructed from the commit history, so they are dated by when the work
actually landed rather than by any tag that existed at the time.

### v0.7.1 — *"Non-Local"* (September 2026)

*Contributed work, plus what a system application asks for that a game does not.*

**Contributed.**

- **`ReleaseMutex` reported success for a release it never performed** — the
  POSIX shim returned `TRUE` unconditionally, so a thread releasing a mutex it
  did not own got success and `NtReleaseMutant` handed `STATUS_SUCCESS` back to
  the guest. The guest then ran on believing a still-held mutex was free. Also
  adds the missing `ERROR_NOT_OWNER` and sets `ERROR_INVALID_HANDLE` on the
  bad-handle path — *[@dplewis](https://github.com/dplewis)* (#18)
- **D3D8 texture translation**, 4,096 lines and the largest single contribution
  to that layer. All 66 Xbox `D3DFMT_*` formats mapped to DXGI, cube textures as
  a `Texture2DArray` with per-face unswizzle, volume textures as `Texture3D`
  with 3D Z-order unswizzle, and software channel conversion for the formats
  with no direct DXGI equivalent. Ships `tests/d3d8_smoke`, which builds the real
  `d3d8_resources.c` against stub device accessors so the format tables are
  checkable without a D3D11 device. The same PR took hardcoded *Burnout 3*
  strings out of the tools and the Linux default paths —
  *[@DarthSidious666](https://github.com/DarthSidious666)* (#17)

Generated-code banners now prefer the title read from the XBE header, with
`--game-name` as an explicit override — the two mechanisms arrived from
different directions in the same release and both are worth having.

**The Xbox Dashboard reached its frame loop**, which meant finding four things
between a title and a first visible frame, none of them in the title:

- **Worker thread stacks were never reclaimed.** The pool counted threads ever
  created rather than threads alive, so a title that cycles workers exhausted it
  and `PsCreateSystemThreadEx` began running them *inline* — which deadlocks
  rather than slows, because the worker finishes before its caller reaches the
  wait it was going to be signalled from.
- **`0xFF000000` was not mapped.** The MCPX span stops one page short of the
  flash ROM, so an access that is ordinary on hardware was a hard fault. Backed
  as plain memory like the NV2A and MCPX apertures.
- **The pushbuffer survey read the wrong memory.** `DMA_PUT` holds a physical
  address and `nv2a_pb_scan` takes guest VAs, so it walked low memory and
  reported a confident inventory of nothing while the title was submitting
  methods all along.
- **The framebuffer window only ever opened from `AvSetDisplayMode`**, so a
  title that draws before setting a display mode got no window however much it
  rendered. The pushbuffer executor opens it now, when a clear has just proved a
  surface address is real.

`RECOMP_WATCHDOG_SECS` also did nothing in any project copied from the template,
because `xbox_WatchdogStart()` is the host's to call and the template never
called it — the one diagnostic that separates a hang from slowness, silently
inert while appearing to be set.

**`tools.split`** — one byte-exact `.s` per function, for decompilation rather
than recompilation. The bytes are `db` directives and the disassembly is the
comment beside them, because x86 has multiple encodings per mnemonic and
reassembling a listing produces code that runs identically and does not *match*.
Verified against the binary: 2,254 of 2,254 functions in the Xbox Dashboard's
`.text` are byte-identical, including the ones with MSVC switch tables parked
mid-body. See [docs/DECOMP.md](docs/DECOMP.md).

**Fixed for new users**, all three from people reporting where they got stuck:
`recomp_types.h` is now written into `--gen-dir` by the pipeline instead of
living only in `templates/runtime/`; `tools.disasm` names the analysis JSON it
wants and the command that writes it; the README's own quick start ran
`tools.xbe_parser` with no `--json`, which is why the next step could not find
it. The project template also could not link, defining three ICALL globals the
runtime already owns.

### v0.7.0 — *"Non-Local"* (August 2026)

*Control flow that leaves a function without returning from it, and the three
places the toolkit got that wrong.*

**Non-local jumps.** A recompiled function is a real C function, so restoring
the guest's `esp` is only half of a `longjmp`: the abandoned frames are still on
the native stack, and control returns into them once the resume point finishes.
Each guest `jmp_buf` is now paired with a native one taken at the `setjmp` call
site — the only place a native `setjmp` is valid — and the guest `longjmp`
becomes a native one, so the frames actually unwind. The CRT's pair is found by
the `"VC20"` cookie MSVC stamps into every `jmp_buf`. On the title tested this
turned a correctly caught image-loader exception, which had been re-entering the
decoder on a dead frame and looping forever, into a clean unwind.

**Frameless callees inherited a dead frame.** A function with no prologue of its
own reads `ebp` through `g_seh_ebp`, but only tail jumps and the SEH helpers
ever wrote it — so one reached by an ordinary call got whatever frame the last
tail jump left behind. It is now published wherever `g_ebp` is. `setjmp` was
saving that stale frame into the buffer, so the `longjmp` that should have
resumed a catch restored a frame two calls dead.

**The `fs:` segment prefix was dropped**, putting the TIB at guest address 0 —
the same address a null pointer dereferences. Two things went wrong there and
both were silent: a null check written as `cmp byte [ecx], 0` read the exception
chain head's `0xFF` and decided the pointer was fine, and a store through a null
pointer overwrote that head instead of faulting. Segment overrides are now
recorded and based at `XBOX_FS_BASE`, which leaves page zero free —
`RECOMP_TRAP_NULL=1` then makes a null dereference fault where it happens
instead of surfacing hundreds of steps later as a NaN.

**Kernel exports that existed but were never dispatched.** `RtlUnwind`,
`XeLoadSection`/`XeUnloadSection` and `NtSuspendThread` all had implementations
and no entry in the bridge table, which is worse than an outright stub: each
returned success without doing anything. `NtSuspendThread` was the costly one —
a worker that parked itself never stopped, and spun through 289 million kernel
calls while the title believed it was idle. After bridging: 9,789.

**MCPX APU never started.** The frame thread idles on `pause_requested`, which
init sets and *only the test tone* ever cleared, so a title that enabled the APU
through `NV_PAPU_SECTL`/`FECTL` got an APU that stayed asleep. Writing those
registers now resumes it.

**Instructions.** `cvtps2pi` / `cvttps2pi` implemented — 36 of them sat inside
one title's WMV decoder as no-op comments.

**Diagnostics**, because a recompiled title offers no debugger and no printf:

- `tools/stackwalk.py` — guest backtraces from a stack dump. The native stack
  shows only whichever translated function is spinning; the guest stack still
  carries a return site for every guest frame.
- `RECOMP_WATCHDOG_SECS` — dumps the guest call stack when a title stops making
  progress, which is otherwise indistinguishable from working.
- `RECOMP_TRACE_ARGS` / `RECOMP_TRACE_DEREF` — stack arguments and one level of
  pointer dereference at each traced entry. Registers alone will not tell you
  which argument arrived null.
- `RECOMP_PEEK` / `RECOMP_PEEK_CHAIN` — read guest dwords, or walk a pointer
  chain, without a run per level.
- `RECOMP_WATCH_VA` — hardware watchpoint on a guest address, generalised from a
  single hardcoded one.
- `RECOMP_PB_SCAN` / `RECOMP_PB_EXEC` — survey a title's NV2A pushbuffer and
  execute its surface and clear methods. The survey ranks what is *not*
  implemented, so the remaining work is a list rather than a guess.
- `RECOMP_FB_WINDOW` — a window on the guest framebuffer. Nothing else scans it
  out, so however much of the GPU works, none of it is observable without this.

**Fixed:** duplicate trace symbols broke the link for any title defining its own
`recomp_trace_*`; they now live once in the kernel.

### v0.6.0 — *"Credit Where Due"* (August 2026)

*The first release with contributors other than the maintainer, and the
housekeeping that should have been in place before there were any.*

**Correctness — the silent kind.** Every fix here produced C that compiled,
linked, ran, and was wrong, with no lifter warning anywhere.

- **Conditional tail calls skipped the frame bridge** — `jcc` to a known
  function entry is a tail call, but only the unconditional form emitted the
  bridge, so the taken edge ran with the caller's frame still live. 8,263 call
  sites across 5,426 functions on the title tested — *[@NoRain211](https://github.com/NoRain211)* (#7)
- **Indirect calls read their target after the return-address push**, so
  `call [esp+X]` resolved from the wrong slot — *[@NoRain211](https://github.com/NoRain211)* (#7)
- **`repe cmpsb` / `repne scasb` folded their flags to a literal 1**, so every
  `memcmp`/`strcmp`-shaped loop in the CRT reported "equal" regardless of
  input — *[@NoRain211](https://github.com/NoRain211)* (#8)
- **`NEG` carry was dropped before a non-adjacent `SBB`/`ADC`**, which is the
  standard 64-bit subtract and sign-extend idiom — *[@NoRain211](https://github.com/NoRain211)* (#8)
- **Signed compares evaluated at 32 bits regardless of operand width**, so the
  sign bit of an 8- or 16-bit operand was never in the right place — *[@NoRain211](https://github.com/NoRain211)* (#8)
- **Packed SSE was lifted as a scalar `float`** — `movaps`/`movups` moved 4 of
  16 bytes and dropped the upper three lanes (18,439 moves), and packed
  arithmetic had no pattern at all (561 operations dropped) — *[@NoRain211](https://github.com/NoRain211)* (#9)
- **904 x87 instructions across 28 mnemonics lifted to comments**, desynchro-
  nising the FPU stack from that point on; `FNSTCW`/`FNSTSW` were comments too,
  so every `fcom`-derived parity test read a hardcoded `true` (1,326 sites) — *[@NoRain211](https://github.com/NoRain211)* (#9)
- **XMM was a function-local**, so a value written in one lifted block and read
  in the next was lost — *[@NoRain211](https://github.com/NoRain211)* (#10)

**Pipeline**

- **`tools/abi_analysis` now exists.** `tools.recomp` had always looked for
  `abi_functions.json`, warned when it was missing, and then fallen back to
  cdecl / 0 params / int-or-void for *every* function — because the tool meant
  to produce that file was never written. Recovers calling convention
  (including thiscall), parameter count from the `ret` immediate, return-type
  hints and frame shape — *[@DarthSidious666](https://github.com/DarthSidious666)* (#6)
- **The SSE runtime.** The lift in #9/#10 emitted 28 `XMM_*` helpers that
  nothing defined. Added `RecompXmm` plus lane-wise implementations, verified by
  compiling real lifter output under MSVC and checking the cases where x86
  disagrees with naive C — `MINPS` returning its second operand on a tie,
  `ANDNPS` being `~dst & src`, `CMPNEQPS` being the unordered form.
- **The research branch merged back**: per-title SEH detection, the
  function-boundary fix, operand-aware x87, the MS Ficl/Fission study, XISO
  redump support, and indirect-call feedback.

**Project**

- **[CONTRIBUTORS.md](CONTRIBUTORS.md)** — including the people who only ever
  filed an issue. [@Tiptup300](https://github.com/Tiptup300) (#1) found that
  every documented getting-started step was broken, on Linux; that report is why
  the pipeline was fixed *and* why this repository has a LICENSE file at all.
  [@M0RSM4LLEO](https://github.com/M0RSM4LLEO) (#2) reproduced it with the
  detail that made it actionable.
- **LGPL compliance.** The xemu-derived APU and NV2A sources always carried
  their notices, but the repository shipped no `NOTICE` and no copy of the
  licence. Both now present, with every affected file listed against the
  copyright it actually carries.
- **The test suite actually runs.** A bare import in `tools/symbols` aborted
  pytest collection for the whole tree, so `pytest tools/` executed nothing.
  Now 141 tests.
- **Differential conformance testing** (`tools/conformance`) — assembles each
  snippet with MSVC, lifts the bytes, and runs the lifted C against the original
  instructions on the real CPU over **2,043 input vectors** covering integer
  results, the x87 stack (values *and* depth) and all four SSE lanes. Adapted
  from ps3recomp's methodology, but stronger here: we target x86 and run on
  x86, so the oracle is the hardware rather than a model of it. It found three
  live bugs, all of which the existing string-comparison tests passed:
  - **`fxch st(i)` was a silent no-op** — Capstone reports `fxch` with both
    operands, `(st(0), st(i))`, and it is the only x87 form that does, so the
    handler picked up the implicit `st(0)` and swapped st0 with itself.
  - **`stc`/`clc`/`cmc` were unimplemented**, so the carry a following
    `adc`/`sbb` read kept whatever the last arithmetic left in it.
  - **`fnstsw` did not model TOP** (status bits 11–13, AH bits 3–5) and the
    `ax` form wrote only AH rather than all of AX.
- **Whole-function conformance** — a second phase compiles a C corpus with
  `/O2 /arch:IA32` (Pentium III: SSE1, no SSE2, like the real hardware), lifts
  the machine code back through the full `FunctionTranslator`, and runs it
  against the original. Testing what the optimiser emits rather than what
  someone thought to write down found two more:
  - **Flag state followed address order, not control flow.** A `jcc` consuming
    a `cmp` from a non-adjacent block inherited the flags of whatever sat above
    it in memory — usually an `add`, which clobbers them. State now propagates
    along predecessor edges, and only when every predecessor agrees.
  - **`js`/`jns` evaluated the sign at 32 bits**, so after an 8- or 16-bit
    `test` every value with the top bit set looked positive. The same width bug
    the signed compares had; these two were missed at the time.
  - **`bt`/`btr`/`bts`/`btc` were unhandled** — 386 instructions, lifted to a
    comment, so the bit was silently left alone. Surfaced once the corpus began
    lifting the CRT's float-to-int helper, which uses `btr` on the x87 control
    word.

  The corpus lifts from a **linked image**, so jump tables, `.rdata` float
  constants and calls to CRT helpers all work — `__allmul` is lifted and
  verified alongside the corpus itself.
- **Conformance against a real title** (`--xbe path/to/default.xbe`) — Xbox
  code is 32-bit x86 and the harness is a 32-bit x86 process, so a game's own
  machine code can be *executed* as the oracle: map the XBE where it was linked
  for, call one of its functions, run the lifted C over the same arguments, and
  compare. Candidates are picked mechanically (no calls, no invented pointers,
  plain `ret`, nothing lifting to a comment), so what gets compared is provably
  safe to run. Verified clean on Burnout 3, Conker, Crimson Skies and Blood
  Wake. No game files are included or needed for the rest of the suite.

  It found that **`fnstsw` did not model C2, the unordered bit**. An x87 compare
  against a NaN sets C3, C2 and C0 together, and `fucompp; fnstsw ax; test
  ah,44h; jp` is how this era's CRT asks "is this a NaN" — reporting "equal"
  answered *no* every time, sending every float classification in a title down
  the wrong branch. Found by running Crimson Skies' own float classification
  against itself.

  Totals: **2,599 snippet vectors, 211 whole-function vectors**, plus per-title
  runs (Burnout 3: 37 functions / 161 vectors clean).

### v0.5.0 — *"Fall-Through"* (July 2026)

- **Fall-through into the next function was dropped.** When the disassembler
  splits a straight-line run of code at an internal branch target, the earlier
  function often ends by falling through into the next — which x86 executes. The
  lifter emitted nothing, so the body ended and skipped the next function's
  shared epilogue: an esp leak that corrupted callee-saved registers.
  **4,587 of 35,286 functions in Burnout 3** had this shape.
- **Per-title SEH detection.** `__SEH_prolog`/`__SEH_epilog` addresses were
  hardcoded to one game's CRT, so on every other title the `ebp` read-back was
  never emitted. Found by signature now.
- Halo bring-up: debug-build symbol recovery, per-target memory map, x87
  correctness, and seven misrouted kernel ordinals.

### v0.4.0 — *"Portable"* (May 2026)

- **Cross-platform layer with an OpenGL D3D8 backend** beside the Windows D3D11
  path, POSIX path handling, and Linux build deps. Builds with GCC/Clang.
- **`ghidra_naming` (optional)** — headless Ghidra FidDb pass recovers real
  CRT/XDK symbol names from a stripped XBE. The core pipeline still needs no
  disassembler.

### v0.3.0 — *"Fixed Function"* (March 2026)

- **Full multi-texture fixed-function pipeline** — 4-stage blending with all
  D3D8 operations and full `D3DTA` argument resolution, 4 samplers per draw.
- **Hardware T&L lighting** — up to 8 lights with materials, global ambient,
  specular, and world-space normal transform; Blinn-Phong with attenuation and
  spotlight cones.
- **Vertex fog** (linear/exp/exp2) and a **4MB DrawPrimitiveUP ring buffer**
  that removes per-call buffer create/destroy.
- **`--seed-functions`** for iterative disassembly on stripped binaries.

### v0.2.0 — *"Programmable"* (March 2026)

- **NV2A register combiner pixel shaders** — full 8-stage plus final combiner
  translated to HLSL at runtime, with a 128-entry cache.
- **NV2A programmable vertex shaders** — 128-bit microcode parser and HLSL
  generator covering all 14 MAC and 8 ILU operations, 192 constant registers,
  and relative addressing.
- **Texture unswizzling** — Xbox Z-order (Morton) to linear.
- **NV2A PGRAPH → D3D11 translator**, push buffer method interception.
- **EEPROM / AV pack / SMBus** so games can query region, language, video
  standard and hardware info.

### v0.1.0 — *"First Light"* (March 2026)

Initial public release: XBE parser, x86 disassembler and function detector,
library-function identifier, the x86 → C recompiler, and the runtime libraries
(kernel, D3D8, DirectSound, APU, NV2A, input), extracted from the Burnout 3
bring-up that started it.

## References

- [XBE File Format](https://xboxdevwiki.net/Xbe) — Xbox Dev Wiki
- [Xbox Kernel Exports](https://xboxdevwiki.net/Kernel) — Xbox Dev Wiki
- [NV2A GPU](https://xboxdevwiki.net/NV2A) — Xbox GPU documentation
- [Xbox Architecture](https://www.copetti.org/writings/consoles/xbox/) — Copetti's deep dive
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — Static recomp for N64 (MIPS→C)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — Static recomp for Xbox 360 (PPC→C)
- [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) — Xbox 360 recomp runtime (Xenia as link-time library)
- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) — Xbox emulator (dynamic recomp)
- [xemu](https://github.com/xemu-project/xemu) — Xbox emulator (LLE)
