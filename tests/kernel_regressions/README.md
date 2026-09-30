# Kernel regressions

Three kernel fixes, each checked against the bug it fixed. Like
`tests/kernel_bridge`, it calls the kernel through the thunk dispatcher with a
plain buffer standing in for guest memory, so no title and no game files are
needed.

```
cmake -S tests/kernel_regressions -B build/kernel-regressions -A x64
cmake --build build/kernel-regressions --config Release
ctest --test-dir build/kernel-regressions -C Release --output-on-failure
```

Windows only, like `tests/kernel_irql_abi`: the pseudo-handle check needs
`DuplicateHandle`.

## What it guarantees

Each check fails without its fix and passes with it.

| Check | Without the fix |
|---|---|
| `xbox_RtlNtStatusToDosError(0xC0000018)` and bridge ordinal 301 return 487 | 317. 487 (`ERROR_INVALID_ADDRESS`) is the only error the MSVC CRT heap retries on when it grows, so the heap stopped growing. |
| `NtDuplicateObject` on `NtCurrentThread()` and `NtCurrentProcess()` succeeds | `STATUS_UNSUCCESSFUL`. Zero-extended on a 64-bit host the pseudo-handles are not pseudo-handles, and `DuplicateHandle` rejects them. |
| No call logging after 2^31 kernel calls | Every call after the wrap logs, because a wrapped negative count satisfies the log gate `count <= budget`. |

The third check drives 2^31 + 200,000 calls and takes tens of seconds, so it is
opt-in: configure with `-DXBOX_SLOW_TESTS=ON` (adds `kernel_regressions_wrap_test`)
or run `kernel_regressions_test wrap` directly.

## What it does not cover

The counter is private to the bridge, so the third check observes it through the
log it gates and cannot read it. The other fixes in the same change (thread-local
qualifier for MinGW, OHCI short-packet handling) need a different harness.
