# Getting Started with Xbox Static Recompilation

This guide walks you through recompiling your first Xbox game, from extracting the XBE to getting the game running on Windows.

## What You Need

- **Windows 11** (or 10 with recent updates)
- **Python 3.10+** with `capstone` installed (`pip install capstone`)
- **Visual Studio 2022** (MSVC compiler) with C/C++ desktop workload
- **CMake 3.20+**
- An original Xbox game disc image (ISO/XISO) — you must own the game
- A tool to extract ISO files ([extract-xiso](https://github.com/XboxDev/extract-xiso) or [xdvdfs](https://github.com/antangelo/xdvdfs))

Optional but very helpful:
- **xemu** — Xbox emulator for live debugging via GDB stub
- **Ghidra/IDA** — for manual analysis of tricky functions
- **Your game's PC port source** (if leaked/available) — for understanding game logic

## Step 0: Set Up Your Project

```bash
# Clone the toolkit
git clone https://github.com/sp00nznet/xboxrecomp.git

# Create your game-specific repo
mkdir my_xbox_game
cd my_xbox_game
git init

# Build the runtime libraries
cd ../xboxrecomp
cmake -S . -B build
cmake --build build --config Release
cd ../my_xbox_game
```

## Step 1: Extract the XBE

Extract `default.xbe` and game data files from your disc image:

```bash
# Using extract-xiso
extract-xiso -x "My Game.iso" -d game_files/

# Or using xdvdfs
xdvdfs unpack "My Game.iso" game_files/
```

You should now have:
```
game_files/
├── default.xbe          # The game executable
├── Data/                # Game data (textures, models, levels)
├── Video/               # FMV files (XMV format)
└── ...                  # Other game-specific files
```

## Step 2: Parse the XBE

```bash
py -3 -m tools.xbe_parser game_files/default.xbe
```

This outputs:
- **Entry point** — where execution starts (e.g., `0x001D2807`)
- **Section layout** — VA, size, and raw offset for .text, .rdata, .data, and XDK lib sections
- **Kernel imports** — which of the 147 kernel functions the game uses
- **XDK version** — helps identify which XDK libraries were linked

**Write these values down.** You'll need them to configure `xbox_memory_layout.h`.

## Step 3: Disassemble

```bash
py -3 -m tools.disasm game_files/default.xbe --text-only -v
```

This typically takes 30-60 seconds for a 2-3 MB .text section. Output goes to `tools/disasm/output/`:

- `functions.json` — every detected function with address, size, instruction count
- `xrefs.json` — call graph (who calls whom)

Check the stats: you should find thousands of functions. A typical Xbox game has 10,000-25,000 functions including CRT and middleware.

## Step 4: Identify Library Functions

```bash
py -3 -m tools.func_id game_files/default.xbe -v
```

This classifies functions into categories:
- **CRT** — C runtime (malloc, free, memcpy, etc.) — usually safe to leave as-is
- **RW** — RenderWare engine (if applicable) — may need manual overrides
- **XDK** — Xbox SDK library code (D3D8, DirectSound, etc.) — often needs stubs
- **GAME** — Game-specific code — your main focus
- **STUB** — Empty/trivial functions — safe to ignore

## Step 5: Recompile

```bash
py -3 -m tools.recomp game_files/default.xbe --all --split 1000
```

This is the big one — it can take 5-15 minutes for a large game. Output:

```
src/game/recomp/gen/
├── recomp_0000.c          # Functions 0-999
├── recomp_0001.c          # Functions 1000-1999
├── ...                    # More splits
├── recomp_dispatch.c      # ICALL dispatch table (binary search)
├── recomp_funcs.h         # Forward declarations
└── recomp_stubs.c         # Stubs for unresolvable targets
```

## Step 6: Set Up Your Build

Create `CMakeLists.txt` in your game project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_xbox_game C)

set(CMAKE_C_STANDARD 11)

# Common defines
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

# xboxrecomp runtime libraries
add_subdirectory(path/to/xboxrecomp)

# Your game executable
add_executable(my_game
    src/main.c                          # Entry point, window, game loop
    src/game/recomp/recomp_manual.c     # Manual function overrides
    src/game/recomp/gen/recomp_0000.c   # Generated code
    src/game/recomp/gen/recomp_0001.c
    # ... all gen files
    src/game/recomp/gen/recomp_dispatch.c
    src/game/recomp/gen/recomp_stubs.c
)

# Suppress warnings in generated code (it's mechanical, not pretty)
target_compile_options(my_game PRIVATE /w)

# Link runtime libraries
target_link_libraries(my_game PRIVATE xboxrecomp)
```

Create `src/main.c`:

```c
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "d3d8_xbox.h"

// Generated code dispatch
extern void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

int main(int argc, char *argv[]) {
    // Load XBE into memory
    FILE *f = fopen("game_files/default.xbe", "rb");
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);
    void *xbe = malloc(size);
    fread(xbe, 1, size, f);
    fclose(f);

    // Initialize Xbox memory layout
    xbox_MemoryLayoutInit(xbe, size);

    // Initialize kernel
    xbox_kernel_init();
    xbox_path_init("game_files", "saves");
    xbox_kernel_bridge_init();

    // Initialize graphics
    xbox_Direct3DCreate8(0);

    // Jump to entry point!
    recomp_func_t entry = recomp_lookup(0x001D2807);  // YOUR entry point
    if (entry) entry();

    return 0;
}
```

## Step 7: Build and Crash

```bash
cmake -S . -B build
cmake --build build --config Release
bin\my_game.exe 2>stderr.txt
```

**It will crash.** That's expected and normal. The stderr log tells you what happened.

## Step 8: Debug Iteratively

This is where the real work begins. The general pattern:

1. **Run** — game crashes
2. **Read stderr** — look for ICALL failures, bad memory access, assertion failures
3. **Identify the problem** — usually one of:
   - Missing ICALL target (function pointer the dispatch table doesn't know about)
   - Bad memory access (pointer to Xbox memory that hasn't been mapped)
   - Unimplemented kernel function (game calls a function we stubbed)
   - Corrupted vtable (native pointer where Xbox VA expected)
4. **Fix it** — add a manual override, stub the function, fix the dispatch table
5. **Rebuild and repeat**

### Common First Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| Crash in ICALL dispatch | Unknown function pointer | Add function to dispatch table or stub it |
| Access violation at 0xFD...... | GPU MMIO access | Initialize NV2A and MMIO hooks |
| Access violation at 0xFE...... | APU MMIO access | Initialize APU and MMIO hooks |
| "SKIP-READ" in stderr | Access to unmapped memory | Check mirror views; might be native ptr confusion |
| Infinite loop | Game waiting for hardware | Stub the wait function or fake the hardware state |
| Stack overflow | Recursive calls or wrong ESP | Check stack setup, ensure ESP starts correctly |

### The ICALL Trace

The most powerful debugging tool. When an indirect call fails, the trace shows:

```
[ICALL] unknown target 0x001A3F50 from RVA 0x000165F0
```

This tells you: function at 0x000165F0 tried to call 0x001A3F50, but it's not in the dispatch table. Usually means you need to add it to `recomp_dispatch.c` or create a manual override.

### Manual Overrides

When a recompiled function doesn't work (crashes, loops forever, reads hardware), replace it:

```c
// In recomp_manual.c
void sub_001A3F50(void) {
    // The original function reads GPU registers we haven't set up.
    // For now, just return success.
    eax = 1;
    esp += 4; return;  // Clean up fake return address
}
```

Register your override in the manual lookup table so ICALLs find it.

## Step 9: Get to Menus

The typical boot sequence for an Xbox game:

1. **CRT startup** — `_mainCRTStartup` → `main()` or `WinMain()`
2. **Hardware init** — D3D device creation, DirectSound init, input setup
3. **Asset loading** — textures, models, levels from disc
4. **Menu system** — title screen, main menu
5. **Gameplay** — the actual game

Each phase introduces new challenges. Hardware init needs working kernel + D3D stubs. Asset loading needs file I/O. Menus need rendering. Gameplay needs everything.

Focus on getting past each phase one at a time.

## Step 10: Beyond Boot

Once the game boots and shows something on screen, you're past the hardest part. From here:

- **Add missing features** — audio, input, save/load
- **Fix rendering** — texture formats, shader states, blend modes
- **Optimize** — profile, find bottlenecks, add proper shader support
- **Mod** — the generated C code is yours to modify. Add HD support, widescreen, new features.

## Tips

- **Start with a simple game** — pick something small with a known engine (RenderWare games are good targets)
- **Use xemu for reference** — run the game in xemu with GDB debugging to understand what memory addresses mean
- **Keep notes** — document every address, every function you identify, every patch you make
- **Don't try to fix everything at once** — stub what you can, fix what you must
- **The 80/20 rule applies** — 80% of functions "just work" in recompiled form. The other 20% is where you spend your time.
- **Read the technical docs** — especially [indirect-calls.md](technical/indirect-calls.md) and [lessons-learned.md](technical/lessons-learned.md)
