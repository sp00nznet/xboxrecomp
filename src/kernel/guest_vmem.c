#include "guest_vmem.h"
#include "xbox_memory_layout.h"
#include <string.h>
#include <stdio.h>

extern ptrdiff_t g_xbox_mem_offset;

/* Real Xbox user-mode address space ends at 0x7FFEFFFF (kernel/system
 * reserve everything from 0x7FFF0000 up). Round the ceiling down to a
 * 64 KiB allocation granularity boundary, matching how NtAllocateVirtualMemory
 * behaves on real hardware. */
#define VMEM_TOP            0x7FFE0000u
#define VMEM_PAGE           4096u

#define MEM_COMMIT_FLAG     0x1000u
#define MEM_RESERVE_FLAG    0x2000u
#define MEM_DECOMMIT_FLAG   0x4000u
#define MEM_RELEASE_FLAG    0x8000u
#define MEM_FREE_STATE      0x10000u
#define MEM_PRIVATE_TYPE    0x20000u

/* winnt.h defines STATUS_INVALID_PARAMETER/STATUS_NO_MEMORY on Windows
 * builds; only fill in what's missing. */
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER       0xC000000Du
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY               0xC0000017u
#endif
#define STATUS_CONFLICTING_ADDRESSES   0xC0000018u

typedef struct {
    uint32_t owner;    /* VA of the reservation this page belongs to, 0 = free */
    uint16_t state;     /* 0 = free, MEM_RESERVE_FLAG, or MEM_COMMIT_FLAG */
    uint16_t protect;
    uint32_t reserve_protect; /* protection the reservation was created with */
} guest_page_t;

/* One entry per 4 KiB page from g_xbox_total_ram to VMEM_TOP. Sized for the
 * worst case (RAM starting at 0) so it works regardless of how much RAM a
 * given run maps; unused low entries just never get touched. */
static guest_page_t s_pages[VMEM_TOP / VMEM_PAGE];

static uint32_t page_index(uint32_t va) { return va / VMEM_PAGE; }

/* Host backing for the extended-VMA range [aliased_top, VMEM_TOP).
 *
 * xbox_memory_layout.c's RAM-plus-mirrors scheme only ever reserves real
 * host memory for [0, aliased_top) -- the base MapViewOfFileEx region plus
 * XBOX_NUM_MIRRORS more views of the same physical pages, placed back to
 * back. Nothing else in this project reserves host memory above that, so
 * a guest reservation up here (now that guest_vmem_query correctly steers
 * the caller's own scanner past aliased_top instead of looping on it --
 * see FINDINGS.md) was succeeding at the bookkeeping level and then
 * faulting for real the first time anything touched it: g_xbox_mem_offset
 * + guest_va pointed at host address space nothing had ever reserved.
 * Reserve it for real, once, lazily (guest_vmem_allocate is the only
 * caller, and by the time it first runs xbox_memory_layout.c has already
 * set g_xbox_mem_offset), then MEM_COMMIT the specific sub-ranges this
 * file's own commit paths touch instead of assuming the memory is already
 * live. */
/* Set by xbox_MemoryLayoutInit, right after the RAM mirrors, once it has
 * actually reserved host memory for this range -- see the big comment
 * there for why that has to happen early rather than lazily here. */
extern int g_xbox_extvma_reserved;

/* Commits [start, end) of the extended-VMA host reservation (real
 * VirtualAlloc MEM_COMMIT, which also zeroes the pages -- the memset
 * callers already did stays too, harmless and cheap, in case any commit
 * path here ever runs before extvma_ensure_reserved for a range that
 * needs it, e.g. inside plain RAM where no reservation is needed at all). */
static int extvma_commit(uint32_t start, uint32_t end)
{
    void *host_addr;
    SIZE_T len;

    if (!g_xbox_extvma_reserved)
        return 0;

    host_addr = (void *)((uintptr_t)g_xbox_mem_offset + start);
    len = (SIZE_T)(end - start);
    if (!VirtualAlloc(host_addr, len, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "guest_vmem: failed to commit extended-VMA host backing "
                        "at %p (%zu bytes, error %lu)\n",
                host_addr, (size_t)len, GetLastError());
        return 0;
    }
    return 1;
}

/* True when every page in [start, end) is currently free. */
static int range_is_free(uint32_t start, uint32_t end)
{
    uint32_t p;
    for (p = page_index(start); p < page_index(end); ++p) {
        if (s_pages[p].state != 0)
            return 0;
    }
    return 1;
}

/* True when every page in [start, end) belongs to the same existing
 * reservation (needed before a commit can back part of it). */
static int range_owned_by(uint32_t start, uint32_t end, uint32_t owner)
{
    uint32_t p;
    if (!owner)
        return 0;
    for (p = page_index(start); p < page_index(end); ++p) {
        if (s_pages[p].owner != owner)
            return 0;
    }
    return 1;
}

int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                         uint32_t protect, uint32_t *status)
{
    uint32_t requested_base = *base;
    uint32_t requested_size = *size;
    uint32_t start, end, p;
    uint32_t owner;
    int is_reserve = (alloc_type & MEM_RESERVE_FLAG) != 0;
    int is_commit  = (alloc_type & MEM_COMMIT_FLAG) != 0;
    uint64_t end64;

    /* Only handle explicit high addresses here -- base=0 ("anywhere") and
     * anything inside RAM still goes through the existing bump allocator. */
    if (requested_base < g_xbox_total_ram)
        return 0;

    /* Everything from here up to (mirror count + 1) * RAM size aliases back
     * onto the exact same physical RAM -- this toolkit's own mirror views
     * (xbox_memory_layout.c) replicate the console's 26-bit address bus
     * wraparound faithfully, so there is no real, distinct memory anywhere
     * in that range to hand out. Treating a request in it as free, new
     * memory (this function's original design) silently backs it with a
     * memset that lands on whatever real guest data already occupies the
     * aliased low address -- confirmed live: a title reserving inside this
     * range zeroed live .rdata sitting at the wrapped-around offset.
     * Refuse cleanly instead, so the caller's own address-mismatch retry
     * (confirmed to exist and work -- see FINDINGS.md Entry 5) picks a
     * real, non-aliased address instead of us corrupting one. */
    {
        uint64_t aliased_top = (uint64_t)g_xbox_total_ram * (1u + XBOX_NUM_MIRRORS);
        if (requested_base < aliased_top) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 1;
        }
    }

    *status = STATUS_INVALID_PARAMETER;

    if (!requested_size || (!is_reserve && !is_commit))
        return 1;

    /* A commit's base has to land on a real page; a reservation's base is
     * rounded down to the 64 KiB allocation granularity real Xbox uses. */
    start = requested_base & ~(is_reserve ? 0xFFFFu : (VMEM_PAGE - 1));
    end64 = (uint64_t)requested_base + requested_size;
    end64 = (end64 + VMEM_PAGE - 1) & ~(uint64_t)(VMEM_PAGE - 1);

    if (start < g_xbox_total_ram || end64 > VMEM_TOP || end64 <= start)
        return 1;
    end = (uint32_t)end64;

    if (is_reserve) {
        if (!range_is_free(start, end)) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 1;
        }
        owner = start;
        for (p = page_index(start); p < page_index(end); ++p) {
            s_pages[p].owner = owner;
            s_pages[p].state = (uint16_t)MEM_RESERVE_FLAG;
            s_pages[p].reserve_protect = protect;
            s_pages[p].protect = 0;
        }
        if (is_commit) {
            if (!extvma_commit(start, end)) {
                *status = STATUS_NO_MEMORY;
                return 1;
            }
            for (p = page_index(start); p < page_index(end); ++p) {
                s_pages[p].state = (uint16_t)MEM_COMMIT_FLAG;
                s_pages[p].protect = (uint16_t)protect;
            }
        }
    } else {
        /* MEM_COMMIT alone: every page in range must already belong to one
         * existing reservation. */
        owner = s_pages[page_index(start)].owner;
        if (!range_owned_by(start, end, owner)) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 1;
        }
        if (!extvma_commit(start, end)) {
            *status = STATUS_NO_MEMORY;
            return 1;
        }
        for (p = page_index(start); p < page_index(end); ++p) {
            s_pages[p].state = (uint16_t)MEM_COMMIT_FLAG;
            s_pages[p].protect = (uint16_t)protect;
        }
    }

    *base = start;
    *size = end - start;
    *status = 0; /* STATUS_SUCCESS */
    return 1;
}

int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                     uint32_t *status)
{
    uint32_t start, end, p, owner;
    uint64_t end64;

    if (*base < g_xbox_total_ram)
        return 0;

    *status = STATUS_INVALID_PARAMETER;
    if (*base >= VMEM_TOP)
        return 1;

    start = *base & ~(VMEM_PAGE - 1);
    owner = s_pages[page_index(start)].owner;
    if (!owner)
        return 1;

    if (free_type == MEM_RELEASE_FLAG) {
        /* Release has to name the exact reservation base and frees the
         * whole thing; *size must be 0 per the NT contract. */
        if (*size || *base != owner)
            return 1;
        end = start;
        while (end < VMEM_TOP && s_pages[page_index(end)].owner == owner)
            end += VMEM_PAGE;
    } else if (free_type == MEM_DECOMMIT_FLAG) {
        if (!*size)
            return 1;
        end64 = (uint64_t)*base + *size;
        end64 = (end64 + VMEM_PAGE - 1) & ~(uint64_t)(VMEM_PAGE - 1);
        if (end64 > VMEM_TOP)
            return 1;
        end = (uint32_t)end64;
        if (!range_owned_by(start, end, owner))
            return 1;
    } else {
        return 1;
    }

    for (p = page_index(start); p < page_index(end); ++p) {
        if (s_pages[p].state == MEM_COMMIT_FLAG) {
            memset((void *)((uintptr_t)g_xbox_mem_offset + (size_t)p * VMEM_PAGE),
                   0, VMEM_PAGE);
        }
        if (free_type == MEM_RELEASE_FLAG) {
            memset(&s_pages[p], 0, sizeof(s_pages[p]));
        } else {
            s_pages[p].state = (uint16_t)MEM_RESERVE_FLAG;
            s_pages[p].protect = 0;
        }
    }

    *base = start;
    *size = end - start;
    *status = 0; /* STATUS_SUCCESS */
    return 1;
}

int guest_vmem_query(uint32_t address, uint32_t info[7])
{
    guest_page_t first;
    uint32_t start, end;

    if (address < g_xbox_total_ram || address >= VMEM_TOP)
        return 0;

    /* The aliased-mirror range (see guest_vmem_allocate's own guard) is
     * never tracked in s_pages -- guest_vmem_allocate always refuses to
     * hand it out, so it never gets marked reserved/committed there. Left
     * alone, that makes this query report it as one giant MEM_FREE_STATE
     * region, and a title's own address-space scanner (which uses this
     * query to find free space) keeps proposing addresses inside it,
     * getting refused, and retrying the same address forever -- confirmed
     * live: X-Men Legends' CRT heap-growth spun on base=0x04000000
     * indefinitely once its retry-on-ERROR_INVALID_ADDRESS path actually
     * worked (see FINDINGS.md). Report it as already reserved instead, so
     * the scanner skips past aliased_top on its very next query. */
    {
        uint64_t aliased_top = (uint64_t)g_xbox_total_ram * (1u + XBOX_NUM_MIRRORS);
        if (address < aliased_top) {
            info[0] = (uint32_t)g_xbox_total_ram;               /* BaseAddress */
            info[1] = (uint32_t)g_xbox_total_ram;               /* AllocationBase */
            info[2] = 0;                                        /* AllocationProtect */
            info[3] = (uint32_t)(aliased_top - g_xbox_total_ram); /* RegionSize */
            info[4] = MEM_RESERVE_FLAG;                         /* State */
            info[5] = 0;                                        /* Protect */
            info[6] = MEM_PRIVATE_TYPE;                         /* Type */
            return 1;
        }
    }

    start = address & ~(VMEM_PAGE - 1);
    first = s_pages[page_index(start)];
    end = start + VMEM_PAGE;
    while (end < VMEM_TOP) {
        guest_page_t *next = &s_pages[page_index(end)];
        if (next->owner != first.owner || next->state != first.state ||
            next->protect != first.protect)
            break;
        end += VMEM_PAGE;
    }

    info[0] = start;                                   /* BaseAddress */
    info[1] = first.owner;                              /* AllocationBase */
    info[2] = first.reserve_protect;                    /* AllocationProtect */
    info[3] = end - start;                               /* RegionSize */
    info[4] = first.state ? first.state : MEM_FREE_STATE; /* State */
    info[5] = first.protect;                             /* Protect */
    info[6] = first.state ? MEM_PRIVATE_TYPE : 0;         /* Type */
    return 1;
}
