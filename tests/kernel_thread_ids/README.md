# Thread ids

`GetCurrentThreadId` (XAPI reads `fs:[0x28]` → ETHREAD.UniqueThread at +0x12C)
and the `ThreadId` that `PsCreateSystemThreadEx` writes for its caller, on
the real memory layout with real worker threads. Every id was 0 once; a
title that files its threads by id then deadlocks. Needs no title: it runs on
the synthetic XBE from `tools/conformance`.

```
cmake -S tests/kernel_thread_ids -B build/kernel-thread-ids
cmake --build build/kernel-thread-ids
ctest --test-dir build/kernel-thread-ids --output-on-failure
```
