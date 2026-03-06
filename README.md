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

---

## What Is This?

This is a complete toolkit for **statically recompiling original Xbox (2001-2005) games** from their retail XBE executables into native Windows programs.

Static recompilation takes the raw x86 machine code from an Xbox binary and translates every function — every `mov`, every `jmp`, every `call` — into equivalent C source code. That C code compiles with MSVC into a native x86-64 `.exe` that runs on modern Windows. The game's original logic executes directly on your CPU, not through an interpreter or JIT compiler.

**This is the first known static recompilation project targeting the original Xbox.**

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

## Quick Start

### Prerequisites

- **Windows 11** (or 10 with recent updates)
- **Python 3.10+** with `capstone` (`pip install capstone`)
- **Visual Studio 2022** (MSVC compiler)
- **CMake 3.20+**
- An original Xbox game disc image (you must own the game)

### Step-by-Step

```bash
# 1. Clone this repo
git clone https://github.com/sp00nznet/xboxrecomp.git
cd xboxrecomp

# 2. Extract default.xbe from your Xbox disc image
#    (Use xdvdfs, extract-xiso, or similar tool)
mkdir game_files
# copy default.xbe and game data into game_files/

# 3. Parse the XBE — learn what you're working with
py -3 -m tools.xbe_parser game_files/default.xbe
#    Output: section map, kernel imports, entry point, XDK version

# 4. Disassemble — find all functions
py -3 -m tools.disasm game_files/default.xbe --text-only
#    Output: tools/disasm/output/ (functions.json, xrefs.json, strings.json)

# 5. Identify library functions
py -3 -m tools.func_id game_files/default.xbe -v
#    Output: tools/func_id/output/ (CRT, RenderWare, vtables classified)

# 6. Lift to C — the big one
py -3 -m tools.recomp game_files/default.xbe --all --split 1000
#    Output: src/game/recomp/gen/ (millions of lines of C)

# 7. Set up runtime shims (see docs/runtime/ for templates)
#    - Xbox kernel replacement (147 imports)
#    - D3D8 -> D3D11 translation layer
#    - Memory layout reproduction
#    - Input system

# 8. Build and run
cmake -S . -B build
cmake --build build --config Release
bin/your_game.exe
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
├── tools/                       # The recompilation toolchain
│   ├── xbe_parser/              # XBE file format parser
│   ├── disasm/                  # x86 disassembler + function detector
│   ├── func_id/                 # Library function identifier
│   └── recomp/                  # x86 -> C static recompiler
├── templates/                   # Starter templates for new projects
│   └── runtime/                 # Runtime shim templates
│       ├── recomp_types.h       # Register model + ICALL macros
│       ├── xbox_memory.h        # Memory layout helpers
│       └── kernel_stubs.h       # Kernel function stub templates
├── docs/                        # Documentation
│   ├── pipeline/                # Step-by-step pipeline guides
│   ├── technical/               # Deep technical documentation
│   ├── formats/                 # Xbox file format references
│   └── runtime/                 # Runtime implementation guides
└── examples/                    # Example configurations
```

## Documentation

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
- [Xbox Kernel Replacement](docs/technical/kernel-replacement.md) — Mapping 147 kernel imports to Win32
- [SEH and Exception Handling](docs/technical/seh-handling.md) — Structured exception handling in recompiled code
- [Lessons Learned](docs/technical/lessons-learned.md) — What worked, what didn't, mistakes to avoid

### Xbox Formats
- [XBE File Format](docs/formats/xbe.md) — Xbox executable format reference
- [Xbox Kernel Exports](docs/formats/kernel-exports.md) — All 366 kernel functions documented

## How The Key Pieces Work

### The Register Model

Xbox uses 32-bit x86 with 8 general-purpose registers. In recompiled code, these become C globals:

```c
// Volatile (caller-saved) — shared across all functions
uint32_t g_eax, g_ecx, g_edx, g_esp;

// Callee-saved — also global (implicit parameter passing via esi/edi/ebx)
uint32_t g_ebx, g_esi, g_edi;

// Stack lives in Xbox memory space
#define PUSH32(val)  do { g_esp -= 4; MEM32(g_esp) = (val); } while(0)
#define POP32(dst)   do { (dst) = MEM32(g_esp); g_esp += 4; } while(0)
```

Every recompiled function is `void func(void)` — arguments pass through the simulated stack and registers, just like real x86.

### Memory Layout

Xbox has 64 MB of unified RAM. We reproduce the exact address layout:

```c
// Create shared memory object
HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64*1024*1024, NULL);

// Map at Xbox virtual addresses
MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, offset, size, (LPVOID)0x00010000);  // XBE
MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, offset, size, (LPVOID)0x80000000);  // mirror
// ... 28 views total covering the Xbox address space
```

Why `CreateFileMapping` instead of `VirtualAlloc`? The Xbox has **mirror regions** — the same physical memory at multiple virtual addresses. File mapping gives us true aliases; VirtualAlloc would give separate copies where writes in one aren't visible in the other.

### The ICALL Problem

The single hardest challenge. When the game does `call [eax+0x10]` (a virtual method call), we don't know the target at compile time. Our dispatch macro handles it:

```c
#define RECOMP_ICALL(va) do {                          \
    recomp_func_t fn = recomp_lookup_manual(va);       \  // 1. hand-written overrides
    if (!fn) fn = recomp_lookup(va);                   \  // 2. auto dispatch table
    if (!fn) fn = recomp_lookup_kernel(va);            \  // 3. kernel bridge
    if (fn) fn();                                       \
    else { g_esp += 4; /* pop dummy ret addr */ }      \
} while(0)
```

Most ICALLs resolve through the auto-generated dispatch table (binary search over all function addresses). The rest are either kernel calls (0xFE000000+ range) or garbage pointers from corrupted vtables that need per-function guards.

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

- **[Burnout 3: Takedown](https://github.com/sp00nznet/burnout3)** — The first game recompiled with this toolkit. 22,097 functions lifted, game boots and renders textured 3D tracks from original assets.

## How You Can Help

This is an emerging field. Here's how you can contribute:

1. **Try it on a new game** — Pick an Xbox exclusive, follow the pipeline, and see how far you get. Even partial results teach us about the toolchain's gaps.
2. **Improve the lifter** — The x86-to-C translator handles ~95% of instructions. Edge cases (SIMD, obscure FPU ops, segment prefixes) need work.
3. **Document Xbox formats** — Every game has its own asset formats. Document what you discover.
4. **Build runtime components** — Better D3D8 emulation, audio, networking — the runtime layer is where most per-game work happens.
5. **Share your findings** — Write up what you learn. The Xbox modding/preservation community benefits from every discovery.

## Dependencies

The toolchain is intentionally lightweight:

```
Python 3.10+
capstone        # x86 disassembly (pip install capstone)
```

That's it. No IDA, no Ghidra, no proprietary tools. Standard library + Capstone.

The runtime (C/C++) uses:
- MSVC (Visual Studio 2022)
- Windows SDK (for D3D11, XInput, XAudio2)
- CMake 3.20+

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

MIT

## Credits

Built with [Claude Code](https://claude.ai) (Anthropic) — proving that AI-assisted systems programming can tackle problems previously considered impractical.

## References

- [XBE File Format](https://xboxdevwiki.net/Xbe) — Xbox Dev Wiki
- [Xbox Kernel Exports](https://xboxdevwiki.net/Kernel) — Xbox Dev Wiki
- [NV2A GPU](https://xboxdevwiki.net/NV2A) — Xbox GPU documentation
- [Xbox Architecture](https://www.copetti.org/writings/consoles/xbox/) — Copetti's deep dive
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — Static recomp for N64 (MIPS→C)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — Static recomp for Xbox 360 (PPC→C)
- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) — Xbox emulator (dynamic recomp)
- [xemu](https://github.com/xemu-project/xemu) — Xbox emulator (LLE)
