# Xbox memory model on a POSIX host

No game files are required: a synthetic XBE exercises every mapping check. The
test takes the image path as its one argument, defaulting to
`tools/conformance/test.xbe`.

```
cmake -S tests/memory_layout_posix -B build/memory-layout
cmake --build build/memory-layout
ctest --test-dir build/memory-layout --output-on-failure
```

A real title is still worth running, because the model does more than read
section headers -- it copies each section's data to its Xbox VA, and some of
what lands there is load-bearing. The kernel thunk table sits in `.rdata`, and
a synthetic image has none, so only a real title shows whether imports resolve:

```
./build/memory-layout/memory_layout_posix_test game_files/default.xbe
```

Look for `Kernel thunks: N entries` in the output. `0 entries` on a real title
means the image did not arrive intact, whatever the section tally says.

If you have no XBE to hand, `tools/conformance/mkxbe.py` builds a synthetic one
(five framed leaf functions in a minimal header). It is not on every branch; the
test reports which path it could not read rather than failing obscurely.

No `-A x64` here, unlike the Windows regressions beside it: that flag selects an
MSVC generator and fails under Ninja or Unix Makefiles.

## What it pins down

The model maps 64 MB at a base address and then 28 mirror views of the same
pages at 64 MB intervals, because the console's 26-bit address bus wraps: guest
`0x04070000` has to read what `0x00070000` holds. Five properties, each naming a
distinct way that went wrong on arm64 macOS:

1. **`xbox_MemoryLayoutInit` succeeds.** Every entry in `try_bases[]` sits below
   4 GB, which on Apple Silicon is inside `__PAGEZERO` and unmappable, and the
   "let the OS choose" sentinel was never reached because the loop condition
   terminated on it rather than using it.
2. **Base and size are aligned to the *host* page.** The code says 4096 in
   places; Apple Silicon uses 16384. 4 KB is right as a *guest* constant and
   wrong as host granularity, and the two meanings are easy to conflate.
3. **All 28 mirrors alias the base, in both directions.** Reading through a
   mirror proves the mapping; writing through one and seeing it at the base
   proves it is shared memory rather than a copy, which is what a title's
   out-of-range write actually depends on.
4. **Shutdown then re-init works**, so teardown releases every view rather than
   leaving addresses claimed.
5. **`RECOMP_TRAP_NULL` actually traps, and leaves the TIB standing.** Both
   halves matter. The guard is applied at host page granularity, so on a
   16 KB-page host a request to protect guest page zero covers guest
   `0..0x3FFF`; with the TIB at `0x1000` that killed init, so the guard
   disabled itself and the diagnostic silently did nothing on every Apple
   Silicon host. Asserting only "page zero faults" would be satisfied by a fix
   that traps it and clobbers the TIB, which is why the TIB is checked too.

Deliberately not asserted: the tiled aperture at `0xF0000000`. That is a
specific architectural alias the layout protects by sacrificing a wrap mirror,
and whether it survives relocation to a high host base is a question about what
guest code assumes. Encoding a guess here would bake in an answer nobody has
established.

## Why the mirror probe forks

A mirror the layout could not place is unmapped, so reading it faults. Catching
`SIGSEGV` and recovering with `siglongjmp` works for one fault and dies when two
unmapped mirrors fall next to each other; asking `mach_vm_region` first reports
some of these addresses as mapped when reading them still faults. Reading in a
forked child cannot be wrong about it -- the child dies, the parent sees the
signal, and the other 27 mirrors still get checked.
