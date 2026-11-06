# Runtime Implementation Guide

> Part of the [Documentation Index](../INDEX.md)

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

## POSIX hosts (macOS, Linux)

The template's `main.c` is Win32: a vectored handler that calls
`xbox_VehMmioFault` and a `WinMain`. A POSIX build needs the same two pieces
in its own `main()`:

```c
#include <signal.h>
#include <ucontext.h>
#include "kernel/xbox_memory_layout.h"   /* xbox_PosixMmioFault */
#include "video/video_player.h"          /* xbox_HostWindowMain */

/* Trapped hardware registers (APU, AC'97, USB OHCI) fault here. Return when
 * the access was serviced: the program counter has been advanced past it. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    if (xbox_PosixMmioFault(uc, (uintptr_t)si->si_addr))
        return;
    signal(sig, SIG_DFL);                /* a real crash: report and die */
}

static int boot_and_run(void)
{
    /* xbox_MemoryLayoutInit, kernel init, the guest entry point, ...,
     * as in the template's WinMain. */
    return 0;
}

int main(void)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);        /* macOS reports a protected page so */
    /* With RECOMP_FB_WINDOW set this opens the framebuffer window on the
     * main thread (where macOS requires it) and runs boot_and_run on a second
     * one; otherwise it just calls boot_and_run. */
    return xbox_HostWindowMain(boot_and_run);
}
```

`xbox_PosixMmioFault` decodes the faulting instruction on arm64 hosts only;
on x86-64 POSIX it always returns 0, and the traps are not armed there.
