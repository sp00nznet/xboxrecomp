# Runtime Libraries

The `src/` directory contains 6 static libraries that provide the Xbox hardware and OS abstraction layer for statically recompiled games. Your recompiled game links against these at build time — no emulator needed at runtime.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                   Your Recompiled Game                    │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐ │
│  │ gen/recomp_*  │  │   manual     │  │  game-specific │ │
│  │ (auto-gen C)  │  │  overrides   │  │  loaders/fmt   │ │
│  └──────┬───────┘  └──────┬───────┘  └───────┬────────┘ │
│         └─────────┬───────┘──────────────────┘           │
│                   │                                       │
│         recomp_lookup() / recomp_lookup_manual()          │
│         (dispatch table — YOU provide these)              │
├───────────────────┼───────────────────────────────────────┤
│                   │     xboxrecomp libraries              │
│         ┌─────────┴─────────┐                             │
│         │   xbox_kernel     │  Memory, files, threads,    │
│         │  (kernel_bridge)  │  sync, crypto, HAL          │
│         └─────────┬─────────┘                             │
│                   │                                       │
│  ┌────────┐ ┌─────┴───┐ ┌──────────┐ ┌───────┐ ┌──────┐│
│  │xbox_   │ │xbox_    │ │xbox_     │ │xbox_  │ │xbox_ ││
│  │d3d8    │ │dsound   │ │apu       │ │nv2a   │ │input ││
│  │        │ │         │ │          │ │       │ │      ││
│  │D3D8→   │ │DSound→  │ │MCPX APU │ │NV2A   │ │XPP→  ││
│  │D3D11   │ │mixer    │ │(xemu)   │ │(xemu) │ │XInput││
│  └────────┘ └─────────┘ └──────────┘ └───────┘ └──────┘│
├──────────────────────────────────────────────────────────┤
│    Windows: D3D11, DXGI, XInput, waveOut, Win32 API      │
└──────────────────────────────────────────────────────────┘
```

## Libraries

| Library | Dir | LOC | Origin | Description |
|---------|-----|-----|--------|-------------|
| **xbox_kernel** | `kernel/` | 7,935 | Custom | Xbox kernel → Win32 replacement (147 imports) |
| **xbox_d3d8** | `d3d/` | 3,372 | Custom | D3D8 → D3D11 graphics compatibility layer |
| **xbox_dsound** | `audio/` | 573 | Custom | DirectSound → software mixer |
| **xbox_apu** | `apu/` | 3,918 | xemu | MCPX APU audio emulation (256 voices) |
| **xbox_nv2a** | `nv2a/` | 3,761 | xemu | NV2A GPU register handlers + MMIO |
| **xbox_input** | `input/` | 212 | Custom | Xbox gamepad → XInput mapping |

## Building

```bash
cd xboxrecomp
cmake -S . -B build
cmake --build build --config Release
```

Output: 6 `.lib` files in `build/src/*/Release/`.

## Linking to Your Game

In your game's CMakeLists.txt:

```cmake
# Point to xboxrecomp
add_subdirectory(path/to/xboxrecomp)

# Link the umbrella target (all 6 libs)
target_link_libraries(my_game PRIVATE xboxrecomp)

# Or link individual modules
target_link_libraries(my_game PRIVATE xbox_kernel xbox_d3d8)
```

## Integration Contract

Your game project **must** provide two functions that the kernel bridge calls:

```c
typedef void (*recomp_func_t)(void);

// Auto-generated dispatch table (from tools/recomp output)
recomp_func_t recomp_lookup(uint32_t xbox_va);

// Hand-written function overrides
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
```

These resolve Xbox virtual addresses to native function pointers. The recompiler tool generates `recomp_dispatch.c` with a binary-search lookup table.

## Initialization Order

```c
#include "kernel.h"
#include "xbox_memory_layout.h"

int main() {
    // 1. Map Xbox 64MB address space (sections, stack, heap, mirrors)
    xbox_MemoryLayoutInit(xbe_data, xbe_size);

    // 2. Initialize kernel thunk table (147 imports → Win32)
    xbox_kernel_init();
    xbox_kernel_bridge_init();

    // 3. Initialize graphics
    IDirect3D8 *d3d = xbox_Direct3DCreate8(0);
    // ... create device, window, etc.

    // 4. Initialize audio (optional)
    MCPXAPUState *apu = mcpx_apu_init_standalone(ram_ptr);

    // 5. Initialize GPU (optional)
    NV2AState *gpu = nv2a_init_standalone(vram, vram_size, ramin, ramin_size);
    nv2a_hook_init(g_xbox_mem_offset);

    // 6. Jump to game entry point
    void (*entry)(void) = recomp_lookup(ENTRY_POINT_VA);
    entry();
}
```

## Per-Module Documentation

Each subdirectory has its own README with API reference:

- [kernel/README.md](kernel/README.md) — Memory layout, file I/O, threading, sync, crypto
- [d3d/README.md](d3d/README.md) — D3D8 interface, render states, textures, shaders
- [audio/README.md](audio/README.md) — DirectSound buffers, 3D audio, mixbins
- [apu/README.md](apu/README.md) — MCPX APU voice processor, mixer, MMIO
- [nv2a/README.md](nv2a/README.md) — NV2A GPU registers, push buffer, PGRAPH
- [input/README.md](input/README.md) — Gamepad state, vibration, button mapping
