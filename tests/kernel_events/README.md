# Kernel events

KEVENTs a title builds by writing the header itself, with no
`KeInitializeEvent` call, checked through the thunk dispatcher. Like
`tests/memory_regressions`, it needs no title and no game files: it runs the
real memory layout on the small synthetic XBE from `tools/conformance`.

```
cmake -S tests/kernel_events -B build/kernel-events -A x64
cmake --build build/kernel-events --config Release
ctest --test-dir build/kernel-events -C Release --output-on-failure
```

| Test | What it checks |
|---|---|
| `kernel_events_default` | No switch set. Pins what every title gets today: the guest address goes to Win32 as a `HANDLE`, so after `KeSetEvent` the wait still fails. |
| `kernel_events_title_kevents` | `RECOMP_TITLE_KEVENTS=1`. The table below. |

Each check fails on v0.12.0 (every wait returns `0xC0000001`) and passes with
the switch:

| Check | Why it matters |
|---|---|
| An unset synchronization event times out | A zero timeout must report the state, not an error. |
| `KeSetEvent` sets it; the wait succeeds and clears `SignalState` | A synchronization wait consumes the signal, in guest memory too. |
| A second wait times out again | The same. |
| `SignalState` written to 1 by the title counts | XDK code sets and clears events by writing the header. |
| A notification event stays set across two waits | Notification events are not consumed. |
| `KeResetEvent` clears it | Host event and guest header agree. |

Not covered: a wait that really blocks and is released from another thread
(the bridges use a host event for exactly that, so a zero timeout checks the
same state), and any title.
