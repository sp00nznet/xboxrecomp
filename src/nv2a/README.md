# xbox_nv2a — NV2A GPU Emulation

Hardware emulation of the Xbox's NV2A GPU (a custom NVIDIA chip derived from GeForce 3/4), extracted from [xemu](https://github.com/xemu-project/xemu). Provides register-level GPU emulation including the push buffer command processor, PGRAPH method dispatch, and MMIO interception.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `nv2a_state.h` | 233 | GPU state struct (PMC, PFIFO, PGRAPH, PCRTC, PRAMDAC, VRAM, RAMIN) |
| `nv2a_regs.h` | 1,522 | Complete register definitions (1500+ defines for all GPU blocks) |
| `qemu_shim.h` | 397 | QEMU → Win32 type abstraction (hwaddr, MemoryRegion, threading, bit ops) |
| `nv2a_core.c` | 791 | Register read/write handlers for all GPU blocks + pgraph_method() |
| `nv2a_mmio_hook.c` | 412 | VEH x86-64 instruction decoder for 0xFD000000+ MMIO |
| `nv2a_mmio_hook.h` | 54 | MMIO hook public API |
| `nv2a_pb_test.c` | 352 | Push buffer test harness (generates and validates GPU frames) |

## Quick Start

```c
#include "nv2a_state.h"
#include "nv2a_mmio_hook.h"

// Allocate VRAM (64 MB) and RAMIN (1 MB)
uint8_t *vram = VirtualAlloc(NULL, 64*1024*1024, MEM_COMMIT, PAGE_READWRITE);
uint8_t *ramin = VirtualAlloc(NULL, 1*1024*1024, MEM_COMMIT, PAGE_READWRITE);

// Initialize NV2A
NV2AState *gpu = nv2a_init_standalone(vram, 64*1024*1024, ramin, 1*1024*1024);

// Install VEH hooks (intercepts 0xFD000000+ memory access)
nv2a_hook_init(g_xbox_mem_offset);

// Now recompiled game code can access GPU registers normally:
//   *(uint32_t*)(0xFD000000 + NV_PMC_BOOT_0)  → intercepted → pmc_read()
//   *(uint32_t*)(0xFD100000 + NV_PBUS_PCI_NV_0) → intercepted → pbus_read()

// Direct API access (bypassing MMIO):
uint64_t boot_id = nv2a_mmio_read(gpu, NV_PMC_BOOT_0, 4);
nv2a_mmio_write(gpu, NV_PGRAPH_INTR, 0, 4);  // Clear interrupts
```

## Architecture

The NV2A is organized into functional blocks, each with its own register space:

```
0xFD000000  ┌─────────────┐
            │     PMC     │  Power Management Controller
0xFD001000  ├─────────────┤
            │    PBUS     │  Bus Interface
0xFD002000  ├─────────────┤
            │   PFIFO     │  Command FIFO Engine
0xFD009000  ├─────────────┤
            │   PTIMER    │  Timer / Performance Counters
0xFD100000  ├─────────────┤
            │     PFB     │  Frame Buffer Controller
0xFD400000  ├─────────────┤
            │   PGRAPH    │  2D/3D Graphics Engine
0xFD600000  ├─────────────┤
            │    PCRTC    │  CRT Controller
0xFD680000  ├─────────────┤
            │  PRAMDAC    │  RAM DAC (clock synthesis, display timing)
0xFD700000  ├─────────────┤
            │    PVIDEO   │  Video Overlay (stub)
0xFD800000  ├─────────────┤
            │     ...     │  Additional blocks
            └─────────────┘
```

## GPU Register Blocks

### PMC (Power Management Controller)
```c
NV_PMC_BOOT_0           // 0x000 - GPU identification (returns 0x02A000A1)
NV_PMC_INTR_0           // 0x100 - Pending interrupts (per-block bits)
NV_PMC_INTR_EN_0        // 0x140 - Interrupt enable mask
```

### PBUS (Bus Interface)
```c
NV_PBUS_PCI_NV_0        // 0x000 - PCI device/vendor ID
NV_PBUS_PCI_NV_1        // 0x004 - PCI command/status
NV_PBUS_PCI_NV_2        // 0x008 - PCI revision/class
```

### PFIFO (Command FIFO)
```c
NV_PFIFO_CACHE1_PUSH0   // Push buffer control
NV_PFIFO_CACHE1_PUSH1   // Push buffer channel/mode
NV_PFIFO_CACHE1_DMA_PUT // DMA put pointer (game writes here)
NV_PFIFO_CACHE1_DMA_GET // DMA get pointer (GPU reads here)
```

### PTIMER (Timer)
```c
NV_PTIMER_INTR_0        // Timer interrupt status
NV_PTIMER_NUMERATOR     // Clock numerator
NV_PTIMER_DENOMINATOR   // Clock denominator
NV_PTIMER_TIME_0        // Current time (low 32 bits)
NV_PTIMER_TIME_1        // Current time (high 32 bits)
```

### PGRAPH (Graphics Engine)

The main 3D rendering engine. Hundreds of registers controlling:

- **Vertex processing**: Transform program, viewport, clip planes
- **Primitive assembly**: Begin/end, triangle lists/strips/fans
- **Rasterization**: Viewport, scissor, point/line parameters
- **Texturing**: 4 texture stages, combiners, filtering
- **Pixel processing**: Register combiners, alpha test, depth test
- **Frame buffer**: Surface format, color/depth offsets, clear

Key registers:
```c
NV_PGRAPH_INTR              // Interrupt status
NV_PGRAPH_INTR_EN           // Interrupt enable

// NV097_* are Kelvin (3D) class methods, offset from PGRAPH base:
NV097_SET_BEGIN_END          // 0x17FC - Start/end primitive
NV097_INLINE_ARRAY           // 0x1818 - Inline vertex data
NV097_SET_SURFACE_FORMAT     // 0x0208 - Render target format
NV097_CLEAR_SURFACE          // 0x01D0 - Clear color/depth
NV097_SET_TRANSFORM_CONSTANT // 0x0B80 - Upload transform matrices
NV097_SET_VIEWPORT_OFFSET    // 0x0A20 - Viewport transform
NV097_SET_VIEWPORT_SCALE     // 0x0AF0 - Viewport scale
```

### PCRTC (CRT Controller)
```c
NV_PCRTC_INTR_0          // VBlank interrupt
NV_PCRTC_INTR_EN_0       // VBlank interrupt enable
NV_PCRTC_START            // Frame buffer start address
NV_PCRTC_RASTER           // Current raster line
```

### PRAMDAC (RAM DAC)
```c
NV_PRAMDAC_GENERAL_CONTROL      // Display control
NV_PRAMDAC_NVPLL_COEFF          // Core clock PLL
NV_PRAMDAC_MPLL_COEFF           // Memory clock PLL
NV_PRAMDAC_FP_VDISPLAY_END      // Display timing
```

## MMIO Hook

The VEH hook intercepts all memory accesses to the GPU register range:

```c
// Install the hook
nv2a_hook_init(g_xbox_mem_offset);

// When game code does:
//   uint32_t val = *(uint32_t*)(0xFD000100);  // Read NV_PMC_INTR_0
//
// The VEH handler:
// 1. Catches EXCEPTION_ACCESS_VIOLATION at 0xFD000100
// 2. Decodes the x86-64 instruction (MOV, MOVZX, ADD, XOR, etc.)
// 3. Calls pmc_read(nv2a_state, 0x100, 4)
// 4. Writes result to the destination register
// 5. Advances RIP past the instruction
```

Supported instructions: MOV (all sizes), MOVZX, MOVSX, ADD, SUB, OR, AND, XOR, CMP, TEST.

```c
// Hook API
void nv2a_hook_init(ptrdiff_t xbox_mem_offset);
bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write);
bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va);
```

## Push Buffer

Xbox games submit GPU commands via a push buffer — a ring buffer in main RAM that the GPU reads. The flow:

```
Game code → writes commands to push buffer (PB_BASE + offset)
         → updates DMA_PUT pointer
         → GPU reads commands from DMA_GET to DMA_PUT
         → Each command: method address + parameter
         → pgraph_method() dispatches to register handler
```

### Push Buffer Command Format

```
31          18 17     13 12          2 1  0
┌─────────────┬────────┬──────────────┬────┐
│    count    │subchan │   method     │mode│
└─────────────┴────────┴──────────────┴────┘

mode 0: incrementing methods (method, method+4, method+8, ...)
mode 1: non-incrementing (same method repeated)
```

### pgraph_method()

The central dispatch function. Receives method address + parameter and updates GPU state:

```c
void pgraph_method(NV2AState *d, uint32_t subchannel, uint32_t method, uint32_t param);

// Handles all NV097_* methods:
// - SET_SURFACE_FORMAT: configures render target
// - SET_BEGIN_END: starts/ends primitive batch
// - INLINE_ARRAY: receives vertex data
// - CLEAR_SURFACE: clears color/depth buffers
// - SET_TRANSFORM_CONSTANT: uploads matrices
// - SET_FLIP_WRITE/FLIP_STALL: page flip
```

## Push Buffer Test

`nv2a_pb_test.c` validates the pipeline by generating a complete frame:

```c
// Generates 80 dwords of GPU commands:
// - Viewport setup (640x480)
// - Surface format (A8R8G8B8 color + D24S8 depth)
// - Clear to cornflower blue
// - Draw 5 triangles with inline vertex data
// - Page flip

// Toggle at runtime with G key (in game projects)
nv2a_pb_test_frame();
```

To customize the push buffer addresses for your game:

```c
// Define BEFORE including nv2a_pb_test.c or in your CMakeLists:
#define PB_BASE_ADDR   0x35D69C  // Your game's PB base pointer address
#define PB_WRITE_ADDR  0x35D6A0  // Your game's PB write pointer address
#define PB_END_ADDR    0x35D6A4  // Your game's PB end pointer address
```

## QEMU Shim Layer

`qemu_shim.h` replaces QEMU types with Win32/C11 equivalents so the xemu-extracted code compiles standalone:

| QEMU Type | Replacement | Notes |
|-----------|-------------|-------|
| `hwaddr` | `uint64_t` | Hardware address |
| `MemoryRegion` | Struct with size + opaque | MMIO region stub |
| `PCIDevice` | Struct with irq + bus | PCI device stub |
| `QemuMutex` | `CRITICAL_SECTION` | Win32 mutex |
| `QemuCond` | `CONDITION_VARIABLE` | Win32 condition variable |
| `QemuThread` | `HANDLE` | Win32 thread |
| `qemu_irq` | `void*` | Interrupt stub |

Helper macros:
```c
muldiv64(a, b, c)    // 128-bit multiply-divide (uses _umul128 on MSVC)
ctz32(val)            // Count trailing zeros
GET_MASK(v, mask)     // Extract masked field
SET_MASK(v, mask, val)// Set masked field
```

## Current Limitations

- **PGRAPH is register-level only** — no actual 3D rendering. Push buffer methods update register state and track frame statistics, but don't produce pixels.
- **PFIFO is simplified** — no hardware channel switching. Commands are processed synchronously.
- **No shader execution** — NV2A vertex/pixel programs are not emulated.
- **No texture fetch** — Texture state is tracked but not sampled.

For actual rendering, use `xbox_d3d8` (the D3D8→D3D11 layer) or build a rendering backend that reads PGRAPH state and issues D3D11 draw calls.
