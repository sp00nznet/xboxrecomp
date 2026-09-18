# Kernel memory APIs on a POSIX host

The `Mm*` and `Nt*` memory calls a title makes, checked against the Win32 shim
underneath them. No game files and no mapped guest window are needed, which is
why this is separate from `memory_layout_posix` next door.

```
cmake -S tests/kernel_memory_posix -B build/kernel-memory
cmake --build build/kernel-memory
ctest --test-dir build/kernel-memory --output-on-failure
```

## What it guarantees

Six properties over 15 checks.

**Allocation and release pair up.** A buffer from
`MmAllocateContiguousMemory` can be passed to `MmFreeContiguousMemory` without
killing the process. The free path chooses between `VirtualFree` and
`_aligned_free` by asking `VirtualQuery` which allocator owns the pointer, and
the two allocators are not interchangeable here: `VirtualAlloc` is `mmap`, and
`_aligned_free` is `free()`, which aborts on a pointer it did not hand out.

**`VirtualQuery` answers about the memory the shim actually mapped.** For a
region from `VirtualAlloc` or `MapViewOfFileEx` it reports the allocation's
base address, a region size running from the queried address to the end of
that region, and `MEM_COMMIT`. Interior addresses resolve to the region that
contains them, not just exact base addresses.

**`VirtualQuery` does not invent mappings.** An address the shim never mapped
is reported `MEM_FREE`, so a guest that probes memory before writing to it can
be told no.

**`MmQueryAllocationSize` agrees with `VirtualQuery`** rather than answering a
fixed page size for every pointer.

**`MmGetPhysicalAddress` translates through the contiguous window.** An address
inside `[XBOX_CONTIG_BASE, XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)` returns its
physical offset; anything outside passes through unchanged. Both ends of the
window and both sides of its upper bound are checked. DMA consumers are
programmed with the result, so a virtual-window address reaching one is not a
value that faults -- it is one the NV2A reads as a physical address with bit 31
set.

**There is one implementation of ordinal 173.** `bridge_MmGetPhysicalAddress`
calls `xbox_MmGetPhysicalAddress` rather than repeating its arithmetic, so the
property above cannot hold on one dispatch path and not the other.

## What it does not cover

The bridge. These checks call the `Mm*` and `Nt*` functions directly, which is
not how a title reaches them -- its indirect calls go through
`bridge_MmGetPhysicalAddress`, which delegates here. A bridge that stopped
delegating would leave every check in this project green.

That path is covered by `tests/kernel_bridge`, which drives the same
ordinal through the dispatcher. It lives apart because
`xbox_kernel_bridge_init()` has process-wide side effects.

## Notes on the harness

**The first check forks.** Its failure mode is the allocator calling `abort()`,
which is a SIGABRT rather than a return value: in-process there is nothing to
inspect, and the crash would take the remaining fourteen checks with it. The
child's exit status distinguishes the cases and the harness names the signal,
because "it crashed" and "the allocator rejected the pointer" are different
findings.

**The stubs at the top of `test_main.c`.** `recomp_lookup` and
`recomp_lookup_manual` are provided by the generated title. A regression
harness has no title, and a static archive resolves only what gets pulled in,
so reaching the kernel bridge needs stand-ins. Nothing here calls a guest
function.
