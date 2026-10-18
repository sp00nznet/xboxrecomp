#include "guest_vmem.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VMEM_PAGE           4096u

#define MEM_COMMIT_FLAG     0x1000u
#define MEM_RESERVE_FLAG    0x2000u
#define MEM_DECOMMIT_FLAG   0x4000u
#define MEM_RELEASE_FLAG    0x8000u
#define MEM_FREE_STATE      0x10000u
#define MEM_PRIVATE_TYPE    0x20000u

/* winnt.h defines these on Windows builds; only fill in what is missing. */
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER       0xC000000Du
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY               0xC0000017u
#endif
#define STATUS_CONFLICTING_ADDRESSES   0xC0000018u

typedef struct {
    uint32_t owner;           /* VA of the reservation this page belongs to, 0 = free */
    uint16_t state;           /* 0 = free, MEM_RESERVE_FLAG or MEM_COMMIT_FLAG */
    uint16_t protect;
    uint32_t reserve_protect; /* protection the reservation was created with */
} guest_page_t;

static guest_page_t *s_pages;   /* one entry per page from s_lo to s_top */
static uint32_t s_mirror_lo;    /* first address that aliases low memory */
static uint32_t s_lo;           /* first tracked address: the top of the mirrors */
static uint32_t s_top;          /* end of what the host let us reserve */
static ptrdiff_t s_offset;      /* host address minus guest address */
static SRWLOCK s_lock = SRWLOCK_INIT;

static uint32_t page_index(uint32_t va) { return (va - s_lo) / VMEM_PAGE; }

int guest_vmem_active(void) { return s_pages != NULL; }

/* How far up from `host` the host address space is free, at most `limit`
 * bytes. The guest's user range and the host's are not the same thing: with the
 * guest image mapped at 0x10000, guest 0x7FFD0000 is host 0x7FFE0000, which on
 * Windows is KUSER_SHARED_DATA, so a straight reservation of the whole range
 * fails outright. */
static size_t host_free_run(uintptr_t host, size_t limit)
{
    uintptr_t cur = host;

    while (cur - host < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)cur, &mbi, sizeof mbi) || mbi.State != MEM_FREE)
            break;
        cur = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return cur - host < limit ? cur - host : limit;
}

/* Reserve the host range for [s_lo, s_top). The mirrors are the last thing the
 * layout maps, so right after them is when a fixed host address is most likely
 * to still be free; asked for lazily, deep into boot, the range was taken by
 * something else nearly every run. MEM_RESERVE only: commits happen where a
 * title commits, as on the console. */
int guest_vmem_init(ptrdiff_t offset, uint64_t mirror_lo, uint64_t mirror_top)
{
    size_t bytes;
    void *host;

    if (s_pages || !recomp_env(RENV_EXT_VMA))
        return 0;
    if (mirror_top >= GUEST_VMEM_TOP) {
        fprintf(stderr, "  Extended VMA: the mirrors reach 0x%llX, past user space;"
                        " nothing above them to track\n",
                (unsigned long long)mirror_top);
        return 0;
    }

    s_offset = offset;
    s_mirror_lo = (uint32_t)mirror_lo;
    s_lo = (uint32_t)mirror_top;
    host = (void *)((uintptr_t)s_lo + (uintptr_t)offset);

    bytes = host_free_run((uintptr_t)host, GUEST_VMEM_TOP - s_lo) & ~(size_t)0xFFFF;
    if (bytes && !VirtualAlloc(host, bytes, MEM_RESERVE, PAGE_NOACCESS))
        bytes = 0;
    if (!bytes) {
        fprintf(stderr, "  Extended VMA: could not reserve host memory at %p (error %lu);"
                        " the tracker stays off\n", host, GetLastError());
        return 0;
    }
    s_top = s_lo + (uint32_t)bytes;
    s_pages = calloc(bytes / VMEM_PAGE, sizeof s_pages[0]);
    if (!s_pages) {
        VirtualFree(host, 0, MEM_RELEASE);
        fprintf(stderr, "  Extended VMA: out of memory for the page table\n");
        return 0;
    }
    fprintf(stderr, "  Extended VMA: reserved %u MB at %p, guest 0x%08X..0x%08X%s\n",
            (unsigned)(bytes / (1024 * 1024)), host, s_lo, s_top,
            s_top < GUEST_VMEM_TOP ? " (the host has something above that)" : "");
    return 1;
}

void guest_vmem_shutdown(void)
{
    if (s_pages)
        VirtualFree((void *)((uintptr_t)s_lo + (uintptr_t)s_offset), 0, MEM_RELEASE);
    free(s_pages);
    s_pages = NULL;
}

/* Commits [start, end) of the host reservation. Zeroes the pages, as
 * VirtualAlloc does. */
static int extvma_commit(uint32_t start, uint32_t end)
{
    void *host = (void *)((uintptr_t)start + (uintptr_t)s_offset);

    if (!VirtualAlloc(host, (SIZE_T)(end - start), MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "guest_vmem: failed to commit %p (%u bytes, error %lu)\n",
                host, end - start, GetLastError());
        return 0;
    }
    return 1;
}

/* True when every page in [start, end) is free. */
static int range_is_free(uint32_t start, uint32_t end)
{
    uint32_t p;
    for (p = page_index(start); p < page_index(end); ++p) {
        if (s_pages[p].state != 0)
            return 0;
    }
    return 1;
}

/* True when every page in [start, end) belongs to one reservation. */
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

static int allocate_locked(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                           uint32_t protect, uint32_t *status)
{
    uint32_t requested_base = *base;
    uint32_t requested_size = *size;
    uint32_t start, end, p, owner;
    int is_reserve = (alloc_type & MEM_RESERVE_FLAG) != 0;
    int is_commit  = (alloc_type & MEM_COMMIT_FLAG) != 0;
    uint64_t end64;

    /* Between the start of the mirrors and the top of them, every address is
     * physical RAM seen again: there is no distinct memory to hand out, and a
     * title that reserved there would zero live data at the wrapped address
     * (confirmed: it wiped .rdata). Above s_top the host would not give us the
     * range. Refuse both cleanly, so the caller's own address-mismatch retry
     * picks a real address. */
    if (requested_base < s_lo || (requested_base >= s_top && requested_base < GUEST_VMEM_TOP)) {
        *status = STATUS_CONFLICTING_ADDRESSES;
        return 1;
    }

    *status = STATUS_INVALID_PARAMETER;
    if (!requested_size || (!is_reserve && !is_commit))
        return 1;

    /* A commit's base lands on a real page; a reservation's base rounds down
     * to the 64 KiB allocation granule. */
    start = requested_base & ~(is_reserve ? 0xFFFFu : (VMEM_PAGE - 1));
    end64 = (uint64_t)requested_base + requested_size;
    end64 = (end64 + VMEM_PAGE - 1) & ~(uint64_t)(VMEM_PAGE - 1);
    if (start < s_lo || end64 > s_top || end64 <= start)
        return 1;
    end = (uint32_t)end64;

    if (is_reserve) {
        if (!range_is_free(start, end)) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 1;
        }
        if (is_commit && !extvma_commit(start, end)) {
            *status = STATUS_NO_MEMORY;
            return 1;
        }
        owner = start;
        for (p = page_index(start); p < page_index(end); ++p) {
            s_pages[p].owner = owner;
            s_pages[p].state = (uint16_t)(is_commit ? MEM_COMMIT_FLAG : MEM_RESERVE_FLAG);
            s_pages[p].reserve_protect = protect;
            s_pages[p].protect = (uint16_t)(is_commit ? protect : 0);
        }
    } else {
        /* MEM_COMMIT alone: every page must already belong to one reservation. */
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
    *status = 0;
    return 1;
}

int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                        uint32_t protect, uint32_t *status)
{
    int handled;

    /* base = 0 means "anywhere", and anything below the mirrors is the heap's. */
    if (!s_pages || *base < s_mirror_lo)
        return 0;
    AcquireSRWLockExclusive(&s_lock);
    handled = allocate_locked(base, size, alloc_type, protect, status);
    ReleaseSRWLockExclusive(&s_lock);
    return handled;
}

static int free_locked(uint32_t *base, uint32_t *size, uint32_t free_type,
                       uint32_t *status)
{
    uint32_t start, end, p, owner;
    uint64_t end64;

    *status = STATUS_INVALID_PARAMETER;
    if (*base < s_lo || *base >= s_top)
        return 1;

    start = *base & ~(VMEM_PAGE - 1);
    owner = s_pages[page_index(start)].owner;
    if (!owner)
        return 1;

    if (free_type == MEM_RELEASE_FLAG) {
        /* Release names the exact reservation base and frees all of it; *size
         * must be 0 per the NT contract. */
        if (*size || *base != owner)
            return 1;
        end = start;
        while (end < s_top && s_pages[page_index(end)].owner == owner)
            end += VMEM_PAGE;
    } else if (free_type == MEM_DECOMMIT_FLAG) {
        if (!*size)
            return 1;
        end64 = (uint64_t)*base + *size;
        end64 = (end64 + VMEM_PAGE - 1) & ~(uint64_t)(VMEM_PAGE - 1);
        if (end64 > s_top)
            return 1;
        end = (uint32_t)end64;
        if (!range_owned_by(start, end, owner))
            return 1;
    } else {
        return 1;
    }

    for (p = page_index(start); p < page_index(end); ++p) {
        if (s_pages[p].state == MEM_COMMIT_FLAG)
            memset((void *)((uintptr_t)s_lo + (uintptr_t)s_offset
                            + (size_t)p * VMEM_PAGE), 0, VMEM_PAGE);
        if (free_type == MEM_RELEASE_FLAG) {
            memset(&s_pages[p], 0, sizeof s_pages[p]);
        } else {
            s_pages[p].state = (uint16_t)MEM_RESERVE_FLAG;
            s_pages[p].protect = 0;
        }
    }

    *base = start;
    *size = end - start;
    *status = 0;
    return 1;
}

int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                    uint32_t *status)
{
    int handled;

    if (!s_pages || *base < s_mirror_lo)
        return 0;
    AcquireSRWLockExclusive(&s_lock);
    handled = free_locked(base, size, free_type, status);
    ReleaseSRWLockExclusive(&s_lock);
    return handled;
}

/* A region nothing can be allocated in, reported as reserved. */
static void reserved_region(uint32_t info[7], uint32_t lo, uint32_t hi)
{
    info[0] = lo;                       /* BaseAddress */
    info[1] = lo;                       /* AllocationBase */
    info[2] = 0;                        /* AllocationProtect */
    info[3] = hi - lo;                  /* RegionSize */
    info[4] = MEM_RESERVE_FLAG;         /* State */
    info[5] = 0;                        /* Protect */
    info[6] = MEM_PRIVATE_TYPE;         /* Type */
}

int guest_vmem_query(uint32_t address, uint32_t info[7])
{
    guest_page_t first;
    uint32_t start, end;

    if (!s_pages || address < s_mirror_lo || address >= GUEST_VMEM_TOP)
        return 0;

    /* The mirrors are never tracked, so left alone a query reports them as one
     * free region, and a title's own address-space scanner (which uses this
     * query to find free space) keeps proposing an address inside it, being
     * refused, and proposing it again -- forever, on the title this came from.
     * Report them as reserved and the scanner moves past the top on its very
     * next query. The same goes for anything the host would not give us. */
    if (address < s_lo) {
        reserved_region(info, s_mirror_lo, s_lo);
        return 1;
    }
    if (address >= s_top) {
        reserved_region(info, s_top, GUEST_VMEM_TOP);
        return 1;
    }

    AcquireSRWLockExclusive(&s_lock);
    start = address & ~(VMEM_PAGE - 1);
    first = s_pages[page_index(start)];
    end = start + VMEM_PAGE;
    while (end < s_top) {
        const guest_page_t *next = &s_pages[page_index(end)];
        if (next->owner != first.owner || next->state != first.state ||
            next->protect != first.protect)
            break;
        end += VMEM_PAGE;
    }
    ReleaseSRWLockExclusive(&s_lock);

    info[0] = start;                                        /* BaseAddress */
    info[1] = first.owner;                                  /* AllocationBase */
    info[2] = first.reserve_protect;                        /* AllocationProtect */
    info[3] = end - start;                                  /* RegionSize */
    info[4] = first.state ? first.state : MEM_FREE_STATE;   /* State */
    info[5] = first.protect;                                /* Protect */
    info[6] = first.state ? MEM_PRIVATE_TYPE : 0;           /* Type */
    return 1;
}
