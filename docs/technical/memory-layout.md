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
| 0x00F80000-0x03FFFFFF | Dynamic heap (block allocator) | ~49 MB |
| 0x04000000-0x7FFFFFFF | RAM mirrors, then reservable space above RAM | — |
| 0x80000000-0x83FFFFFF | Contiguous arena (`MmAllocateContiguousMemory`) | 64 MB |
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

## Dynamic Heap

`xbox_HeapAlloc` started as a bump allocator — increment a pointer, never
reclaim — on the reasoning that Xbox titles allocate while loading and hold
everything. That holds for Burnout 3 and not in general: middleware churns.
*X-Men Legends* frees and reallocates 2 MB pool segments on every cutscene, and
with nothing reclaimed it ran out within minutes:

```
xbox_HeapAlloc: out of memory
out of thread stacks, running worker inline
```

— and then froze when the inline worker suspended the thread that created it.

The heap is now a block table: allocations are recorded in address order,
`xbox_HeapFree` marks a block free and coalesces it with free neighbours, and a
request reuses the first free block that fits. Three rules make reuse actually
return memory:

- **Carve, don't hand over.** Reuse takes only the aligned piece it needs and
  leaves the front and back free. Handing a whole freed block (2 MB) to a
  16-byte request leaks the rest just as surely as never freeing.
- **Coalescing steps over empty slots.** A merge leaves a zero-size slot behind;
  a neighbour check that stops at it stops every later merge.
- **One lock.** DirectSound, the movie threads and the title allocate at once.

`NtFreeVirtualMemory` on heap-backed memory releases the block (`MEM_RELEASE`)
and treats `MEM_DECOMMIT` as a no-op. It used to fall through to a host
`VirtualFree`, passing a guest address to Windows as a 64-bit pointer, so
nothing came back.

The minimum allocation of 4096 bytes stays: Xbox D3D8 code sometimes computes
resource sizes from GPU capabilities that read 0, and zero-size allocations
would all return the same address.

## Memory Above RAM

Some titles manage address space themselves: reserve a range, commit pieces of
it, and check they got the address they asked for. The MSVC CRT heap does this
when it grows, starting at `0x04000000` — the first address past RAM. Three
things went wrong there:

| Problem | Effect |
|---|---|
| `NtQueryVirtualMemory` reported the RAM mirrors as free | the CRT's scanner picked `0x04000000`, was refused, and asked again forever |
| `NtAllocateVirtualMemory` with an explicit base above RAM went to the heap, which ignores the requested base | the title got a different address, noticed, and failed |
| nothing on the host backed those addresses | even correct bookkeeping had nothing to hand out |

`guest_vmem.c` tracks reservations and commits above RAM per 4 KB page,
honours the exact requested base or fails with a status the caller's retry
logic understands, and answers queries from its own records. The mirrors are
reported as **reserved**, so scanners step past them. `xbox_MemoryLayoutInit`
reserves host memory for the range right after mapping the mirrors, committing
pieces only when the title commits them.

The status matters as much as the answer. A refused base is
`STATUS_CONFLICTING_ADDRESSES` (`0xC0000018`); Windows maps that to
`ERROR_INVALID_ADDRESS` (487), and 487 is the only error on which the CRT tries
another address. `RtlNtStatusToDosError` had no mapping for it, so it returned
the generic 317 and the heap stopped growing.

A title executable should link with `/DYNAMICBASE:NO`: with address-space
randomisation the host kept taking the fixed range first.

## The Contiguous Arena

`MmAllocateContiguousMemory` hands out memory from a window at
`XBOX_CONTIG_BASE` (`0x80000000`), and `MmGetPhysicalAddress` answers
`va - XBOX_CONTIG_BASE` for it. Titles give those physical addresses to
hardware: DirectSound fills the APU's scatter-gather tables with them, the USB
driver links its descriptors with them, and D3D writes them into the
pushbuffer. Every device model therefore has to turn a physical address back
into memory it can read, and they did not agree on how.

The ambiguity was structural. The arena's physical addresses started at 0 — the
same numbers as the title image's own addresses — and `MmGetPhysicalAddress`
passes anything outside the arena through unchanged. *X-Men Legends*' USB stack
hands the controller both kinds: contiguous descriptors (physical
`0x006DC6A0`) and a static buffer in `.data` (VA `0x005DCE24`). No rule can tell
those apart, and the model that guessed "VA" read the title's own globals and
wrote its frame counter into them.

The arena now starts above the loaded image, so the two ranges cannot overlap,
and `xbox_ContiguousIsPhysical(phys)` answers exactly: true for addresses the
arena handed out, which are reached at `XBOX_CONTIG_BASE + phys`; anything else
is a pass-through VA. The APU (`apu_phys`), the OHCI model (`bus_to_va`), the
pushbuffer executor (`dma_resolve`) and the display-mode bridge all use that
one rule. It lives in the small `xbox_devbus` library, with the device
interrupt lines, so a device model can link it without the kernel bridge.

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
            │ Dynamic Heap         │  ~49 MB (block allocator)
0x03FFFFFF  └──────────────────────┘  End of 64 MB region
            │ ... 28 mirror views  │  Each 64 MB, aliased to base
0x80010000  │ Fake kernel PE hdr   │  1 page
0xFD000000  │ NV2A GPU registers   │  On-demand VEH allocation
0xFE000000  │ Kernel thunks        │  Synthetic VAs for 147 imports
```
