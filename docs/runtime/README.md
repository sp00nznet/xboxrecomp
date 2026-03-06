# Runtime Implementation Guide

The recompiled C code needs a host environment to run. This directory documents the four major runtime subsystems you need to build for each game.

## The Four Pillars

### 1. Memory Layout (`xbox_memory.c`)

Reproduce the Xbox's 64 MB address space at the correct virtual addresses. This is the most critical piece — get it wrong and nothing else works.

See: [../technical/memory-layout.md](../technical/memory-layout.md)

Template: [../../templates/runtime/xbox_memory.h](../../templates/runtime/xbox_memory.h)

### 2. Kernel Shim (`xbox_kernel.c`)

Replace the 147 Xbox kernel imports with Win32 equivalents. Most can be stubbed (return STATUS_SUCCESS). The critical ones are memory allocation and file I/O.

See: [../technical/kernel-replacement.md](../technical/kernel-replacement.md)

Template: [../../templates/runtime/kernel_stubs.h](../../templates/runtime/kernel_stubs.h)

### 3. Graphics Translation (`d3d8_to_d3d11.c`)

The Xbox uses a modified Direct3D 8. You need a translation layer to D3D11 (or D3D12, Vulkan, etc.). This is the largest runtime component.

See: [../technical/d3d-translation.md](../technical/d3d-translation.md)

### 4. Input System (`xbox_input.c`)

Map Xbox controller input to Windows XInput. This is usually the simplest piece — Xbox controllers on Windows are nearly 1:1.

## Build System

We recommend CMake with MSVC. Structure your project as static libraries:

```
src/
├── kernel/     → xbox_kernel.lib
├── d3d/        → d3d_translation.lib
├── audio/      → audio_stubs.lib
├── input/      → input_layer.lib
└── game/       → your_game.exe (links all the above + recompiled code)
```

## Game-Specific Components

Beyond the four pillars, each game needs:

- **Manual function overrides** — hand-written replacements for functions that don't work when mechanically translated (physics, rendering orchestrators, hardware-specific code)
- **Asset loaders** — each game/engine has its own file formats for textures, models, levels, audio, etc.
- **Game-specific workarounds** — vtable guards for corrupted pointers, state machine patches, etc.

These go in the game-specific repo, not in the generic xboxrecomp toolkit.
