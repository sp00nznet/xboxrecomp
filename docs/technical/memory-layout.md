# Memory Layout Reproduction

How to map Xbox memory at its original virtual addresses on Windows so that recompiled code with hardcoded addresses works unmodified.

## The Problem

Xbox games are compiled with hardcoded absolute addresses. The Burnout 3 binary contains thousands of instructions like:

```asm
mov eax, [0x004D532C]    ; read global variable
cmp dword ptr [0x4A1C74], 0  ; check button state
mov [0x5FFD08], ecx       ; write boost meter
```

After recompilation to C, these become:

```c
eax = MEM32(0x004D532C);
if (CMP_EQ(MEM32(0x4A1C74), 0)) goto loc_xyz;
MEM32(0x5FFD08) = ecx;
```

For this to work, reading address 0x004D532C must return the actual game data that was originally at that address. The entire Xbox memory map must be reproduced at the correct virtual addresses.

## Xbox Memory Map

The Xbox has 64 MB of unified RAM shared between CPU and GPU. Key regions:

| Address Range | Content | Size |
|---------------|---------|------|
| 0x00000000-0x000000FF | KPCR / TIB (thread info block) | 256 B |
| 0x00010000-0x00010FFF | XBE image header | 4 KB |
| 0x00011000-0x002CCFFF | .text (game code) | 2.73 MB |
| 0x002CC200-0x0036B7BF | XDK library code (D3D, DSOUND, XMV, etc.) | ~600 KB |
| 0x0036B7C0-0x003B2354 | .rdata (constants, strings, vtables) | 280 KB |
| 0x003B2360-0x0076EFFF | .data + BSS (globals, zero-initialized data) | 3.9 MB |
| 0x00780000-0x00F7FFFF | Stack (8 MB, grows downward) | 8 MB |
| 0x00F80000-0x03FFFFFF | Dynamic heap (bump allocator) | ~49 MB |
| 0x80010000+ | Xbox kernel PE header (fake, 1 page) | 4 KB |
| 0xFD000000+ | NV2A GPU registers (on-demand allocation) | Variable |
| 0xFE000000+ | Kernel function thunks (synthetic VAs) | ~600 B |

Total mapped: 64 MB contiguous at the base, plus special regions.

## Why CreateFileMapping, Not VirtualAlloc

The Xbox memory controller uses a 26-bit address bus. All addresses wrap modulo 64 MB:

```
Address 0x04070000 == Address 0x00070000  (both access same physical byte)
Address 0x20000448 == Address 0x00000448
```

The RenderWare engine exploits this. Its memory walker crosses 64 MB and reads mirrored data for an extended walk covering 256+ MB of virtual addresses. Game initialization code also writes large data structures past 64 MB that on real hardware wrap into physical RAM.

**VirtualAlloc cannot do this.** VirtualAlloc gives you distinct physical pages at each virtual address. Writing to 0x04070000 does NOT update the data at 0x00070000. We wasted days debugging this before switching to file mappings.

**CreateFileMapping + MapViewOfFileEx** creates true aliases. Multiple virtual address ranges can map to the same physical pages:

```c
// Create a page-file-backed mapping for 64 MB
HANDLE mapping = CreateFileMappingA(
    INVALID_HANDLE_VALUE,  // page file backed
    NULL,                  // default security
    PAGE_READWRITE,
    0, 64 * 1024 * 1024,  // 64 MB
    NULL                   // unnamed
);

// Map the base view at the desired Xbox address
void *base = MapViewOfFileEx(
    mapping,
    FILE_MAP_ALL_ACCESS,
    0, 0,
    64 * 1024 * 1024,
    (LPVOID)0x00000000     // desired base address
);

// Map mirror views at 64 MB intervals
for (int m = 0; m < 28; m++) {
    uintptr_t mirror_addr = (uintptr_t)base + (m + 1) * 64 * 1024 * 1024;
    void *mirror = MapViewOfFileEx(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        64 * 1024 * 1024,
        (LPVOID)mirror_addr
    );
    // Now writes to mirror_addr + X are visible at base + X
}
```

With 28 mirror views covering 1.75 GB of address space, the RenderWare memory walker can traverse the full range it expects.

## Address Translation: The XBOX_PTR Macro

All memory access goes through a single macro:

```c
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
```

### Why the uint32_t Cast is Essential

The `(uint32_t)` cast in XBOX_PTR is critical. On a 64-bit Windows build, `uintptr_t` is 64 bits. Without the cast:

```c
// WRONG: if addr is the result of (0xFFFFFFFF + 1), it becomes 0x100000000
// on 64-bit, this is 4 GB past our mapping -> access violation
#define XBOX_PTR_BAD(addr) ((uintptr_t)(addr) + g_xbox_mem_offset)
```

Xbox addresses are 32-bit and arithmetic in recompiled code can overflow. The uint32_t cast truncates to 32 bits first, matching Xbox hardware behavior where addresses wrap at 4 GB:

```c
// CORRECT: overflow wraps to 32 bits, then extends to 64-bit for the add
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
```

### The Memory Offset

When the mapping lands at the original Xbox address (0x00000000), `g_xbox_mem_offset` is 0 and the MEM macros are identity casts. When Windows cannot map at the preferred address (common on Windows 11 where low addresses are reserved), the offset adjusts all accesses:

```c
// Set once during init, then read-only
ptrdiff_t g_xbox_mem_offset = (uintptr_t)actual_base - XBOX_MAP_START;
```

The implementation tries multiple base addresses in order of preference:

```c
static const uintptr_t try_bases[] = {
    0x00010000,  // Original Xbox address (ideal)
    0x00800000,  // 8 MB - above typical PEB/TEB
    0x01000000,  // 16 MB
    0x02000000,  // 32 MB
    0x10000000,  // 256 MB
    0,           // Let OS choose (last resort)
};
```

## Section Initialization

After mapping the 64 MB region, the XBE file's sections are copied to their original addresses:

```c
// Copy XBE header (kernel thunk table, certificate, section info)
memcpy(XBOX_VA(0x00010000), xbe_data, header_size);

// Copy .text (code bytes -- needed for RW memory walker)
memcpy(XBOX_VA(0x00011000), xbe + 0x00001000, 2863616);

// Copy .rdata (constants, strings, vtables)
memcpy(XBOX_VA(0x0036B7C0), xbe + 0x0035C000, 289684);

// Copy initialized .data
memcpy(XBOX_VA(0x003B2360), xbe + 0x003A3000, 424960);

// BSS is already zeroed by the file mapping
```

Additional XDK library sections are also copied (XMV, DSOUND, WMADEC, XONLINE, XNET, D3D, XGRPH, XPP, DOLBY, XON_RD, .data1).

## Gotchas

### .rdata Is Not Write-Protected

You would expect .rdata (read-only data) to be protected:

```c
VirtualProtect(XBOX_VA(0x0036B7C0), 289684, PAGE_READONLY, &old_protect);
```

**Do not do this.** The .rdata end (0x003B2454) and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF). VirtualProtect rounds to page boundaries, so making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only. Game initialization code writes to globals in that overlap range and faults.

Additionally, the game writes to .rdata at runtime. String pointers in .rdata get overwritten during resource loading. This is technically a bug in the original game, but it works on Xbox because .rdata is not actually protected in the Xbox kernel's memory model.

### .rdata String Corruption

Because the game writes to .rdata at runtime, string data gets corrupted. Functions that read filenames from .rdata must read from the original XBE file data instead:

```c
// WRONG: reads from potentially-corrupted .rdata in mapped memory
const char *name = (const char *)XBOX_PTR(name_va);

// CORRECT: reads from pristine XBE file copy
extern const uint8_t *g_xbe_data;
size_t file_offset = (name_va - 0x36B7C0) + 0x35C000;
const char *name = (const char *)(g_xbe_data + file_offset);
```

### BSS Mirror Addresses Fail

Some BSS addresses (around 0x76000000) would need mirror views at addresses that Windows 11 reserves for system use. About 4 out of 33 mirror views fail to map. This is acceptable -- the game only accesses those addresses through the base view, and the RenderWare walker handles missing mirrors gracefully.

### Fake Thread Information Block

The recompiler drops the `fs:` segment prefix from memory accesses like `mov eax, fs:[0x28]`. These become `MEM32(0x28)`, reading from low memory. A fake TIB is populated at address 0x0:

```c
MEM32(0x00) = 0xFFFFFFFF;      // SEH: end of chain
MEM32(0x04) = XBOX_STACK_TOP;  // Stack base (high address)
MEM32(0x08) = XBOX_STACK_BASE; // Stack limit (low address)
MEM32(0x18) = 0x00000000;      // Self pointer
MEM32(0x20) = 0x00000000;      // KPCR Prcb pointer
MEM32(0x28) = FAKE_TLS_VA;     // TLS / RW engine context
```

The RenderWare engine reads `[fs:[0x28] + 0x28]` to find its per-thread data area. A fake structure chain is set up in BSS memory.

### Xbox Kernel PE Header

RenderWare's cache initialization code reads `MEM32(0x8001003C)` to parse the Xbox kernel's PE header and find the INIT section for CPU cache line sizing. A fake PE header with 0 sections is allocated at 0x80010000 so the function gracefully skips:

```c
void *kernel_page = VirtualAlloc((LPVOID)0x80010000, 4096,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
memset(kernel_page, 0, 4096);
*(uint32_t *)((uint8_t *)kernel_page + 0x3C) = 0x80;  // e_lfanew
// NumberOfSections = 0, so the INIT section search finds nothing
```

## Dynamic Heap: Bump Allocator

The Xbox heap serves allocations from `MmAllocateContiguousMemory` and similar kernel functions. A simple bump allocator works because Xbox games rarely free memory:

```c
static uint32_t g_heap_next = XBOX_HEAP_BASE;  // 0x00880000

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment) {
    if (size < 4096) size = 4096;  // minimum to prevent overlapping zero-size allocs

    uint32_t result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_BASE + XBOX_HEAP_SIZE)
        return 0;  // out of memory

    g_heap_next = result + size;
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;  // returns Xbox VA, not native pointer
}

void xbox_HeapFree(uint32_t xbox_va) {
    (void)xbox_va;  // no-op
}
```

The minimum allocation size of 4096 bytes prevents a subtle bug: Xbox D3D8 code sometimes computes resource sizes from GPU capabilities that return 0 (no real NV2A hardware), causing zero-size allocations that all return the same address and overlap.

## NV2A GPU Registers

The Xbox GPU (NV2A) has memory-mapped registers at 0xFD000000+. Game code and XDK library code read/write these registers directly. On Windows, these addresses are not mapped by default.

A Vectored Exception Handler (VEH) intercepts access violations in this range and allocates pages on demand:

```c
LONG WINAPI nv2a_veh_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t fault_addr = info->ExceptionRecord->ExceptionInformation[1];
    if (fault_addr >= 0xFD000000 && fault_addr < 0xFF000000) {
        // Allocate a page at the faulting address
        uintptr_t page = fault_addr & ~0xFFF;
        VirtualAlloc((LPVOID)page, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
```

The allocated pages are zeroed, so GPU register reads return 0 (safe defaults). NV2A functions that spin-wait on register values must be stubbed entirely -- allocating the page only prevents the crash; the spin-wait still loops forever on a zero value.

## Memory Layout Summary

```
0x00000000  ┌──────────────────────┐
            │ Fake TIB / KPCR      │  256 bytes
0x00010000  ├──────────────────────┤
            │ XBE Image Header     │  4 KB
0x00011000  ├──────────────────────┤
            │ .text (game code)    │  2.73 MB
0x002CC200  ├──────────────────────┤
            │ XDK library sections │  ~600 KB
0x0036B7C0  ├──────────────────────┤
            │ .rdata (constants)   │  280 KB
0x003B2360  ├──────────────────────┤
            │ .data + BSS          │  3.9 MB
0x00780000  ├──────────────────────┤
            │ Stack (8 MB)         │  g_esp starts at top
0x00F80000  ├──────────────────────┤
            │ Dynamic Heap         │  ~49 MB (bump allocator)
0x03FFFFFF  └──────────────────────┘  End of 64 MB region
            │ ... 28 mirror views  │  Each 64 MB, aliased to base
0x80010000  │ Fake kernel PE hdr   │  1 page
0xFD000000  │ NV2A GPU registers   │  On-demand VEH allocation
0xFE000000  │ Kernel thunks        │  Synthetic VAs for 147 imports
```
