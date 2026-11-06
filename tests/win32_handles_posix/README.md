# Win32 handles on a POSIX host

A HANDLE the shim issues has to survive being stored by a 32-bit guest. No
game files needed. POSIX only: a Win32 HANDLE is a handle-table index and fits
by construction, so there is nothing here for Windows to check.

```
cmake -S tests/win32_handles_posix -B build/w32-handles
cmake --build build/w32-handles
ctest --test-dir build/w32-handles --output-on-failure
```

## What it guarantees

**Every handle fits in 32 bits.** Events, semaphores, mutexes, files and
directories. A recompiled title is 32-bit code, so every handle it receives
goes into a 32-bit slot -- in a guest struct, on the guest stack, in a kernel
object field. The kernel bridge does it literally: `sp[1] =
(uint32_t)(uintptr_t)h`.

**A handle survives that truncation.** Checked separately, because fitting and
surviving are different claims and a fix satisfying only the first would be
wrong. The handle is put through `(uint32_t)(uintptr_t)h` and then used --
`SetEvent`, `WaitForSingleObject`, `WriteFile` -- and must still name the same
object.

**A closed handle's slot is reused.** An index is a finite resource in a way a
pointer is not. A title opening and closing handles in a loop would walk off
the end of the table within minutes if `CloseHandle` did not free the slot;
10000 create/close cycles must all succeed and stay at the same index.

## Why the round-trips fork

Using a truncated handle dereferences an address that was never an object, so
the failure is a SIGSEGV rather than a return value. In-process it takes the
whole run with it and reports nothing about the handle types after it -- the
first version of this test died at check 2 with exit 139. The child inherits
the handle across `fork`, so it is the same object either way, and the harness
names the signal.

## Background

The shim used to return the `w32_object` pointer as the handle, and macOS heap
allocations sit above 4 GB: `0x1038dffd0`. The guest kept the low half, handed
it back, and the shim dereferenced `0x038dffd0`. Nothing reported a bad handle,
because from the shim's side a handle is just a pointer.

`tests/kernel_directory` is where it surfaced. It guards its own setup with
`(uintptr_t)h > UINT32_MAX` and exited 11 rather than running -- the whole
directory ABI it covers was unreachable on this host for that one reason.
