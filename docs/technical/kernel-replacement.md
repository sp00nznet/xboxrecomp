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
| PsCreateSystemThreadEx | Host thread, created suspended; see *Threads* below |
| KeSetEvent / KeResetEvent / KeWaitForSingleObject | Win32 events and waits |
| KeInitializeDpc / KeInsertQueueDpc | Queued, run on the kernel timer thread at DISPATCH_LEVEL |
| RtlEnterCriticalSection / RtlLeaveCriticalSection | Host `CRITICAL_SECTION`, shadowed per guest structure |
| KeSetBasePriorityThread / KeSetPriorityThread | Host thread priority, through the thread's KTHREAD |
| NtSuspendThread / NtResumeThread | Host suspend, only at a safe point |

Guest threads are real host threads, and the console's rules about them are
not free on a many-core host. The section *Threads, Priorities and IRQL* below
is what they cost to reproduce.

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

That holds when nothing executes the pushbuffer. A title that links the XDK's own D3D needs the pushbuffer executed, and there the better fix is to answer the register the title polls — see [Pushbuffer Executor](pushbuffer-executor.md).

### Input (~3 ordinals)

| Xbox Function | Win32 Equivalent |
|---------------|-----------------|
| XInputGetState | XInputGetState (almost identical API) |
| XInputGetCapabilities | XInputGetCapabilities |
| XInputSetState | XInputSetState (rumble) |

Xbox input is nearly 1:1 with the Win32 XInput API. The main difference is the Xbox controller struct layout.

### Titles That Drive USB Themselves

The XInput mapping above is for titles whose input goes through the D3D8/XAPI
layer the toolkit replaces. A title that statically links the XDK's own USB
stack talks to an OHCI host controller instead, and `src/usb/` models one with
an emulated Xbox pad. Two things stood between the model and a controller:

1. **Nothing starts it.** `xbox_OhciInit()` must run after the memory layout is
   up, and the title's VEH must route faults in the controller's range to
   `xbox_OhciHandleMmio()`. `ohci.h` says the title's VEH does this; the
   template's `veh_handler` only knows the APU, so with `RECOMP_USB=1` the log
   has no `[OHCI0]` lines at all. Add both next to the APU's:

   ```c
   if (xbox_OhciOwnsAddress(xbox_va) && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
       return EXCEPTION_CONTINUE_EXECUTION;
   ...
   xbox_OhciInit();          /* no-op unless RECOMP_USB is set */
   ```

2. **Descriptors are physical addresses.** The driver links its endpoint and
   transfer descriptors with `MmGetPhysicalAddress`, and the model read them as
   guest VAs: all-zero descriptors under `RECOMP_USB_TRACE=1`, and the frame
   counter and done-queue head written into the title's own `.data` every
   frame. The model now resolves them through the contiguous arena — see
   [Memory Layout: The Contiguous Arena](memory-layout.md#the-contiguous-arena).

Two OHCI 1.0a rules the model also broke: a short IN packet on a TD without
`bufferRounding` is DATA UNDERRUN (code 9) and halts the endpoint, where the
model reported success and XAPI walked into the dummy tail TD; and the done
queue goes to `HccaDoneHead` only while WDH is clear, where the model
overwrote it whenever anything completed, replacing lists the driver was
walking. With those fixed, enumeration runs through SET_ADDRESS,
GET_DESCRIPTOR, SET_CONFIGURATION and the XID descriptor. XAPI's done-queue
handler then touches a descriptor it has already freed, so the path is not
finished.

For most titles, replacing XAPI's seven input functions (`XInitDevices`,
`XGetDevices`, `XGetDeviceChanges`, `XInputOpen`, `XInputGetState`,
`XInputSetState`, `XInputClose`) with host code in `recomp_manual.c` is the more
robust route: the USB stack never starts, and any host pad works. Emulated USB
matters for titles that talk to unusual devices directly.

### Audio

A title that links the XDK's DirectSound drives the MCPX APU directly, through
its registers and DMA, and the toolkit emulates the APU (`src/apu/`). That path
needs the device interrupt and real IRQL described below; the rest of what it
took is in [APU Audio](apu-audio.md).

### HAL and System (~20 ordinals)

| Xbox Function | Implementation |
|---------------|---------------|
| HalReadSMCTrayState | Returns "disc present" |
| HalReadWritePCISpace | Returns 0 (no PCI) |
| KeBugCheck / KeBugCheckEx | Logs and continues (does not crash) |
| KeQueryPerformanceCounter | QueryPerformanceCounter |
| KeQueryPerformanceFrequency | QueryPerformanceFrequency |
| RtlEnterCriticalSection | Host `CRITICAL_SECTION` (see Threading) |

## Threads, Priorities and IRQL

The Xbox has one CPU, strict priorities and a kernel that only preempts where
it is safe. Titles depend on all three without saying so. A host with many
cores and its own scheduler breaks each one in its own way.

### Threads start when their creator lets them

On one CPU a new thread does not run until its creator blocks or yields, so
titles create a thread and *then* store its handle where the thread will read
it. A host thread that starts at once on another core reads the handle first —
in *X-Men Legends* it then suspended a reused handle's thread forever.
`PsCreateSystemThreadEx` therefore creates the host thread suspended:

- `CreateSuspended` (argument 8) is honoured; such a thread waits for
  `NtResumeThread`. It used to start anyway.
- Any other new thread starts at its creator's next kernel call, or after 5 ms
  from the timer thread if the creator makes none.
- The thread's KTHREAD exists from creation. It was registered on first run, so
  `ObReferenceObjectByHandle` on a fresh thread polled 200 × 1 ms for it — a
  2–3 s stall every time a title set a new thread's priority.

### KTHREAD and priorities

The bridge gave every thread the KTHREAD pointer `0`: `KeGetCurrentThread`
returned it, `ObReferenceObjectByHandle` wrote it, and
`KeSetBasePriorityThread` passed the guest pointer to Windows as a `HANDLE` and
failed. Every priority change was silently lost. Now each guest thread's
KTHREAD is its own TIB address — real guest memory, unique per thread, so a
stray read of a KTHREAD field is harmless — and a table maps it to a duplicated
host handle. `KeSetPriorityThread` maps absolute priorities 0–31 onto Win32
levels (16 and above → time critical). The first 16 calls are logged; a
`-- unknown thread` suffix means a KTHREAD was not in the table.

Priorities restore ordering, not exclusion. Threads still run on different
cores at once, so a title that relies on "the high-priority thread finishes its
step before anything else runs" is still exposed. Pinning every guest thread to
one core was tried and rejected: with real priorities, above-normal sound
threads spinning on critical sections starved the main thread.

### Suspend only at a safe point

The Xbox suspends a thread with an APC, delivered below DISPATCH_LEVEL — never
while it holds a kernel lock. A host `SuspendThread` can stop a guest thread
inside a bridge call holding a bridge lock, the CRT's stderr lock or the heap
lock, or at raised IRQL holding the dispatch lock, and then every thread that
needs that lock stops too. Middleware schedulers suspend and resume
constantly, so this showed up as intermittent whole-game freezes during movies.

Each thread now counts how deep it is in the kernel (`kernel_thunk_dispatch`)
plus whether it is at raised IRQL; host waits and sleeps step out of the count.
`NtSuspendThread` suspends, waits until the suspension has taken effect
(`GetThreadContext`), and if the thread is busy resumes it, yields and tries
again, giving up after 100 ms.

### Device interrupts and IRQL

`KfRaiseIrql` and `KeRaiseIrqlToDpcLevel` used to record a number and nothing
else. On the console, code at DISPATCH_LEVEL or above cannot be interrupted by
a DPC or an ISR; here ISRs and DPCs run on the kernel timer thread, in parallel
with the title, so a driver's protected section was torn by its own interrupt
handler. Raising to DISPATCH_LEVEL or above now takes one process-wide dispatch
lock (`kernel_hal.c`), and the timer thread raises to DISPATCH before any ISR or
DPC. `KeRaiseIrqlToSynchLevel`, `KeGetCurrentIrql` and `KeSynchronizeExecution`
follow the same rule.

Device models raise interrupts with `xbox_set_irq_line(vector, level)`
(`xbox_devbus.c`). Lines are level-triggered: the timer thread wakes at once
and calls the connected ISR, and keeps calling it each tick while the line is
up. The APU is vector 5. `irq 5 -> ISR claimed it` at boot means the APU's ISR
runs.

The vertical-blank interrupt is off unless `RECOMP_VBLANK` is set, and
`RECOMP_VBLANK=0` means off, so a launcher can default it on and still let a
user disable it. Turn it on for any title whose frame or movie loop calls D3D's
`BlockUntilVerticalBlank`: that waits on an event only the vblank ISR sets, and
without it the thread spins instead of waiting and starves the others.

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
    // VA -> file offset must come from the title's section table; the values
    // below are this build's .rdata for illustration only.
    size_t file_offset = (name_va - 0x36B7C0) + 0x35C000;
    const char *name = (const char *)(g_xbe_data + file_offset);

    // Open and read from game data directory
    // The game directory defaults to the current working directory's "game"
    // subfolder (see kernel_path.c); override per-title as needed.
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "game/%s", name);
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

### Pseudo-Handles Are Negative

`NtCurrentThread()` is `(HANDLE)-2`, the 32-bit `0xFFFFFFFE`. Widened as
unsigned on a 64-bit host it becomes `0x00000000FFFFFFFE`, which Windows does
not recognise as its own `-2`, so `DuplicateHandle` fails and the CRT, which
duplicates the current thread handle, retries forever:

```
[KERNEL] ordinal 197 (NtDuplicateObject) → returned 0xC0000001
[KERNEL] ordinal 301 (RtlNtStatusToDosError) → returned 0x0000013D
```

Handle tokens at `0xFFFFFFF0` and above are sign-extended in
`bridge_resolve_handle`, and the first few `NtDuplicateObject` failures are
logged with the handle and the Windows error.

### Counters Outlive 32 Bits

A title polling the clock through the kernel — *X-Men Legends* calls
`KeQuerySystemTime` hundreds of millions of times a minute — passes 2^31 kernel
calls within minutes. The call counter was an `int`; it wrapped negative, and
`KERNEL_LOG_ON()`, which is `count <= budget`, turned "log the first N calls"
into "log every call". The frame rate fell to about 1 FPS with every thread
queued on the stderr lock. The counter is `long long`.

### Declare What You Call

`apu_mmio_hook.c` called `getenv` without `<stdlib.h>`. C then assumes an `int`
return, and on x64 the pointer is cut to 32 bits — a crash only when the
variable is set, which is why it hid. MSVC says so, as warning C4013; treat
C4013 as an error in runtime code.
