/**
 * Xbox Memory Layout Template
 *
 * Template for reproducing the Xbox memory layout on a Windows host.
 * Adapt the section addresses and sizes for your specific XBE's layout.
 *
 * The Xbox has 64 MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * The game's code and data were linked expecting specific address ranges.
 *
 * On Windows, we need to:
 *   1. Reserve the same virtual address range (0x00010000+)
 *   2. Map sections to their expected addresses
 *   3. Handle the fact that Xbox has no ASLR
 *   4. Provide contiguous memory for fake GPU resources
 *
 * Strategy:
 *   Use CreateFileMapping + MapViewOfFileEx to place sections at their
 *   original VAs. File mapping allows mirror views that alias the same
 *   physical pages, which is needed because the Xbox 26-bit address bus
 *   wraps all addresses modulo 64 MB.
 */

#ifndef XBOX_MEMORY_H
#define XBOX_MEMORY_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 *
 * CUSTOMIZE: Replace these values with your game's actual section
 * layout from XBE analysis. Use an XBE parser to extract section
 * VAs, sizes, and raw file offsets.
 * ================================================================ */

/** Base address of all XBE files (constant across all Xbox games). */
#define XBOX_BASE_ADDRESS       0x00010000

/** Start of the mapped region. Include low memory (0x0) because
 *  game code reads from addresses like 0x20 and 0x28 (Xbox TIB/KPCR
 *  fields accessed via fs: segment, which the recompiler translates
 *  to flat memory reads). */
#define XBOX_MAP_START          0x00000000

/* ---- Section addresses (CUSTOMIZE per game) ---- */

/** .text section (executable code) */
#define XBOX_TEXT_VA            0x00011000    /* CUSTOMIZE */
#define XBOX_TEXT_SIZE          0x002BC000    /* CUSTOMIZE */
#define XBOX_TEXT_RAW_OFFSET    0x00001000    /* CUSTOMIZE: file offset in XBE */

/** .rdata section (read-only data: strings, vtables, constants) */
#define XBOX_RDATA_VA           0x0036B7C0    /* CUSTOMIZE */
#define XBOX_RDATA_SIZE         289684        /* CUSTOMIZE */
#define XBOX_RDATA_RAW_OFFSET   0x0035C000    /* CUSTOMIZE */

/** .data section (read-write globals + BSS) */
#define XBOX_DATA_VA            0x003B2360    /* CUSTOMIZE */
#define XBOX_DATA_SIZE          3904988       /* CUSTOMIZE: total virtual size */
#define XBOX_DATA_INIT_SIZE     424960        /* CUSTOMIZE: initialized portion */
#define XBOX_DATA_RAW_OFFSET    0x003A3000    /* CUSTOMIZE */
/* BSS starts at DATA_VA + DATA_INIT_SIZE, zero-initialized */

/** Xbox physical memory size. */
#define XBOX_MEM_SIZE           (64 * 1024 * 1024)  /* 64 MB */

/* ---- Stack configuration ---- */

/** Stack size for the simulated Xbox stack.
 *  8 MB provides headroom for failed ICALL stdcall arg leaks. */
#define XBOX_STACK_SIZE         (8 * 1024 * 1024)

/** Base VA of the stack area (above last XBE section).
 *  CUSTOMIZE: set this above your game's last section. */
#define XBOX_STACK_BASE         0x00780000    /* CUSTOMIZE */

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP          (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* ---- Heap configuration ---- */

/** Base VA of the dynamic heap (above stack). */
#define XBOX_HEAP_BASE          (XBOX_STACK_BASE + XBOX_STACK_SIZE)

/** Heap fills the remaining 64 MB address space.
 *  This ensures RenderWare's memory probing stops at the correct
 *  boundary (64 MB), matching Xbox hardware behavior. */
#define XBOX_HEAP_SIZE          (XBOX_MEM_SIZE - XBOX_HEAP_BASE)

/* ---- Mirror configuration ---- */

/** Number of 64 MB mirror views to create.
 *  Xbox RAM wraps at 64 MB due to 26-bit address bus. Game engines
 *  (especially RenderWare) walk memory past 64 MB expecting wrapping.
 *  28 mirrors covers ~1.75 GB of address space. */
#define XBOX_NUM_MIRRORS        28

/* ---- Kernel data exports area ---- */

/** Base VA for fake kernel data exports.
 *  Games read hardware info, kernel version, etc. from kernel data
 *  exports. Allocate a page in unused VA space for these. */
#define XBOX_KERNEL_DATA_BASE   0x00740000    /* CUSTOMIZE */
#define XBOX_KERNEL_DATA_SIZE   4096

/* Offsets within the kernel data area (standard layout) */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* Kernel version (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage pointer (4 bytes) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */

/* ================================================================
 * Memory initialization API
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Creates a file mapping backed by the page file, maps the base view
 * at or near XBOX_MAP_START, copies XBE sections to their expected VAs,
 * initializes the stack and heap, sets up the fake TIB at address 0,
 * and creates mirror views for RAM wrapping.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file in bytes.
 * @return TRUE on success, FALSE on failure.
 *
 * Implementation pattern (in your .c file):
 *
 *   1. CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
 *                         0, total_size, NULL)
 *   2. MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
 *                      total_size, (LPVOID)desired_base)
 *      Try 0x00010000 first, then fallback addresses (0x10000000, etc.)
 *   3. memcpy sections from XBE to their VAs
 *   4. Set up fake TIB at VA 0x0:
 *      - [0x00] = 0xFFFFFFFF (SEH end of chain)
 *      - [0x04] = XBOX_STACK_TOP (stack base)
 *      - [0x08] = XBOX_STACK_BASE (stack limit)
 *      - [0x28] = fake TLS pointer (for RenderWare fs: access)
 *   5. Map mirror views at 64 MB intervals
 *   6. Set g_xbox_mem_offset and g_esp
 */
BOOL xbox_memory_init(const void *xbe_data, size_t xbe_size);

/**
 * Shut down and release all Xbox memory mappings.
 */
void xbox_memory_shutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_is_xbox_address(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory is not initialized.
 */
void *xbox_get_memory_base(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_get_memory_offset(void);

/**
 * Get the file mapping handle (for creating additional mirror views).
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_get_mapping_handle(void);

/* ================================================================
 * Dynamic heap allocator
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works.
 *
 * For production use, consider replacing with a proper allocator
 * (dlmalloc, etc.) that supports free operations.
 * ================================================================ */

/**
 * Allocate from the Xbox heap.
 * @param size      Number of bytes to allocate.
 * @param alignment Required alignment (power of 2, minimum 4).
 * @return Xbox VA of the allocation, or 0 on failure.
 */
uint32_t xbox_heap_alloc(uint32_t size, uint32_t alignment);

/**
 * Free a heap allocation.
 * Currently a no-op for the bump allocator. Implement if your game
 * requires memory recycling.
 */
void xbox_heap_free(uint32_t xbox_va);

/* ================================================================
 * VEH handler for NV2A GPU register pages
 *
 * The Xbox GPU (NV2A) has memory-mapped registers at physical
 * addresses 0xFD000000-0xFDFFFFFF. Some game code (especially
 * D3D8 LTCG) reads/writes these registers directly.
 *
 * Strategy: Use a Vectored Exception Handler (VEH) to catch
 * access violations in the NV2A range and map stub pages on demand.
 *
 * Implementation pattern:
 *
 *   LONG CALLBACK nv2a_veh_handler(PEXCEPTION_POINTERS info) {
 *       if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
 *           return EXCEPTION_CONTINUE_SEARCH;
 *       uintptr_t fault_addr = info->ExceptionRecord->ExceptionInformation[1];
 *       if (fault_addr >= NV2A_BASE && fault_addr < NV2A_BASE + NV2A_SIZE) {
 *           // Map a zero page at the faulting address
 *           uintptr_t page = fault_addr & ~0xFFF;
 *           VirtualAlloc((LPVOID)page, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
 *           return EXCEPTION_CONTINUE_EXECUTION;
 *       }
 *       return EXCEPTION_CONTINUE_SEARCH;
 *   }
 *
 * Call AddVectoredExceptionHandler(1, nv2a_veh_handler) during init.
 * ================================================================ */

/* NV2A register space (adjust + g_xbox_mem_offset for actual addresses) */
#define XBOX_NV2A_BASE  0xFD000000u
#define XBOX_NV2A_SIZE  0x01000000u  /* 16 MB */

#ifdef __cplusplus
}
#endif

#endif /* XBOX_MEMORY_H */
