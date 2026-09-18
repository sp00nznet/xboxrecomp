# Kernel bridge

The dispatch path a title's kernel calls actually take. No game files and no
mapped guest window are needed -- a plain buffer stands in for guest memory.

```
cmake -S tests/kernel_bridge -B build/kernel-bridge
cmake --build build/kernel-bridge
ctest --test-dir build/kernel-bridge --output-on-failure
```

Runs on both hosts, unlike the `_posix` projects beside it. Nothing in it is
platform-specific: no `fork`, no signals, no POSIX headers -- just
`VirtualAlloc`, stdio, and the kernel's own API, which resolves to
`<windows.h>` on Windows and to the shim in `platform/xbox_winnt.h`
elsewhere. Add `-A x64` when configuring alongside the Windows regressions.

What does differ per host is `xbox_kernel_bridge_init()`'s `VirtualProtect`
over the thunk region -- real on Windows, shimmed here -- and the first check
below fails if that rewrite does not happen, which is a reason to run this in
both places rather than only one.

## What it guarantees

Four checks, all on ordinal 173, `MmGetPhysicalAddress`.

**A thunk entry resolves to a bridge entry point.** Given an import in the
form an XBE stores it (`0x80000000 | ordinal`), `xbox_kernel_bridge_init()`
rewrites the entry in place to a synthetic VA, and `recomp_lookup_kernel`
turns that VA back into a callable dispatcher.

**Ordinal 173 translates through the contiguous window.** Dispatched with a
guest stack, an address inside
`[XBOX_CONTIG_BASE, XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)` returns its physical
offset. DMA consumers are programmed with the result, so a virtual-window
address reaching one is not a value that faults -- it is one the NV2A reads as
a physical address with bit 31 set.

**Ordinal 173 pops its stdcall argument.** After the call `esp` has advanced
past the return address and the four bytes of argument. A wrong
`stdcall_args_for_ordinal` entry corrupts the caller's frame some distance
from the call, with nothing naming the ordinal.

## Why this is a separate project

Two reasons, one practical and one about what the checks are worth.

`xbox_kernel_bridge_init()` has process-wide side effects: it rewrites a thunk
table in guest memory and installs slot state that later calls read. That does
not belong partway through a run of unrelated API checks.

More importantly, `tests/kernel_memory_posix` calls `xbox_MmGetPhysicalAddress`
directly, and that is not how a title reaches it. Ordinal 173 previously had
two implementations that disagreed -- the bridge translating, and
`xbox_MmGetPhysicalAddress` returning its argument unchanged as a placeholder.
The bridge now calls the latter, so the arithmetic exists once; but a bridge
that stopped delegating would leave every check in that project green. This
one fails instead, which was measured rather than assumed.

## How the bridge is reachable at all

`bridge_*` functions are `static`, and `kernel_thunk_dispatch` reads guest CPU
state (`g_kernel_dispatch_slot`, the guest stack) rather than taking
arguments, so neither can be called directly. `recomp_lookup_kernel` is not
static: given a synthetic VA it selects the slot and returns the dispatcher.
Set `g_xbox_mem_offset` to a buffer, lay out a guest stack, set `g_esp`, call.

`tests/kernel_directory` uses the same seam for the directory ABI, and is the
older example of the pattern -- it is a Windows test, which is the precedent
for this one building there.

## The stubs at the top of `test_main.c`

`recomp_lookup` and `recomp_lookup_manual` are provided by the generated
title. A regression harness has no title, and a static archive resolves only
what gets pulled in, so reaching the kernel bridge needs stand-ins. Nothing
here calls a guest function.
