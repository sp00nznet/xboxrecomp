# Step 5: Building the Runtime

## Overview

The lifted C code from Step 4 is just one piece of the puzzle. It expects to run in an environment that mimics the original Xbox: memory at specific addresses, kernel functions available for calling, a D3D8 graphics device, and input from Xbox controllers. The runtime provides all of this.

The runtime has four major subsystems:
1. **Memory Layout** -- map Xbox virtual addresses into Windows process space
2. **Kernel Shim** -- replace 147+ Xbox kernel functions with Win32 equivalents
3. **D3D8-to-D3D11 Bridge** -- translate Direct3D 8 calls to modern Direct3D 11
4. **Input** -- map Xbox controller input to XInput

## Memory Layout

### The Problem

Xbox games use hardcoded absolute addresses for all global variables. When the original code does `mov eax, [0x004D532C]`, it expects a specific variable at that exact address. On Windows, address space layout randomization (ASLR) and existing allocations mean address `0x004D532C` is not available by default.

### The Solution: CreateFileMapping + MapViewOfFileEx

The runtime creates a file mapping object backed by the page file and maps views of it at the exact Xbox virtual addresses:

```c
// Create a 64 MB file mapping (Xbox total RAM)
HANDLE hMapping = CreateFileMapping(
    INVALID_HANDLE_VALUE,   // page file backed
    NULL,                   // default security
    PAGE_READWRITE,         // read-write access
    0, 64 * 1024 * 1024,   // 64 MB
    NULL                    // unnamed
);

// Map the base view at address 0x00000000
void *base = MapViewOfFileEx(
    hMapping,
    FILE_MAP_ALL_ACCESS,
    0, 0,                   // offset 0
    64 * 1024 * 1024,       // 64 MB
    (void*)0x00000000       // requested base address
);
```

### Why Not VirtualAlloc?

An earlier approach used `VirtualAlloc` to reserve memory at the target addresses. This works for the basic case, but fails for **memory mirrors**. The Xbox has 64 MB of physical RAM that is identity-mapped, but some games access it through mirrored addresses (e.g., `0x80000000 + offset` is the same physical memory as `0x00000000 + offset`).

With `VirtualAlloc`, two allocations at different addresses are completely independent -- writing to one does not affect the other. With `CreateFileMapping`, two `MapViewOfFileEx` calls on the same mapping object create true aliases: writes to one view are instantly visible through the other, because they share the same physical pages.

### Mirror Views

The Xbox maps its 64 MB of RAM at multiple address ranges. The runtime pre-maps 28 mirror views:

```c
#define XBOX_NUM_MIRRORS 28

for (int i = 1; i <= XBOX_NUM_MIRRORS; i++) {
    uintptr_t mirror_addr = (uintptr_t)i * 64 * 1024 * 1024;
    g_mirror_views[i-1] = MapViewOfFileEx(
        hMapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        64 * 1024 * 1024,
        (void*)mirror_addr
    );
    // Some mirrors will fail (address already in use) -- that's OK
}
```

Not all mirrors succeed -- Windows may have DLLs or other allocations at some addresses. For Burnout 3, 4 out of 33 attempted mirrors fail due to address conflicts. This is acceptable because the game primarily accesses RAM through the base view.

### Section Loading

After mapping, the runtime copies XBE section data to the correct addresses:

```c
// Copy .rdata (read-only data: strings, vtables, constants)
memcpy((void*)XBOX_RDATA_VA, xbe_data + RDATA_RAW_OFFSET, XBOX_RDATA_SIZE);

// Copy initialized .data (global variables with initial values)
memcpy((void*)XBOX_DATA_VA, xbe_data + DATA_RAW_OFFSET, XBOX_DATA_INIT_SIZE);

// Zero-fill BSS (uninitialized globals)
memset((void*)(XBOX_DATA_VA + XBOX_DATA_INIT_SIZE), 0,
       XBOX_DATA_SIZE - XBOX_DATA_INIT_SIZE);

// Copy XDK library data sections
for (int i = 0; i < NUM_EXTRA_SECTIONS; i++) {
    memcpy((void*)g_extra_sections[i].va,
           xbe_data + g_extra_sections[i].raw_offset,
           g_extra_sections[i].size);
}
```

The .text section is NOT copied -- the recompiled code is native Windows code in the compiled binary, not the original x86 instructions. Only data sections need to be at their original addresses.

### NV2A GPU Registers

The Xbox GPU (NV2A) has memory-mapped registers starting at `0xFD000000`. Some game code spin-waits on GPU status registers:

```asm
loop:
    mov eax, [0xFD400700]    ; read GPU status register
    test eax, 0x100
    jnz loop                 ; wait until bit clears
```

These registers don't exist on Windows. A Vectored Exception Handler (VEH) catches access violations in this range and maps a page on demand with zeroed values:

```c
LONG WINAPI gpu_veh_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t addr = info->ExceptionRecord->ExceptionInformation[1];
        if (addr >= 0xFD000000 && addr < 0xFF000000) {
            // Map a zero page at this address
            VirtualAlloc((void*)(addr & ~0xFFF), 4096,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
```

This makes GPU spin-waits return immediately (reading 0), which is correct since we're not using the NV2A GPU.

### Heap Allocator

The runtime provides a bump allocator for Xbox heap allocations (MmAllocateContiguousMemory, etc.):

```c
#define XBOX_HEAP_BASE  0x00880000   // above stack
#define XBOX_HEAP_SIZE  (~55.5 MB)   // fills remaining 64 MB

static uint32_t heap_cursor = XBOX_HEAP_BASE;

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment) {
    heap_cursor = (heap_cursor + alignment - 1) & ~(alignment - 1);
    uint32_t result = heap_cursor;
    heap_cursor += size;
    if (heap_cursor >= XBOX_HEAP_BASE + XBOX_HEAP_SIZE)
        return 0;  // out of memory
    return result;
}
```

A bump allocator is sufficient because Xbox games rarely free memory -- they allocate during loading and hold everything for the level's duration.

## Kernel Shim

The Xbox kernel provides 366 possible exports. Games import a subset. Burnout 3 uses 147.

### Architecture

Each kernel function needs a **bridge** that:
1. Reads arguments from the simulated Xbox stack (g_esp)
2. Translates Xbox pointers to native pointers (adding g_xbox_mem_offset)
3. Calls the Win32 equivalent
4. Writes the return value to g_eax
5. Handles calling convention differences (Xbox kernel uses stdcall)

### Synthetic VA Scheme

Kernel thunk entries in the XBE contain ordinals (`0x80000000 | ordinal`). At initialization, the runtime replaces each with a synthetic VA:

```c
#define KERNEL_VA_BASE  0xFE000000

// For each imported ordinal:
MEM32(thunk_table_addr + slot * 4) = KERNEL_VA_BASE + slot * 4;
```

When `RECOMP_ICALL` encounters a VA in `[0xFE000000, 0xFE000600)`, it routes to `recomp_lookup_kernel()` which dispatches to the per-ordinal bridge function.

### Bridge Function Example

```c
// Ordinal 116: MmAllocateContiguousMemory
static void bridge_MmAllocateContiguousMemory(void) {
    // stdcall: args on stack, callee cleans
    uint32_t size = MEM32(g_esp + 4);     // arg 1: size
    // (arg 2: alignment - ignored for bump allocator)

    uint32_t result = xbox_HeapAlloc(size, 16);

    g_eax = result;  // return value in eax
    g_esp += 4;      // pop return address
    g_esp += 8;      // pop 2 args (stdcall)
}
```

### Kernel Function Categories

| Category | Count | Approach |
|----------|-------|----------|
| Memory (MmAlloc, NtAllocateVirtualMemory, ...) | 12 | Bump allocator |
| File I/O (NtCreateFile, NtReadFile, NtClose, ...) | 8 | Win32 CreateFile/ReadFile |
| Threading (PsCreateSystemThreadEx, KeWaitFor...) | 15 | Mostly stubbed (single-threaded) |
| Synchronization (RtlEnterCriticalSection, ...) | 10 | Win32 CriticalSection |
| String/RTL (RtlInitAnsiString, RtlCompare...) | 20 | Direct reimplementation |
| AV/Video (AvSetDisplayMode, ...) | 5 | Stubbed (using our own D3D) |
| Hardware (HalReadSMBus, HalReturnToFirmware, ...) | 8 | Stubbed |
| Network/Xbox Live (XNetStartup, ...) | 15 | Stubbed |
| Data exports (KeTickCount, HardwareInfo, ...) | 12 | Static data in kernel data area |
| Other | 42 | Case-by-case |

For Burnout 3, approximately 68 kernel functions are bridged to actual Win32 implementations, and 79 are stubbed (return success without doing anything).

### Data Exports

Some kernel ordinals are data exports, not functions. The thunk entry must point to actual readable memory:

```c
// Ordinal 156: KeTickCount - pointer to tick counter
#define KDATA_TICK_COUNT  (XBOX_KERNEL_DATA_BASE + 0x020)
MEM32(thunk_slot_156) = KDATA_TICK_COUNT;

// In the update loop:
MEM32(KDATA_TICK_COUNT) = GetTickCount();
```

The kernel data area is a 4 KB block at `0x00740000` that holds all data exports.

## D3D8-to-D3D11 Bridge

### The Problem

Xbox games use Direct3D 8, which doesn't exist on modern Windows (D3D8 was removed from the Windows SDK years ago). Even if it did, Xbox D3D8 has extensions (push buffers, NV2A-specific features) that PC D3D8 doesn't support.

### COM Vtable Emulation

Xbox D3D8 objects (Device, VertexBuffer, IndexBuffer, Texture, Surface) are COM objects with vtables. Game code calls methods through vtable dispatch:

```asm
mov eax, [esi]           ; load vtable pointer
call [eax + 0x88]        ; Device->SetRenderState (vtable slot 34)
```

The bridge creates fake COM objects with vtables that point to our D3D11 wrapper functions:

```c
typedef struct {
    void **vtable;                     // points to d3d8_device_vtable[]
    ID3D11Device *d3d11_device;
    ID3D11DeviceContext *d3d11_context;
    IDXGISwapChain *swap_chain;
    // ... state tracking ...
} D3D8Device;

static void *d3d8_device_vtable[] = {
    /* 0x00 */ d3d8_QueryInterface,
    /* 0x04 */ d3d8_AddRef,
    /* 0x08 */ d3d8_Release,
    /* ... */
    /* 0x88 */ d3d8_SetRenderState,
    /* ... ~100 methods ... */
};
```

### Fixed-Function to Programmable Pipeline

D3D8 uses a fixed-function pipeline (SetTextureStageState, SetTransform, etc.). D3D11 requires programmable shaders. The bridge translates:

- **Vertex transforms**: the bridge captures the World/View/Projection matrices from `SetTransform` calls and passes them to an HLSL vertex shader as constant buffers
- **Texture stage states**: COLOROP, COLORARG1/2, ALPHAOP translate to pixel shader state
- **Lighting**: fixed-function lighting parameters become shader constants
- **Fog**: fog mode, start/end, color become pixel shader parameters

A small set of HLSL shaders handles the common cases:
- Vertex-color only (no texture)
- Single texture with vertex colors
- Multi-texture blending (2 texture stages)

### Vertex and Index Buffers

The bridge implements the Lock/Unlock staging pattern:

```c
HRESULT d3d8_VertexBuffer_Lock(/* ... */) {
    // Create a CPU-accessible staging buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(staging_buffer, 0, D3D11_MAP_WRITE, 0, &mapped);
    *ppData = mapped.pData;
    return D3D_OK;
}

HRESULT d3d8_VertexBuffer_Unlock(/* ... */) {
    context->Unmap(staging_buffer, 0);
    // Copy staging to GPU buffer
    context->CopyResource(gpu_buffer, staging_buffer);
    return D3D_OK;
}
```

### Texture Formats

Xbox textures use DXT compression (DXT1, DXT3, DXT5), which D3D11 supports natively as BC1/BC2/BC3. The bridge maps D3D8 format constants to DXGI format constants:

| D3D8 (Xbox) | DXGI (D3D11) |
|-------------|-------------|
| D3DFMT_DXT1 | DXGI_FORMAT_BC1_UNORM |
| D3DFMT_DXT3 | DXGI_FORMAT_BC2_UNORM |
| D3DFMT_DXT5 | DXGI_FORMAT_BC3_UNORM |
| D3DFMT_A8R8G8B8 | DXGI_FORMAT_B8G8R8A8_UNORM |
| D3DFMT_X8R8G8B8 | DXGI_FORMAT_B8G8R8X8_UNORM |
| D3DFMT_R5G6B5 | DXGI_FORMAT_B5G6R5_UNORM |

Xbox textures may also be **swizzled** (Morton/Z-order layout instead of linear). The bridge detects swizzled textures and unswizzles them during upload.

## Input System

### Xbox Controller Mapping

The Xbox controller is accessed through the XPP (Xbox Peripheral Port) library. Game code reads controller state from memory-mapped addresses. The runtime intercepts these reads and provides XInput data:

```c
// Map Xbox gamepad state to XInput
XINPUT_STATE xi_state;
XInputGetState(0, &xi_state);

// Write to Xbox controller memory addresses
MEM16(XBOX_GAMEPAD_BUTTONS) = xi_state.Gamepad.wButtons;
MEM8(XBOX_GAMEPAD_LEFT_TRIGGER) = xi_state.Gamepad.bLeftTrigger;
MEM8(XBOX_GAMEPAD_RIGHT_TRIGGER) = xi_state.Gamepad.bRightTrigger;
MEM16(XBOX_GAMEPAD_THUMB_LX) = xi_state.Gamepad.sThumbLX;
MEM16(XBOX_GAMEPAD_THUMB_LY) = xi_state.Gamepad.sThumbLY;
```

Keyboard input is also mapped for testing: WASD for movement, Shift for boost, etc.

## Game-Specific: Manual Function Overrides

Not all translated functions work correctly. The runtime supports **manual overrides** -- hand-written C functions that replace specific translated functions:

```c
// recomp_manual.c
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) {
    switch (xbox_va) {
        case 0x000636D0: return manual_sub_000636D0;  // physics
        case 0x000110E0: return manual_sub_000110E0;  // frame pump
        case 0x00011240: return manual_sub_00011240;  // resource loader
        // ... 30+ overrides ...
        default: return NULL;
    }
}
```

Manual overrides are the primary debugging tool. When a function crashes or produces wrong results, you replace it with a hand-written version that either:
- **Stubs it** (returns immediately, optionally returning a success code)
- **Fixes it** (reimplements the logic with correct Win32 calls)
- **Instruments it** (adds logging/traces to understand what it does)

For Burnout 3, approximately 33 functions have manual overrides, including the physics engine, frame pump, resource loader, and several RenderWare initialization functions.

## Build System

The entire project is built with CMake:

```bash
cmake -S . -B build
cmake --build build --config Release
```

The output is a single executable (`bin/burnout3.exe`) that links:
- The auto-generated recompiled code (static library)
- The manual overrides
- The kernel shim (static library)
- The D3D8-to-D3D11 bridge (static library)
- The audio stubs (static library)
- The input layer (static library)
- The main scaffold (entry point, window creation, game loop)

System dependencies: `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `xinput.lib`, `ws2_32.lib`.

## Startup Sequence

1. Load the XBE file from disk (keep in memory for .rdata string fallback)
2. Call `xbox_MemoryLayoutInit()` to map sections to Xbox VAs
3. Call `xbox_kernel_init()` to fill the kernel thunk table
4. Call `xbox_kernel_bridge_init()` to wire synthetic VAs
5. Initialize g_esp to XBOX_STACK_TOP
6. Create the window and D3D11 device
7. Call the decoded entry point function: `sub_001D2807()`
8. Enter the Windows message loop, calling the game's frame function each tick
