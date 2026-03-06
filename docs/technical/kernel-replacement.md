# Xbox Kernel Replacement

Mapping 147 Xbox kernel imports to Windows equivalents for recompiled game code.

## How Xbox Kernel Imports Work

The Xbox kernel exposes functions through ordinal-based imports. Unlike Windows DLLs which export by name, the Xbox kernel export table uses only ordinal numbers. The game's XBE file contains a kernel thunk table -- an array of entries where each entry stores `0x80000000 | ordinal`:

```
Thunk table at VA 0x0036B7C0:
  [0] = 0x80000001  → ordinal 1  (AvGetSavedDataAddress)
  [1] = 0x80000002  → ordinal 2  (AvSendTVEncoderOption)
  [2] = 0x80000003  → ordinal 3  (AvSetDisplayMode)
  ...
  [146] = 0x80000168 → ordinal 360 (HalInitiateShutdown)
```

On real Xbox hardware, the kernel loader replaces each ordinal entry with the actual function pointer before the game runs. In our recompilation, we perform this replacement ourselves during initialization.

## Synthetic VA Scheme

Each kernel thunk slot gets a synthetic virtual address in the 0xFE000000 range:

```c
#define KERNEL_VA_BASE  0xFE000000u

// Slot i gets VA: 0xFE000000 + i*4
// Slot 0: 0xFE000000
// Slot 1: 0xFE000004
// Slot 2: 0xFE000008
// ...
```

During initialization, the ordinal entries in Xbox memory are replaced with these synthetic VAs:

```c
void xbox_kernel_bridge_init(void) {
    for (int i = 0; i < XBOX_KERNEL_THUNK_TABLE_SIZE; i++) {
        uint32_t thunk_va = XBOX_THUNK_TABLE_VA + i * 4;
        uint32_t entry = BRIDGE_MEM32(thunk_va);

        if (entry & 0x80000000) {
            uint32_t ordinal = entry & 0x7FFFFFFF;
            g_slot_ordinals[i] = ordinal;

            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                // DATA export: write the VA of the data, not a function pointer
                BRIDGE_MEM32(thunk_va) = data_va;
            } else {
                // FUNCTION export: write synthetic VA
                BRIDGE_MEM32(thunk_va) = KERNEL_VA_BASE + i * 4;
            }
        }
    }
}
```

When recompiled code does `call [0x0036B7C0]`, it reads the synthetic VA (e.g., 0xFE000000) and triggers RECOMP_ICALL. The kernel lookup recognizes the 0xFE000000 range and dispatches to the appropriate bridge function.

## Data vs Function Exports

Some kernel ordinals export data, not functions. For example:

| Ordinal | Export | Type |
|---------|--------|------|
| 17 | ExEventObjectType | Data (type object) |
| 156 | KeTickCount | Data (counter) |
| 322 | XboxHardwareInfo | Data (hardware info struct) |
| 324 | XboxKrnlVersion | Data (version struct) |

The game reads these thunk entries and **dereferences** the result to access the data:

```asm
mov eax, [0x0036B800]    ; read thunk entry for KeTickCount
mov ecx, [eax]           ; dereference to get the tick count value
```

For data exports, the thunk entry must point to actual readable memory containing the expected structure. A "kernel data area" is allocated at 0x00740000 with the necessary structures:

```c
// XboxHardwareInfo at kernel data area
BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;     // Retail
BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4)  = 0xA1;  // NV2A rev A1
BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5)  = 0xB1;  // MCPX rev B1

// XboxKrnlVersion
BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;     // Major
BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;     // Minor
BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;  // Build (XDK version)

// KeTickCount - updated every ~1ms by a background thread
BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();
```

## Per-Ordinal Bridge Functions

Each kernel function needs a bridge that reads arguments from the Xbox simulated stack, translates Xbox virtual addresses to native pointers, calls the Win32 implementation, and stores the result in g_eax.

### Stack Argument Access

After the ICALL dispatcher pops the dummy return address (`g_esp += 4`), arguments are at:

```c
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))
```

### Address Translation

Xbox pointers (32-bit VAs in Xbox memory) must be translated to native pointers:

```c
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)
```

### Example: NtCreateFile Bridge

```c
static void bridge_NtCreateFile(void) {
    uint32_t handle_ptr = STACK_ARG(0);   // PHANDLE → Xbox VA
    uint32_t desired    = STACK_ARG(1);   // ACCESS_MASK → value
    uint32_t obj_attr   = STACK_ARG(2);   // POBJECT_ATTRIBUTES → Xbox VA
    uint32_t io_status  = STACK_ARG(3);   // PIO_STATUS_BLOCK → Xbox VA
    uint32_t alloc_size = STACK_ARG(4);   // PLARGE_INTEGER → Xbox VA
    uint32_t file_attr  = STACK_ARG(5);   // ULONG → value
    uint32_t share      = STACK_ARG(6);   // ULONG → value
    uint32_t disposition = STACK_ARG(7);  // ULONG → value
    uint32_t create_opt = STACK_ARG(8);   // ULONG → value

    // Extract filename from OBJECT_ATTRIBUTES → ANSI_STRING
    // (Xbox uses ANSI, not Unicode, for kernel paths)
    char *xbox_path = extract_xbox_path(obj_attr);
    char win32_path[MAX_PATH];
    xbox_path_to_win32(xbox_path, win32_path);

    // Open via Win32
    HANDLE h = CreateFileA(win32_path, desired, share, NULL,
                           disposition_to_win32(disposition),
                           file_attr, NULL);

    // Write handle back to Xbox memory
    if (handle_ptr)
        BRIDGE_MEM32(handle_ptr) = (uint32_t)(uintptr_t)h;

    // Write IO_STATUS_BLOCK
    if (io_status) {
        BRIDGE_MEM32(io_status + 0) = 0;  // Status = SUCCESS
        BRIDGE_MEM32(io_status + 4) = 0;  // Information
    }

    g_eax = (h != INVALID_HANDLE_VALUE) ? 0 : 0xC000000F; // STATUS_NO_SUCH_FILE
}
```

## Kernel Function Categories

### Memory Management (~15 ordinals)

| Xbox Function | Implementation |
|---------------|---------------|
| MmAllocateContiguousMemory | Bump allocator (returns Xbox VAs in heap region) |
| MmAllocateContiguousMemoryEx | Bump allocator with alignment |
| MmFreeContiguousMemory | No-op (bump allocator doesn't free) |
| NtAllocateVirtualMemory | Bump allocator |
| NtFreeVirtualMemory | No-op |
| ExAllocatePool / ExAllocatePoolWithTag | Bump allocator |
| MmMapIoSpace | Returns Xbox VA for NV2A register range |

Key insight: all memory allocations go through the bump allocator because allocated memory must be in the Xbox VA space for MEM32() to work. Using VirtualAlloc directly would return native addresses that recompiled code cannot access through the MEM macros.

### File I/O (~8 ordinals)

| Xbox Function | Win32 Equivalent |
|---------------|-----------------|
| NtCreateFile | CreateFileA (with Xbox→Win32 path translation) |
| NtReadFile | ReadFile |
| NtWriteFile | WriteFile |
| NtClose | CloseHandle |
| NtQueryInformationFile | GetFileSize / GetFileInformationByHandle |
| NtQueryVolumeInformationFile | GetDiskFreeSpaceEx (for drive space queries) |

Xbox file paths use device notation (`\Device\CdRom0\`, `D:\`, `T:\`) which must be translated to Windows paths pointing at the game data directory.

### Threading (~10 ordinals)

| Xbox Function | Implementation |
|---------------|---------------|
| PsCreateSystemThreadEx | Runs start routine synchronously (single-threaded model) |
| KeSetEvent / KeResetEvent | Stubbed (no real threading) |
| KeWaitForSingleObject | Returns immediately (STATUS_SUCCESS) |
| KeInitializeDpc / KeInsertQueueDpc | Stubbed |
| KeInitializeCriticalSection | No-op |
| KeEnterCriticalSection | No-op |
| KeLeaveCriticalSection | No-op |

The recompiled game runs single-threaded. The first PsCreateSystemThreadEx call (the main game thread) runs synchronously, inheriting the current register state. Subsequent calls (worker threads) are deferred -- running them synchronously would corrupt game state.

### Graphics (~10 ordinals)

| Xbox Function | Implementation |
|---------------|---------------|
| AvSetDisplayMode | Sets resolution (stored for D3D11 init) |
| AvGetSavedDataAddress | Returns pointer to AV data area |
| D3DDevice_CreateXXX | Routed through D3D8 compatibility layer |

NV2A push buffer functions are stubbed entirely. They spin-wait on GPU registers:

```c
// Original Xbox code (simplified):
void NV2A_PushCommand(uint32_t cmd) {
    while (MEM32(0xFD003210) & 0x100)  // wait for GPU ready
        ;
    MEM32(0xFD003200) = cmd;           // write command
}
```

Allocating the NV2A register page via VEH prevents the access violation, but the while loop spins forever because the register reads as 0. The entire function must be stubbed.

### Input (~3 ordinals)

| Xbox Function | Win32 Equivalent |
|---------------|-----------------|
| XInputGetState | XInputGetState (almost identical API) |
| XInputGetCapabilities | XInputGetCapabilities |
| XInputSetState | XInputSetState (rumble) |

Xbox input is nearly 1:1 with the Win32 XInput API. The main difference is the Xbox controller struct layout.

### Audio (~5 ordinals)

| Xbox Function | Implementation |
|---------------|---------------|
| DirectSoundCreate | Returns stub object (no audio) |
| DirectSoundCreateBuffer | Returns stub buffer |
| DirectSoundDoWork | No-op |

Audio is fully stubbed in the current implementation. XAudio2 integration is planned but not yet implemented.

### HAL and System (~20 ordinals)

| Xbox Function | Implementation |
|---------------|---------------|
| HalReadSMCTrayState | Returns "disc present" |
| HalReadWritePCISpace | Returns 0 (no PCI) |
| KeBugCheck / KeBugCheckEx | Logs and continues (does not crash) |
| KeQueryPerformanceCounter | QueryPerformanceCounter |
| KeQueryPerformanceFrequency | QueryPerformanceFrequency |
| RtlEnterCriticalSection | No-op (single-threaded) |

## Implementation Breakdown

Of the 147 kernel imports in Burnout 3:

| Category | Count | Notes |
|----------|-------|-------|
| Fully bridged (real functionality) | ~68 | Memory, file I/O, timing, input |
| Stubbed (return success) | ~79 | Threading, audio, network, crypto |
| Not hit during gameplay | ~20 | Debug, online, rare error paths |

## The Resource Load Queue

One of the most important kernel interactions is file loading. The game uses a ring buffer queue to request file loads:

```c
// Queue structure: 24 entries x 80 bytes each
// Entry layout: 64B filename + 4B flag_ptr + 4B resource_ptr + 4B param + 4B status

static void bridge_resource_load(void) {
    // Read filename from Xbox .rdata (use pristine XBE copy to avoid corruption)
    uint32_t name_va = MEM32(entry_ptr + 0);
    size_t file_offset = (name_va - 0x36B7C0) + 0x35C000;
    const char *name = (const char *)(g_xbe_data + file_offset);

    // Open and read from game data directory
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "Burnout 3 Takedown/%s", name);
    FILE *f = fopen(path, "rb");

    // Write file data directly into the resource VA buffer
    uint32_t resource_va = MEM32(entry_ptr + 68);
    fread((void *)XBOX_PTR(resource_va), 1, file_size, f);
}
```

The resource VA points into Xbox heap memory. File data is loaded directly into the Xbox memory map so the game's existing code can process it via MEM macros.

## Common Gotchas

### Return Values in g_eax

All kernel bridge functions must set `g_eax` before returning. For NTSTATUS functions, 0 = STATUS_SUCCESS. For allocation functions, the return value is a pointer (Xbox VA or native, depending on the function). Forgetting to set g_eax causes the caller to see stale values from previous calls.

### Pointer vs Value Arguments

Some arguments are pointers (Xbox VAs that need translation), others are plain values. Translating a value as a pointer or vice versa causes crashes. Each bridge function must know its parameter types from the Xbox kernel documentation.

### Stdcall Stack Cleanup

Xbox kernel functions use stdcall convention (callee cleans stack). The translated code handles this -- after the ICALL returns, the generated code adjusts esp by the expected amount. The bridge function does NOT need to manipulate g_esp for argument cleanup.
