# Threads and waits for the in-process reset

Audit for the game-reset phase 0 (tasks 0.3 and 0.4). It was read on
`feat/game-reset-p0` at 11e099c2 of the toolkit. All file:line references are
relative to `src/`. A `~` marks a line that was read from a range and not
pinned exactly.

Pause column legend:

- **pause flag**: the thread checks a reset-pause flag at its loop head and
  parks there.
- **not needed**: the thread does not touch guest state.
- **keep running**: the thread must keep serving the host, for example the
  window or the audio device.
- **guest thread: unwinds**: the thread runs guest code and leaves by the
  unwind (D3).

## 1. Host threads

| Role | Creator | Loop head | Guest memory / guest code | Pause |
|---|---|---|---|---|
| `guest-worker` | kernel/kernel_bridge.c:978 `bridge_spawn_thread` -> `bridge_thread_main`:890 (role set at 898) | runs `bridge_run_thread_inline`:870, then `bridge_thread_end(0)` | guest code (thread entry) | guest thread: unwinds |
| `guest-main` | role at kernel/kernel_bridge.c:1123 (ordinal 255, first call, INLINE mode) and 10152 (ordinal 254, `bridge_PsCreateSystemThread`:10125); on SDL builds the host thread is `guest_thread` (video/fb_present_sdl.c:359, started at 465) | runs fn() inline inside the bridge under `xbox_GuestCpuEnterCallback` | guest code | guest thread: unwinds (boot thread) |
| `vblank-ack` | kernel/kernel_bridge.c:3360 `kernel_vblank_ack_init` (INIT_ONCE `g_vbl_ack_once`) -> `kernel_vblank_ack_thread`:3292 | `for(;;)` 3299: `WaitForSingleObject(g_vbl_ack_event, INFINITE)` at 3303, then `xbox_VblankAckWait`:3242 (1/16 ms steps, WaitForSingleObject at 3269) | interlocked clears of PCRTC_INTR_0/PMC_INTR_0 in guest NV2A regs; **no guest code** | pause flag |
| `kernel-timer` | kernel/kernel_bridge.c:4511 `kernel_timer_start` -> `kernel_timer_thread`:4423 | legacy 4437 (Sleep(10) at 4441); `kernel_timer_loop_ms`:4258 (loop 4262, Sleep at 4269/4274); `kernel_timer_loop_ns`:4298 (loop 4304, `xbox_HostTimerSleepNs`); `kernel_timer_loop_det`:4359 (loop 4363, `xbox_GuestDetDeviceWait`) | **runs guest code**: DPCs (`kernel_drain_dpcs`), the vblank ISR and DPCs (`kernel_vblank_raise`:3623 -> `kernel_vblank_deliver`), timer DPCs in `kernel_fire_timers` (writes SignalState at timer+4), `xbox_SpinWake`; own worker stack and TIB | pause flag at each loop head, plus an unwind point for a guest call-in (task 1.4) |
| `nv2a-ack` (pushbuffer walker) | kernel/xbox_memory_layout.c:2800 `xbox_Nv2aAckStart` -> `nv2a_ack_thread`:2377 | `while(!g_nv2a_ack_stop)` ~2386; Sleep(1) at 2396/2669/2750, fast-kick naps, `xbox_KickWakeSleep` 2732; det: `xbox_GuestDetDeviceWait` | writes guest regs, GET and fences (`fence_mirrors_tick`:1420), `dsp_ack_tick`:1098, `poke_tick`:1082, `counter_mirrors_tick`, KeTickCount; runs the D3D11 or Metal backend: Metal `waitUntilCompleted` at d3d/nv2a_pb_metal.m:364 and 2774; on Windows it also owns the D3D11 window and device (d3d/nv2a_pb_d3d11.c: PeekMessage 403, CreateWindowW 629/636) | pause flag between segments; on Windows the message pump must keep running while paused |
| `nv2a-kick` | kernel/xbox_memory_layout.c:2368 `xbox_Nv2aKickStart` (only with RENV_FLIP_LOG) -> `nv2a_kick_thread`:2246 | loop 2251 | reads the kick register only | not needed (debug) |
| raster workers (CPU backend) | kernel/nv2a_pb_exec.c:5039 (Win) / 5051 (POSIX) `raster_worker` | WaitForSingleObject 4987 / `pthread_cond_wait` 5002 | write the host-side raster only; the walker waits for them at 5107/5117 | not needed (they idle while the walker is paused) |
| `mcpx.apu_thread` | apu/apu_core.c:1657 `qemu_thread_create` (CreateThread in apu/apu_shim.h:138) | `while(!exiting)` 1353; cond wait 1409; `pause_requested` loop 1245 | guest audio RAM (voice lists, SGE buffers); no guest code | pause flag: the existing `mcpx_apu_wait_for_idle`:1543 (`pause_requested` + `idle_cond`) is the pause |
| DSP GP/EP workers | apu/apu_dsp_run.c:307 `worker_main` | cond wait 231 (cv at 244/313/362) | DSP state; guest RAM via the APU | follow the APU frame thread (pause with it) |
| APU MMIO trace reporter | apu/apu_mmio_hook.c:185 and 377 (`apu_trace_reporter`, nanosleep 10 s at 348; Sleep(10000) at 160) | `for(;;)` | stats only | not needed (debug) |
| `ohci` | usb/ohci.c:1872 -> `ohci_thread`:1486 | `for(;;)` ~1512, `ohci_pass_begin`; Sleep(1) 1406, Sleep(OHCI_TICK_MS) 1423; det device | **runs guest code**: the guest USB ISR via `recomp_lookup` (ohci.c:1090-1110, under `xbox_IrqlEnterInterrupt`), `xbox_IrqPost`:1709; guest OHCI descriptors; own TIB (`ohci_thread_tib`) | pause flag, plus an unwind point for the ISR call-in |
| `pad_quit` | usb/usb_gamepad.c:648 -> `pad_quit_thread`:611 | one-shot: exit hooks, then `ExitProcess` | none | not needed (scripted runs only) |
| `vp-pump` (FMV) | video/video_pump.c:178 `xbox_VideoPlayFile` -> `vp_thread`:61 | `while(s_playing)` 113: PeekMessage, video update and render, Sleep(1); Windows only (POSIX stub at 193) | own window and `xbox_Direct3DCreate8` device (HLE D3D8); no guest memory | stop the player (RECOMP_FMV_HOST) before the reset |
| `fb-present` | video/fb_present.c:707 -> `fb_thread`:536 | `while(s_fb_running)` ~611: PeekMessage, Sleep(16) | reads the guest framebuffer `s_fb_va` | keep running (read-only; a torn frame at most) |
| `sdl-main` | process main thread; role at video/fb_present_sdl.c:953 | `SDL_WaitEventTimeout(…,100)` ~970 | none | keep running (the reset is asked from here) |
| host-open | input/host_open.c:43 `open_thread` (Windows ShellExecute, one-shot); on POSIX the window thread runs it (`xbox_HostOpenSdlSync`) | — | none | not needed |
| watch re-protect | kernel/xbox_memory_layout.c:3479 (POSIX) -> `watch_posix_reprotect` | `for(;;)` ~3437, nanosleep 200 us at 3439 | mprotects the watched guest page | not needed (debug, RECOMP watch) |
| watch resolver | kernel/xbox_memory_layout.c:3565 | Sleep(20) at 3537 | reads guest memory | not needed (debug) |
| watch rearm | kernel/xbox_memory_layout.c:3625 | Sleep(20) at 3208 | re-arms the guard | not needed (debug) |
| `watchdog` | kernel/xbox_memory_layout.c:3796 -> `xbox_watchdog_thread` (role at 3682) | one `Sleep(s_watchdog_secs*1000)` at 3687, then a dump and exit | reads only | pause or disarm: it would fire during a long reset |
| profilers / samplers | platform/host_prof.c:561/571, platform/mem_stats.c:458/465, kernel/kernel_prof.c:222 | timed sleeps | none | not needed |
| Win32-shim timer and threadpool threads | platform win32_compat.c:1266 `timer_thread`; 1315 threadpool trampoline | — | only `xbox_KeSetTimerEx` (kernel_sync.c:540) and `xbox_KeInsertQueueDpc` use them, and only through the dead native table (section 2) | not needed (unreachable) |
| audio device threads | SDL audio stream (apu/apu_sdl.c:132); XAudio2 voice callbacks (apu/apu_xaudio2.c) | library-owned | host ring only | keep running (silence while the APU is paused; `xa2_expect_drain` marks it as a drain, not an underrun) |

Signal and fault handlers run on the guest thread that faults, so they are
not separate threads. They are: `watch_posix_handler` (xbox_memory_layout.c
~3390-3473), the Metal guard handler (d3d/nv2a_pb_metal.m:2881-2950), the
APU MMIO hook (apu/apu_mmio_hook.c:289, sigaction) and the AC97 VEH
(xbox_memory_layout.c:837). None of them blocks, except the APU MMIO hook,
which takes the APU lock (section 3).

Spec correction: design.md Facts says `vblank-ack` "delivers the vblank DPCs
into guest code". It does not. It only acks the interrupt bits. The
`kernel-timer` thread raises the vblank and runs the ISR and the DPCs.

## 2. Guest-thread creators

- `xbox_PsCreateSystemThreadEx` is in kernel/kernel_thread.c:77. It calls
  CreateThread at 119 and has the role `xbox-thread` at 40.
  - Its only reference is kernel/kernel_thunks.c:266, in
    `xbox_resolve_ordinal`, case 255.
  - That table is filled only by `xbox_kernel_init` (kernel_thunks.c:395),
    into the host array `xbox_kernel_thunk_table`.
  - Nothing reads that array. A grep across the toolkit src, tools and
    tests and the game repos found only tools/validate_ordinals.py.
  - The games' `host_main` calls `xbox_kernel_init()`. Only the fill runs.
  - **The `xbox-thread` path is unreachable.**
- What the guest calls instead:
  - `xbox_kernel_bridge_init` (kernel_bridge.c:12052) patches the guest thunk
    table with synthetic VAs (0xFE000000 + i*4).
  - `recomp_lookup_kernel` returns `kernel_thunk_dispatch`
    (kernel_bridge.c:11729).
  - Ordinal 255 goes to `bridge_PsCreateSystemThreadEx`
    (kernel_bridge.c:1078; table entry at 11127; 40 arg bytes at 10839).
  - Ordinal 254 goes to `bridge_PsCreateSystemThread` (10125; table entry at
    11632).
- Thread modes:
  - **INLINE (default, BLiNX):** the first call (the boot thread) runs inline
    as `guest-main`. Later calls go to `bridge_spawn_thread` and become
    `guest-worker`.
  - **SPAWN (`xbox_SetThreadMode`, Burnout 3):** the first call spawns a
    thread and the entry returns.
  - **Inline fallbacks run a worker on the caller's own thread:**
    `RECOMP_WORKERS=inline` (~1180 and 10168), running out of thread stacks
    (~1195-1202), and `bridge_run_thread_inline`.
    - The unwind must not stop at the inner entry. It has to reach the
      caller's frame (D3).
- Direct calls in game code: the game repos have no direct call to
  `xbox_PsCreateSystemThreadEx` or to CreateThread for guest code.
  - Every guest thread starts through the ordinal 254/255 bridges.
- Host-side call-ins into guest code that are not guest threads:
  - `kernel-timer` runs DPCs, the vblank ISR and timer DPCs.
  - `ohci` runs the USB ISR.
  - Both have their own TIB and stack.
  - Each also enters through the user APC dispatcher on the waiting guest
    thread (`bridge_NtUserIoApcDispatcher`:2619).

## 3. Host blocking on a guest thread outside a wait bridge

| Site | Waits on | Max duration | Way out | Owner | Change outside module? |
|---|---|---|---|---|---|
| guest CPU acquire, kernel/kernel_guest_cpu.c:589 (sched queue) and 961 (ticket queue); entry `xbox_GuestCpuAcquire`:1260 | the guest CPU token | unbounded (2 ms poll, `GUEST_CPU_POLL_MS`) | the holder releases at a back edge or kernel call; the poll lets a reset flag be checked | kernel (guest_cpu) | no: a check inside the poll loop |
| det turn `wait_turn` kernel/kernel_guest_det.c:634 (cond 1000 ms at 639) | the det ring token | unbounded (1 s slices) | the token holder hands on | kernel (guest_det) | no |
| det device wait `xbox_GuestDetDeviceWait`:829 (cond INFINITE at 833, `while(!grant)` 832) and `device_round` ~850 (cond INFINITE at 864, `while(!done)`) | a det device grant or round | unbounded, **INFINITE** | the ring grants it | kernel (guest_det) | no, but an INFINITE wait needs a broadcast from the reset |
| `xbox_SpinWait` kernel/kernel_pacing.c:280 (cond at 250) | a spin wake generation | 1 ms (`SPIN_TIMEOUT_MS`) | timeout, then guest CPU acquire | kernel (pacing) | no |
| `xbox_RtlEnterCriticalSection` kernel/kernel_rtl.c:482: `EnterCriticalSection(cs)` at 530; det path `xbox_GuestDetBlock` at 525 | the host CS shadow of a guest CS | **unbounded**; the holder is another guest thread | the holder leaves the CS | kernel (rtl) | **yes, in effect**: a holder that unwinds never leaves, so the waiter blocks for good. Fix: a timed or try-loop enter with a reset check in kernel_rtl, or drop the shadows before releasing the waiters |
| `xbox_KeDelayExecutionThread` kernel/kernel_thread.c:176: virtual-time loop of Sleep/SleepEx(1) ~214/217; real-time `SleepEx(ms)`/`Sleep(ms)` ~245/249 | the guest's interval | the real-time path is **as long as the guest asks** (one Sleep) | timeout; alertable SleepEx by APC only | kernel (thread) | no: split into slices with a check |
| `xbox_NtYieldExecution` kernel_thread.c:420 / SwitchToThread | — | one time slice | — | kernel | no |
| APU MMIO from guest code (signal hook apu/apu_mmio_hook.c:289 -> `mcpx_apu_mmio_write`/`read`, apu_core.c:1747/1758, takes `d->lock`) | the APU lock while the frame thread holds it | about one APU frame | the frame thread releases | apu | no, but **the APU pause must not hold `d->lock` while waiting for the guest**. `mcpx_apu_wait_for_idle` waits on `idle_cond`, which drops the lock, so it is fine as written |
| D3D KickOff flush spin (comment at kernel/xbox_memory_layout.c:1762-1800) | guest code spinning on PFB 0x100410 bit 16 until the walker clears it | until the walker's next tick | a back edge or SpinWait stop point inside guest code | nv2a (walker) | **yes**: if the walker is paused before the guest thread stops, the spin keeps waiting. It is guest code, so the back-edge stop point covers it **only if the stop is taken before the walker pause** |
| GPU-interrupt wait in MakeSpace (same comment) | guest wait on a NOP-with-notify event | unbounded if the walker stops | the walker raises it | nv2a | same ordering point as above: stop the guests first, then the walker |
| `hud_capture` frame wait enhance/hud_capture.c:599 (nap 1 ms, up to `CAP_IDLE_MS`=200) and the file lock at ~694 | the walker going idle | 200 ms; the lock is one frame | timeout | enhance (capture tool, opt-in) | no |
| `kernel_file.c:1725` fsync (NtFlushBuffersFile) | the disk | disk-bound (ms to s) | completion | kernel (file) | no; let it finish |
| file read and write bridges (NtReadFile 5442, NtWriteFile 5517, scatter and gather) | synchronous host read() and write(), or the RAM cache | disk-bound | completion | kernel (file) | no |
| `bridge_HalReturnToFirmware` FMV wait (kernel_bridge.c:~2100-2117, Sleep(50) at 2112) | the FMV player ending | 60 s | timeout | kernel | n/a: the reset refuses during FMV anyway |
| stubs that do not wait | `KeRemoveQueue` 9636, `NtSignalAndWaitForSingleObjectEx` 10020 (both return 0 at once); mutant and semaphore create and release (6346/6379/6653/6669) do not block | — | — | kernel | — |

`NtSuspendThread` (6455, `xbox_GuestSchedSuspend`) parks the *target* thread
inside the guest CPU acquire (sched). The way out is the same check as the
first row. A suspended thread must also be counted as stopped.

## 4. Wait bridges

"Unwind return" is where a `xbox_ResetUnwinding()`-style check sits: right
after the host wait returns and before the first guest-state change. **C**
marks a wait whose signal is consumed inside the host primitive. There, a
wake by the reset that returns success would already have taken a guest
signal.

| Bridge | Host wait | First guest-state change after | Unwind return | Consumes |
|---|---|---|---|---|
| `bridge_KeWaitForSingleObject` 2358, guest-event path | 2376 `xbox_KeWaitForSingleObject` | 2379 `BRIDGE_MEM32(object+4)=0` (sync event cleared), then 2380 `bridge_user_apc_finish` (APC drain, eax) | between 2376 and 2379 | **C** (host auto-reset) plus the guest clear at 2379 |
| `bridge_KeWaitForSingleObject`, other objects | 2393 | 2395 apc_finish | between 2393 and 2395 | **C** |
| `bridge_NtWaitForSingleObject` 2405 | 2415 | 2417 apc_finish | between 2415 and 2417 | **C** |
| `bridge_NtWaitForSingleObjectEx` 2463 | 2481 | 2483 apc_finish | between 2481 and 2483 | **C** |
| `bridge_NtWaitForMultipleObjectsEx` 2521 | 2559 | 2561 apc_finish | between 2559 and 2561 | **C**; WaitAll consumes partially (below) |
| `bridge_KeWaitForMultipleObjects` 6695 | 6732 | 6743 `BRIDGE_MEM32(guest_events[i]+4)=0`, then 6745 apc_finish | between 6732 and 6743 | **C** plus the guest clears |
| `bridge_KeDelayExecutionThread` 2660 | 2666 `bridge_user_apc_wait` (delivers APCs at once), then 2669 `xbox_KeDelayExecutionThread` (kernel_thread.c:176) | the return (eax) | before APC delivery at 2666 and after 2669 | no (timer only) |
| `bridge_NtYieldExecution` 2700 | kernel_thread.c:420 | eax | after | no |
| `bridge_RtlEnterCriticalSection` 2183 (and `…AndRegion` 8274) | kernel_rtl.c:525/530 | `xbox_cs_note_owner` (owner recorded) | before note_owner; the acquired CS must then be released | **C** (ownership) |
| `bridge_RtlTryEnterCriticalSection` | none: kernel_rtl.c `xbox_RtlTryEnterCriticalSection` never waits | `xbox_cs_note_owner` on success (the Enter/Leave shadow) | — | no (no wait) |
| `bridge_ExAcquireReadWriteLock*` 8717/8724 | maps to a guest-side lock or sync wait | eax | after | per object |
| `bridge_IoSynchronousDeviceIoControlRequest` 7070 | device dispatch, no host wait found | — | — | — |

Below the bridges, every wait funnels through kernel_sync.c `sync_wait_one`
(250) and `sync_wait_many` (276), called by `xbox_NtWaitForSingleObject`
303, `…Ex` 313, `xbox_NtWaitForMultipleObjectsEx` 333,
`xbox_KeWaitForSingleObject` 357 and `xbox_KeWaitForMultipleObjects` 373.
They have three modes:

1. **det:** `sync_det_wait` -> `xbox_GuestDetBlock`, polling with zero-timeout
   WaitFor*. **C**: each successful poll consumes.
2. **virtual due:** a 1 ms slice loop. It already has a natural place for a
   reset check.
3. **guest_sched:** a zero poll, then `xbox_GuestCpuBlocked`, then
   `WaitForSingleObjectEx`/`WaitForMultipleObjectsEx`. **C**.

On POSIX the shim (platform win32_compat.c) does the following:

- `wait_single` consumes in its post-loop switch (~690-700): auto-reset
  events, semaphore counts and mutex ownership.
- `WaitForMultipleObjectsEx` (761) is a 1 ms usleep poll loop. With waitAll
  it polls each handle with `WaitForSingleObject(h,0)`. That consumes the
  ready members before all of them are ready. **This is an existing bug,
  independent of the reset.**

What this means for the unwind:

- **The wake event:** a reset wake handle cannot simply be added to a WaitAll
  wait, since WaitAll would then need the reset handle as well. Nor can it be
  added to a wait that already has 64 handles (MAXIMUM_WAIT_OBJECTS).
  - Prefer a wake that the 1 ms POSIX poll and the virtual-time slice loops
    check as a flag.
  - On native Windows, use an APC (`QueueUserAPC`) to the waiting thread, so
    the alertable waits return, or a reset event added only to WaitAny
    waits with count < 64.
  - Non-alertable WaitAll and INFINITE waits need a timed slice loop.
- **The unwind order:** after a wake by the reset, the unwind return must
  come before the guest clears (2379, 6743) and before
  `bridge_user_apc_finish`. A signal consumed in the host primitive is lost
  either way. That is harmless here, because the reset reinitialises every
  guest object, but it must not be counted as a successful wait.

## Findings for the go/no-go

Counts:

- 23 host-thread rows (about 28 creator sites).
- 2 of them run guest code on a host thread: `kernel-timer` and `ohci`.
- 2 guest-thread roles: `guest-main` and `guest-worker`. `xbox-thread` is
  dead.
- 13 blocking sites outside the wait bridges. Section 3 has 15 rows: the
  stub row is not a blocking site, and `NtYieldExecution` blocks for one
  time slice only.
- 12 wait-bridge rows, 8 of them marked **C**.

**Criterion 2: every way out is inside its own module.** Met, with three
risks to design for:

1. **Guest critical sections** (kernel_rtl.c:530). The way out is a plain
   `EnterCriticalSection` whose holder is a guest thread that will unwind
   without leaving it. The fix stays inside kernel_rtl: a try-loop with a
   reset check, or dropping the CS shadows before the release. It is still
   the single biggest deadlock risk.
2. **The order between guest stops and walker pause.** The D3D KickOff flush
   spin and MakeSpace's GPU-interrupt wait are guest code that only the
   nv2a walker frees. So the guests must stop at their back edges first.
   Only then do the walker and the APU pause. If the order is reversed, the
   guests can't stop.
3. **Guest call-ins on host threads and inline workers.** `kernel-timer`
   (DPCs, vblank ISR, timers) and `ohci` (USB ISR) each need their own unwind
   point at the call-in (task 1.4). The inline worker paths (`RECOMP_WORKERS=inline`,
   no stacks, `bridge_run_thread_inline`) need the unwind to pass the inner
   entry.

Also: the det INFINITE waits (guest_det.c:833/864) and the real-time
`KeDelayExecutionThread` sleep need slicing or a broadcast. Both changes stay
inside the kernel module. Disarm the `watchdog` during a reset.

**Criterion 3: every wait bridge has an unwind return before any guest-state
change.** Met. Every bridge has a single host wait followed by
`bridge_user_apc_finish`, and the two guest clears (2379, 6743) come after
the wait, so one check after the wait covers each.

These caveats remain:

- 8 of the 12 bridge rows consume the signal inside the host primitive. The
  check therefore sees an already-consumed signal. That is acceptable only
  because the reset reinitialises all guest objects.
- POSIX WaitAll consumes partially even without a reset (an existing bug,
  worth its own follow-up).
- The wake can't simply be a handle added to WaitAll waits or to 64-handle
  waits. Use the flag in the 1 ms POSIX poll and the virtual slices, and an
  APC on native Windows.

## The stop probe

`RECOMP_DEBUG=reset_probe=<flips>` (src/kernel/kernel_reset_probe.c)
measures how long each guest thread takes to reach a stop point at the
listed flips, without stopping anything. It found one more way out that
section 2 misses: outside the deterministic mode, a worker that ends in
`PsTerminateSystemThread` calls `ExitThread` without
`xbox_GuestCpuPart`, so the guest CPU scheduler never forgets its record
(`sched_forget`). The reset's thread count must not rely on Part alone;
the probe parts its own slot there. The guest unwind closes the gap: that
exit now calls `xbox_GuestCpuPart` on every path.

## The guest unwind

src/kernel/kernel_guest_unwind.c brings every guest thread back out of
guest code when a reset asks. It is inert until `xbox_GuestUnwindEnable`:
no thread is tracked, and each unwind point is one load of
`g_xbox_reset_pending` and a branch.

- **The jump.** `bridge_thread_main` runs the worker's guest code under
  `xbox_GuestUnwindCallIn`, which keeps a `jmp_buf` (`setjmp` on Windows,
  `_setjmp` on POSIX). A nested call-in (an inline worker, a callback) runs
  plainly, so the thread always goes back to its outermost one. Lifted
  frames own no host resources, so the `longjmp` leaks nothing. The call-in
  puts back the IRQL, the IRQL holder count and the one-CPU gate depth as
  they were when it was entered.
- **The points.**
  - `xbox_GuestCpuPreempt`, ahead of its early returns. The request keeps
    the preempt flag and the waiter count up, so every back edge calls in.
  - `xbox_SpinWait` and `xbox_SpinWaitKick`.
  - The guest CPU lock: its entry, its queue (`sched_take` unlinks the
    waiter), and after the grant.
  - The entry and exit of `kernel_thunk_dispatch`.
  - The blocking waits: `sync_wait_one`, `sync_wait_many` and the delay
    also wait on the thread's unwind event. On Windows the event is added to
    the wait. A wait-all or a 64-handle wait polls the flag in 20 ms slices
    instead. On POSIX the shim's `w32_wait_abortable` registers the event.
    The virtual-time slices check the flag on every pass.
  - A contended guest critical section, once it is taken.
  - The deterministic ring's turn and its block loop.
- **What the thread lets go of.**
  - Before the jump, the guest critical sections it owns, every recursion.
    The single-lock mode included. Their waiters wake.
  - A guest mutant it owns stays owned (abandoned). Its waiters wake on
    their own unwind events.
  - The guest CPU: the exit path parts it. A boot loop that parks drops it
    with `xbox_GuestCpuUnwindDrop` and takes it back after the resume.
  - Its registry slot (`xbox_GuestUnwindPart`) is freed last, after the
    thread object's end is written. A reset that waits for the thread can
    then clear guest memory safely.
- **Held regions.** An ISR or a DPC drain that runs on a guest thread
  borrows the worker stack. `xbox_GuestUnwindHold` counts the region, and a
  point inside it refuses: it logs `[RESET] unwind refused` and returns.
  The first point after the region unwinds.
- **Host threads.** The DPC, ISR, OHCI and APU call-ins wrap their guest
  call in `xbox_GuestUnwindHostBegin` and `xbox_GuestUnwindHostEnd`. A
  call-in that starts while a reset is pending skips the guest code. One
  that is already running is counted, and the reset waits for it to end.
  With the unwind off the begin is one load and a branch, and nothing is
  counted.
- **The join race.** `xbox_GuestUnwindSpawnBegin` counts a thread before
  its `CreateThread`, so the reset waits for it. When the thread joins with
  the flag up, its call-in runs no guest code.
- **The request.** `xbox_GuestUnwindRequest` raises the flag and sets every
  tracked thread's event. It returns 1 when it raised the flag. It returns
  0, raising nothing, when the unwind is off or an untracked thread lives.
  `xbox_GuestUnwindWaitStopped` returns once each tracked thread has parted
  or parked, no untracked thread lives and no host call-in is running. On a
  timeout it lists the tids still running and the counts. `xbox_GuestUnwindResume` drops the flag.
- **Limits.**
  - The title's main thread has no call-in until the boot loop owns it.
    Until then its points return.
  - A thread from `xbox_PsCreateSystemThreadEx` has no call-in. A request
    with such a thread live logs it and aborts. No current title has one
    (section 2).
  - The registry holds 64 threads. A 65th, or one whose unwind event
    cannot be made, is logged and counted as untracked. A request refuses
    while any untracked thread lives, because nothing could stop it.
- **Lock records.** A thread that ends holding a guest critical section
  clears its owner records at its part. The host reuses thread ids, and a
  later thread with the same id would otherwise let go of a lock it never
  took.
- **Stats.** `xbox_GuestUnwindStatsGet` gives the unwinds per point kind,
  the refusals, the critical-section recursions let go, the untracked
  threads and the host call-ins running.
- **Test.** The `guest_unwind` ctest covers every point kind (the kernel
  wait on one object, any of two and all of two), the lock off and on, the
  hold, a thread at DISPATCH under the one-CPU gate, the join race, the
  park, the deterministic ring, the registry limit and the host call-ins.
