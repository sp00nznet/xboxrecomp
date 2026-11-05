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
| `memory_regressions_default` | No switch set. Pins what every title gets today: reuse hands a freed block over whole, a freed contiguous block is reused (this fork's first-fit contiguous arena works in every mode), an explicit high base is not honoured. A change to the default fails here. Every mode but `heap_quarantine` also checks that a page allocation owns whole pages: `NtAllocateVirtualMemory` of 0x46500 bytes reports 0x47000, and the next small block starts past it. Before, it started in the last page, so when JSRF made that buffer read-only, zeroing the next block faulted on 4 KB host pages (Proton). |
| `memory_regressions_reclaim_zero` | `RECOMP_HEAP_RECLAIM=0`, which must mean off: a game may default the switch on, and 0 is how a run turns it back off. The default mode's checks. |
| `memory_regressions_heap_reclaim` | `RECOMP_HEAP_RECLAIM=1`. The first table below. |
| `memory_regressions_heap_quarantine` | `RECOMP_HEAP_RECLAIM=1` with `RECOMP_HEAP_QUARANTINE=64`. The quarantine table below. |
| `memory_regressions_contig_avoid_image` | `RECOMP_CONTIG_AVOID_IMAGE=1`, the default spelled out: a contiguous block lands outside the loaded image, as in every mode but the next. |
| `memory_regressions_contig_fill_image` | `RECOMP_CONTIG_AVOID_IMAGE=0`: a contiguous block fills from the bottom through the image again, the order before the default changed. |
| `memory_regressions_ext_vma` | `RECOMP_EXT_VMA=1`. The second table. |
| `memory_regressions_ext_vma_128` | `RECOMP_EXT_VMA=1` on a 128 MB map. The tracker stays off. |

## `RECOMP_HEAP_RECLAIM`

Each check fails without the change and passes with it.

| Check | Without it |
|---|---|
| A 16-byte request after a 2 MB free takes 16 bytes | The whole 2 MB block, so a few thousand small requests drain the heap. |
| The rest of that block is still available | Not reachable: it went with the first request. |
| Four 256-byte requests after four 1 MB blocks are freed still leave a 768 KB request room inside them | It lands above them: each small request took a whole 1 MB block. This is how JSRF ran out of heap loading `Misc.dat`, with half the heap held as slack in reused blocks. |
| Freeing three neighbours in the order first, second, third merges all three | The third never joins: the merge of the first two left an empty slot that the neighbour search stopped at. |
| With the heap filled to 1 MB short of the top, two 4 MB blocks freed at the cursor (merged into one 8 MB tail) hold a 9 MB request at their address; with room above the cursor, a 4 MB request after a 3 MB tail is freed still bumps past it | The 9 MB request fails: it bumped from the cursor, past a free run it could have used. KUF freed 32 MB at the cursor, asked for 33558528 bytes, and failed with a 44.7 MB free run. The bump-past half holds in every mode, so a title that works today keeps its addresses. |
| `NtFreeVirtualMemory(MEM_RELEASE)` on memory the heap supplied returns it | `STATUS_UNSUCCESSFUL`. The 32-bit guest slot went to the host `VirtualFree` as a pointer, so nothing came back. |
| A page `MEM_DECOMMIT` then `MEM_COMMIT` again reads as zero; its neighbours keep their data | The old contents: both calls were no-ops on heap memory, while the console hands recommitted pages back zeroed. |
| `MmFreeContiguousMemory` gives a contiguous block back and the next request of that size reuses it | Nothing came back: the address went to the general heap, which does not own it, so a title that frees and reloads a scene runs the 64 MB window out. In this fork the contiguous arena is first-fit in every mode, so the default mode checks reuse too. |

## `RECOMP_HEAP_QUARANTINE`

Freed heap blocks wait in a bounded FIFO before they can be handed out again,
so a stale pointer reads the old contents instead of whatever reused the block.
JSRF kept a record inside a freed block, and a sound bank loaded over it, which
hung the game. The switch is opt-in. The fix belongs in the title path
that frees the record; the quarantine is a safety net. The test holds 64 KiB so
its arithmetic stays small; a game run wants a few MiB (8192 suggested), so the
blocks freed alongside a stale one fit in the queue with it.

| Check | Without it |
|---|---|
| Freeing the same block twice holds it once | The block sits in the queue twice, so after the first copy goes back the second frees a block that may already be reused. |
| A freed block keeps its contents and the next request does not get it | The next request of that size gets the block back at once. |
| Past 64 KiB the oldest block goes back first | (n/a: nothing is held) |
| A block larger than the limit is freed at once; the held blocks stay | One big free (a scene buffer at teardown) evicts every held block first, the stale record among them. |
| A request that would run out of heap drains the queue and is satisfied | (n/a) |

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
