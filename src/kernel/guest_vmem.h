#pragma once
/* Page tracker for guest address space above the RAM mirrors.
 *
 * The heap (xbox_HeapAlloc, xbox_ReserveAlloc) hands out addresses inside the
 * mapped range and never honours a caller-supplied base: it gives whatever its
 * cursor is at. That is fine for a title that passes base = NULL, but one that
 * reserves a specific high address and checks it got that address back needs a
 * real answer -- the address, or a failure status its own retry logic can act
 * on -- not a different address substituted in.
 *
 * This tracks state per 4 KiB page from the top of the RAM mirrors to the top
 * of user address space, backed by host memory reserved when the layout comes
 * up and committed only where a title commits.
 *
 * Opt-in, RECOMP_EXT_VMA=1. Without it nothing here is initialised and every
 * function answers "not mine" (returns 0), so the bridge behaves as before.
 *
 * Everything below g_memory_size (RAM, or the mapped size when a title asked
 * for one) is not this file's business. What lies between there and the top of
 * the mirrors aliases low memory, and is refused or reported reserved.
 */
#include <stddef.h>
#include <stdint.h>

/* End of user address space, rounded down to a 64 KiB allocation granule. Real
 * Xbox user mode ends at 0x7FFEFFFF; the kernel and system reserve the rest. */
#define GUEST_VMEM_TOP 0x7FFE0000u

/* Called by xbox_MemoryLayoutInit once the mirrors are mapped. `offset` is
 * host address minus guest address; the mirrors occupy [mirror_lo, mirror_top).
 * Does nothing unless RECOMP_EXT_VMA is set, or when the mirrors already reach
 * the top of user space (a 128 MB map), where there is nothing above them to
 * track. Returns 1 when the tracker is live. */
int guest_vmem_init(ptrdiff_t offset, uint64_t mirror_lo, uint64_t mirror_top);
void guest_vmem_shutdown(void);

/* True once guest_vmem_init has succeeded. */
int guest_vmem_active(void);

/* Each returns 0 when the request is not the tracker's (it is not live, or the
 * address is below the mirrors' start), in which case the caller carries on
 * with its own path. Otherwise they return 1 with *status set. */
int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                        uint32_t protect, uint32_t *status);
int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                    uint32_t *status);
/* info[7] = { BaseAddress, AllocationBase, AllocationProtect, RegionSize,
 *             State, Protect, Type } -- the 28-byte guest
 *             MEMORY_BASIC_INFORMATION layout. */
int guest_vmem_query(uint32_t address, uint32_t info[7]);
