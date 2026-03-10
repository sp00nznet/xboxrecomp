# xbox_kernel — Xbox Kernel Replacement Layer

Replaces 147 Xbox kernel imports with Win32 equivalents. This is the foundation layer that every recompiled game needs — it provides memory layout, file I/O, threading, synchronization, and hardware abstraction.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `kernel.h` | 829 | Master public header — all types, constants, and function prototypes |
| `xbox_memory_layout.h` | 204 | Memory layout constants and API |
| `xbox_memory_layout.c` | 534 | Memory mapping (CreateFileMapping + 28 mirror views) |
| `kernel_bridge.c` | 1,973 | Core dispatcher — thunk table, ICALL resolution, kernel data exports |
| `kernel_file.c` | 834 | File I/O — NtCreateFile, NtReadFile, path translation |
| `kernel_thread.c` | 344 | Threading — PsCreateSystemThreadEx, thread priorities |
| `kernel_sync.c` | 547 | Synchronization — events, semaphores, mutexes, critical sections |
| `kernel_memory.c` | 323 | Memory allocation — MmAllocateContiguousMemory, page alloc |
| `kernel_rtl.c` | 334 | Runtime library — string ops, memory copy, ANSI/Unicode conversion |
| `kernel_hal.c` | 453 | Hardware abstraction — PCI, timers, DPC, IRQLs |
| `kernel_crypto.c` | 409 | Cryptography — SHA1, RC4, HMAC |
| `kernel_thunks.c` | 414 | Thunk layer for recompiled code → kernel dispatch |
| `kernel_ob.c` | 126 | Object manager — handle table, reference counting |
| `kernel_io.c` | 242 | Device I/O control |
| `kernel_path.c` | 199 | Xbox path parsing (`\Device\CdRom0\` → Windows path) |
| `kernel_pool.c` | 49 | Pool memory allocator |
| `kernel_xbox.c` | 121 | Misc Xbox APIs (XeLoadSection, etc.) |

## Memory Layout

The Xbox has 64 MB of unified RAM. We reproduce the exact address layout using `CreateFileMapping` + `MapViewOfFileEx`:

```
Xbox VA Range          Purpose                  Size
─────────────────────────────────────────────────────
0x00010000-0x00010BCC  XBE header               ~3 KB
0x00011000-0x002BBFFF  .text (code)             2.73 MB
0x0036B7C0-0x003B2363  .rdata (constants)       283 KB
0x003B2360-0x0076FFFF  .data (globals + BSS)    3.8 MB
0x00700000-0x0073FFFF  RW data copy             256 KB
0x00740000-0x0077FFFF  Kernel data exports      256 KB
0x00760000-0x0076FFFF  TLS area                 64 KB
0x00780000-0x00F7FFFF  Stack (8 MB, grows down) 8 MB
0x00F80000-0x03FFFFFF  Heap (bump allocator)    ~49 MB
```

### Why CreateFileMapping?

The Xbox has **mirror regions** — the same physical memory appears at multiple virtual addresses (26-bit address bus wrapping). `CreateFileMapping` gives us true aliases where writes through one address are instantly visible at all mirrors. `VirtualAlloc` would give separate copies.

We create 28 mirror views covering 1.75 GB of address space:

```c
// The core mapping
HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64*1024*1024, NULL);

// Primary view at Xbox base (0x10000000 on host)
MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, 0, XBOX_TOTAL_RAM, target_base);

// Mirror views every 64 MB
for (int i = 1; i < 28; i++)
    MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, 0, XBOX_TOTAL_RAM, target_base + i * 64MB);
```

### Global State

```c
// Defined in xbox_memory_layout.c, used everywhere
extern ptrdiff_t g_xbox_mem_offset;  // Add to Xbox VA → get native pointer

// Simulated x86 registers (the register model)
extern uint32_t g_eax, g_ecx, g_edx, g_esp;  // Volatile
extern uint32_t g_ebx, g_esi, g_edi;          // Callee-saved
extern uint32_t g_seh_ebp;                     // SEH frame pointer
```

### API

```c
// Initialize — call FIRST before anything else
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);
void xbox_MemoryLayoutShutdown(void);

// Query
void     *xbox_GetMemoryBase(void);    // Native pointer to Xbox VA 0
ptrdiff_t xbox_GetMemoryOffset(void);  // g_xbox_mem_offset value
BOOL      xbox_IsXboxAddress(uintptr_t address);
HANDLE    xbox_GetMappingHandle(void); // For creating additional mirror views

// Heap (bump allocator, no free)
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void     xbox_HeapFree(uint32_t xbox_va);  // No-op currently
```

### Customizing for Your Game

Edit `xbox_memory_layout.h` to match your XBE's section layout:

```c
#define XBOX_TEXT_VA       0x00011000   // From XBE section headers
#define XBOX_TEXT_SIZE     2863616      // .text section size
#define XBOX_RDATA_VA      0x0036B7C0   // .rdata section
// ... etc
```

The `xbe_parser` tool outputs these values for any XBE.

## Kernel Thunk Table

The Xbox kernel exposes functions via ordinal numbers. Games import them through a thunk table at a fixed address. We reproduce this:

```c
// The thunk table lives at the same Xbox VA as the original
#define XBOX_KERNEL_THUNK_TABLE_BASE  0x0036B7C0
#define XBOX_KERNEL_THUNK_TABLE_SIZE  147

// Each entry maps ordinal → function pointer (as synthetic VA)
extern ULONG_PTR xbox_kernel_thunk_table[147];
```

### Initialization

```c
xbox_kernel_init();         // Populate thunk table with ordinal → function mappings
xbox_kernel_bridge_init();  // Write thunk entries into Xbox memory, set up kernel data area
```

### Kernel Data Exports

Some kernel ordinals are data exports (not functions). We allocate a kernel data area:

```c
#define XBOX_KERNEL_DATA_BASE  0x00740000

// Offsets within the data area
#define KDATA_HARDWARE_INFO     0x000  // XBOX_HARDWARE_INFO struct
#define KDATA_KRNL_VERSION      0x010  // XBOX_KRNL_VERSION struct
#define KDATA_TICK_COUNT        0x020  // KeTickCount (updated by timer)
#define KDATA_HD_KEY            0x030  // XboxHDKey (16 bytes)
#define KDATA_EEPROM_KEY        0x050  // XboxEEPROMKey (16 bytes)
// ... more at documented offsets
```

## File I/O

Xbox file I/O uses NT-style APIs (NtCreateFile, NtReadFile). We translate Xbox paths to Windows:

```
Xbox Path                    → Windows Path
───────────────────────────────────────────────
\Device\CdRom0\Data\foo.dat → <game_dir>\Data\foo.dat
D:\Data\foo.dat              → <game_dir>\Data\foo.dat
T:\save.dat                  → <save_dir>\save.dat
Z:\cache.dat                 → <save_dir>\cache\cache.dat
```

### Path Setup

```c
// Call early — before game tries to open files
xbox_path_init("Burnout 3 Takedown", "saves");
```

## Threading

Xbox threads use `PsCreateSystemThreadEx` with NT-style parameters. We create Win32 threads with a wrapper that sets up the register context:

```c
// The kernel bridge creates a thread that:
// 1. Looks up the start routine via recomp_lookup()
// 2. Sets up ESP for the new thread
// 3. Calls the recompiled function
NTSTATUS xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle,
    ULONG   ThreadExtraSize,
    ULONG   KernelStackSize,
    ULONG   TlsDataSize,
    PULONG  ThreadId,
    PVOID   StartContext1,
    PVOID   StartContext2,
    BOOLEAN CreateSuspended,
    BOOLEAN DebuggerThread,
    PVOID   StartRoutine        // Xbox VA → resolved via recomp_lookup()
);
```

## Synchronization

Full implementations of:

| Xbox API | Win32 Implementation |
|----------|---------------------|
| NtCreateEvent | CreateEvent |
| NtSetEvent / KeSetEvent | SetEvent |
| NtWaitForSingleObject | WaitForSingleObject |
| NtWaitForMultipleObjectsEx | WaitForMultipleObjects |
| NtCreateSemaphore | CreateSemaphore |
| NtReleaseSemaphore | ReleaseSemaphore |
| RtlInitializeCriticalSection | InitializeCriticalSection |
| RtlEnterCriticalSection | EnterCriticalSection |
| RtlLeaveCriticalSection | LeaveCriticalSection |

## NTSTATUS Codes

```c
#define STATUS_SUCCESS                0x00000000
#define STATUS_TIMEOUT                0x00000102
#define STATUS_PENDING                0x00000103
#define STATUS_NO_MEMORY              0xC0000017
#define STATUS_ACCESS_DENIED          0xC0000022
#define STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034
#define STATUS_OBJECT_NAME_COLLISION  0xC0000035
// ... full list in kernel.h

#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
```

## Calling Conventions

| Convention | Used By | Example |
|-----------|---------|---------|
| `__stdcall` | Most kernel functions | `xbox_NtCreateFile(...)` |
| `__fastcall` | Kf* and ObfDereference | `xbox_KfRaiseIrql(NewIrql)` |
| `__cdecl` | Variadic functions | `xbox_DbgPrint(format, ...)` |
