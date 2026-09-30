# Memory regressions

The guest heap and the memory bridge, checked against the bugs they had. Like
`tests/kernel_bridge`, it needs no title and no game files: it runs the real
memory layout on the small synthetic XBE from `tools/conformance` and calls the
kernel through the thunk dispatcher.

```
cmake -S tests/memory_regressions -B build/memory-regressions -A x64
cmake --build build/memory-regressions --config Release
ctest --test-dir build/memory-regressions -C Release --output-on-failure
```

One test per mode, one process each, because the switches are read once and cached:

| Test | What it checks |
|---|---|
| `memory_regressions_default` | No switch set. Pins what every title gets today: reuse hands a freed block over whole, contiguous memory is never reused, an explicit high base is not honoured. A change to the default fails here. |
| `memory_regressions_heap_reclaim` | `RECOMP_HEAP_RECLAIM=1`. The first table below. |
| `memory_regressions_ext_vma` | `RECOMP_EXT_VMA=1`. The second table. |
| `memory_regressions_ext_vma_128` | `RECOMP_EXT_VMA=1` on a 128 MB map. The tracker stays off. |

## `RECOMP_HEAP_RECLAIM`

Each check fails without the change and passes with it.

| Check | Without it |
|---|---|
| A 16-byte request after a 2 MB free takes 16 bytes | The whole 2 MB block, so a few thousand small requests drain the heap. |
| The rest of that block is still available | Not reachable: it went with the first request. |
| Freeing three neighbours in the order first, second, third merges all three | The third never joins: the merge of the first two left an empty slot that the neighbour search stopped at. |
| `NtFreeVirtualMemory(MEM_RELEASE)` on memory the heap supplied returns it | `STATUS_UNSUCCESSFUL`. The 32-bit guest slot went to the host `VirtualFree` as a pointer, so nothing came back. |
| A page `MEM_DECOMMIT` then `MEM_COMMIT` again reads as zero; its neighbours keep their data | The old contents: both calls were no-ops on heap memory, while the console hands recommitted pages back zeroed. |
| `MmFreeContiguousMemory` gives a contiguous block back and the next request of that size reuses it | Nothing came back: the address went to the general heap, which does not own it, so a title that frees and reloads a scene runs the 64 MB window out. The default mode pins the old bump allocator. |

## `RECOMP_EXT_VMA`

A title that reserves a specific address above the RAM mirrors (`0x74000000` up
on a 64 MB map) and checks it got that address back. Without the switch the
request is satisfied from the heap at a different address; the title notices and
fails, and touching the address it asked for faults.

| Check | Without it |
|---|---|
| Reserve at `0x76000000` returns `0x76000000` | The heap's address (`0x00F90000` in the run above). |
| Commit inside it succeeds and the memory is usable | Access violation: nothing backs that host address. |
| Query reports the committed part `MEM_COMMIT`, the rest `MEM_RESERVE`, one allocation base | "Everything above RAM is free", so a scanner is handed live memory. |
| Reserving the same range again is `STATUS_CONFLICTING_ADDRESSES` | A second grant. |
| An address inside the mirrors is refused, and queries report it reserved | It aliases low memory: a reservation there zeroed live `.rdata` on the title this came from. A scanner that is told "free" retries the same address forever. |
| `base = 0` still comes from the heap | (unchanged) |
| Release, then query, then reserve again works, and the memory is zero | (n/a) |

The 128 MB mode guards the case where the mirrors already reach past user space
(`128 MB * 29` is 3.7 GB): there is nothing above them to track, and a tracker
that subtracted anyway would ask for a range of negative size. It cannot fail on
v0.12.0, which has no tracker; it exists for this tracker.

The tracker reserves host memory at guest address plus the layout's offset, and
stops at the first thing the host has there. On Windows with the image mapped at
`0x10000` that is `KUSER_SHARED_DATA`, so the tracked range ends at
`0x7FFD0000`, not `0x7FFE0000`; the startup line says where it ended. If the host
range is taken earlier still, link the title with `/DYNAMICBASE:NO` so the
loader does not place modules in the low 2 GB.

## What it does not cover

Titles. It shows the allocator and the tracker do what they should when the
switch is set and what they did before when it is not; it does not show any
particular title runs better with them. Which titles free enough for the switch to matter, and
whether a title's addresses matter to it, is a per-title question.

The block table's lock is not exercised: nothing here allocates from two
threads.
