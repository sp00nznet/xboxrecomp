#pragma once
/* Page tracker for guest virtual address space above physical RAM.
 *
 * The ordinary Xbox heap (xbox_HeapAlloc / xbox_ReserveAlloc) only ever
 * hands out addresses inside the mapped RAM range and never honors a
 * caller-supplied base address -- it just bump-allocates wherever its
 * cursor happens to be. That is fine for titles that always pass
 * base=NULL ("give me anything"), but a title that explicitly reserves a
 * specific high address and then verifies it got that exact address back
 * needs a real answer, not a different address silently substituted in.
 *
 * This tracks state per 4 KiB guest page for the extended range between
 * the top of RAM and the top of user address space, independently of the
 * physical-RAM heap, so a NtAllocateVirtualMemory that asks for
 * (say) 0x04000000 gets exactly 0x04000000 back -- or a real failure
 * status the caller's own retry logic can act on -- instead of a
 * different address it never asked for.
 */
#include <stdint.h>

int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                         uint32_t protect, uint32_t *status);
int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                     uint32_t *status);
/* info[7] = { BaseAddress, AllocationBase, AllocationProtect, RegionSize,
 *             State, Protect, Type } -- the 28-byte guest
 *             MEMORY_BASIC_INFORMATION layout, minus padding. */
int guest_vmem_query(uint32_t address, uint32_t info[7]);
