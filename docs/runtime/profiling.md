# Profiling a frame (where a slow scene's time goes)

A slow scene costs time in four places, and the fix depends on which one:

- **game code**: the recompiled guest functions doing the game's own work;
- **toolkit**: the kernel and HLE layer and the pushbuffer executor (the
  walker decoding the NV2A command stream);
- **renderer**: the D3D11 or Metal backend turning the decoded state into
  host draws, and the host driver under it (DXVK under Proton);
- **GPU**: the host GPU running those draws.

This page is the recipe for splitting a frame into those four parts, on the
native Windows PC and on the Steam Deck (D3D11 under Proton). Every tool
here is opt-in and changes nothing the game draws.

## The instruments

| What | Key | Where | What it says |
|---|---|---|---|
| Per-flip walker split | `RECOMP_TRACE=flip` | every host | `[PB-PERF] flip N wall W ms walk K ms (draw D, decode C) ... present P ... kickwait Q`: the walker's time per flip, split into backend submission (draw) and command decoding; kickwait is the walker waiting for the guest |
| Flip times | `RECOMP_TRACE=pacing=all` | every host | `[PACING] flip N t_us T batches B`: flips/s, and the batch count that tells a heavy flip from a light one |
| GPU frame span and Present | `RECOMP_TRACE=d3d11_prof=all` | D3D11 | `[D3D11-PROF] flip N gpu G ms present P ms`: timestamp queries from the flip's first command to its last, and the Present call's own time. The span includes the GPU's idle gaps between submissions, so it is an upper bound (it reads close to the frame time when the walker submits all through the frame); pair it with the GPU's busy % below. Metal's `metal_prof` gpu figure is a different measure (the sum of each command buffer's GPU start-to-end, gaps left out), so the two backends' gpu ms do not compare |
| In-process sampler | `RECOMP_TRACE=prof=HZ,prof_delay=S,prof_out=PATH` | Windows, Wine, macOS | every thread's pc HZ times a second from S s on, written at exit (`host_prof.h`) |
| Deck-shaped CPU | `RECOMP_DEBUG=host_cpus=0xFF` | native Windows | the whole process on 8 logical CPUs (4 cores with SMT), as the Deck has |
| Wine's ack nap | `RECOMP_DEBUG=kick_nap=delay` | native Windows | the walker naps as it does under Wine (`hybrid` spins on native Windows) |
| GPU busy | sysfs `gpu_busy_percent`, `nvidia-smi` | Deck, PC | how much of the time the GPU works, and at what clock |
| Bucketing | `tools/prof/prof_buckets.py` | the Mac | the sampler report in ms per flip per bucket and thread |

The sampler needs the game to exit by itself (`RECOMP_WINDOW_QUIT_AFTER`)
so its exit hook writes the report. Under Wine it samples every thread on
every round (mode `all`: waits included, sorted out by module); on native
Windows only threads that ran since the last round (`running-since`), and a
thread counts busy for the whole round if it ran at all in it, so a thread
that spins and sleeps in short turns reads over 100%. Compare buckets within
one host; across hosts compare the `[PB-PERF]` times.

## The recipe

1. **Pick the scene and its window.** A scripted input
   (`RECOMP_INPUT_SCRIPT=@...`) and an empty save dir make it repeat. Run it
   once with `flip,pacing=all` and find the heavy flips (for BLiNX 2's
   `@stage1-2-barricade`, the flips of 8000 batches or more, 37-72 s after
   the first flip). Set `prof_delay` to the window's start and
   `RECOMP_WINDOW_QUIT_AFTER` to its end.
2. **The PC, main profile.** Native Windows has the CPU headroom to sample
   at 250 Hz and to run WPR beside the game. Run the scene three ways:
   stock; Deck-shaped (`host_cpus=0xFF,kick_nap=delay`); Deck-shaped plus
   `prof=250`. The first two give the PC-to-Deck gap, the third the buckets
   and the sampler's overhead (its flips/s and walk ms against the second).
3. **The Deck, light.** The same scene, short runs: one with the traces
   only, one with `prof=250`, and a repeat of the first for run-to-run noise.
   A user-scope `systemd-run` loop reads `gpu_busy_percent` and
   `pp_dpm_sclk` every 250 ms; the sampler report, the GPU log and the game
   log come back over ssh. Nothing is installed, nothing is built there.
4. **Bucket.** On the Mac:

   ```
   python3 -m tools.prof.prof_buckets prof.txt --exe cat_recomp.exe \
       --pdbutil <llvm-mingw>/bin/llvm-pdbutil --log game-stdio.log \
       --names <game>/config/symbols.toml --spin <the game's busy-waits>
   ```

   A Windows release exe has no symbol table; the tool reads the publics of
   the PDB beside it (lld writes one even without `-g`). Statics have no
   public and count under the public before them, the same source file in
   practice: buckets are right, per-function names approximate. `--names`
   makes the game's named guest functions count as guest; `--spin` moves
   its busy-waits (BLiNX 2: `d3d_kickoff,sys_main_loop,d3d_fence_wait,
   d3d_vblank_dpc_ack`) to `guest-spin`.
5. **Read it.** Per thread, ms per flip in each bucket (`guest`,
   `guest-spin`, `kernel`, `executor`, `renderer`, `driver`, `unix`,
   `audio`). The frame is as slow as its longest chain: the main guest thread
   (guest work plus kernel), and the walker (executor plus renderer plus the
   driver calls on its thread), then DXVK's threads and the GPU. A guest
   thread mostly in `guest-spin` waits for the walker; a walker mostly in
   kickwait waits for the guest.

Buckets sum over threads, which run in parallel, so they add up to more than
a frame. `unix` (Wine only) is a pc outside every PE module: Mesa/RADV
working or a thread blocked in a Unix syscall; the sampler cannot tell the
two apart (`perf` on the Deck could, if ever needed).

## Example: BLiNX 2, mission 1-2 barricade, Deck

Exe ba2cc77d (toolkit 48ae7cd8), GE-Proton11-7, 729 heavy flips (p50 10000
batches):

- 29.6-30.0 flips/s; walk p50 20.1-20.3 ms (draw 9.5-9.7, decode 9.9),
  kickwait 15 ms; Present call 0.04 ms (DXVK queues it).
- Sampler at 250 Hz: walk 21.9 ms (+8%), flips/s unchanged.
- Main guest thread 31 ms/flip on CPU: 23.6 ms spinning in d3d_kickoff and
  the vblank wait, 7.1 ms game code, 0.3 ms kernel.
- Walker 15.1 ms/flip: executor 8.6, renderer 4.0, driver calls 2.3. DXVK's
  threads another 11.3 ms (queue 5.1, cs 4.6, submit 1.5) on other cores.
- GPU busy 17-37% (5 s means) at 200-1040 MHz: the GPU is not the limit.

## Example: the same scene on the PC

Same exe, native Windows 11, RTX-class GPU (`nvidia-smi` beside the run):

| Run | flips/s | walk p50 (draw, decode) | kickwait | GPU busy |
|---|---|---|---|---|
| stock | 30.0 | 17.4 (7.5, 8.3) | 10.3 | 30% at 670 MHz |
| stock + `prof=250` | 30.0 | 17.8 (7.8, 8.4) | 10.3 | 30% |
| Deck-shaped (`host_cpus=0xFF,kick_nap=delay`) | 26.8 | 19.3 (8.3, 9.4) | 16.6 | 30% |
| Deck-shaped + `prof=250` | 27.1 | 18.0 (7.8, 8.9) | 16.1 | 30% |
| Deck (for reference) | 29.6-30.0 | 20.1-20.3 (9.5-9.7, 9.9) | 15 | 17-37% |

- The sampler costs about 2% of walk time on the PC and no flips/s (8%
  on the Deck); run to run noise is about as large.
- The Deck-shaped PC walker comes within 5% of the Deck's walk time, so the
  PC reproduces the Deck's walker cost and can stand in for it when
  profiling the executor and renderer. Its flips/s is lower than the Deck's:
  `kick_nap=delay` naps on the Windows timer, which is coarser than Wine's,
  so the PC loses frames the Deck keeps. Use the Deck-shaped run for the
  walker's split, not for the frame rate.
- Buckets, Deck-shaped + sampler (ms/flip): main guest thread guest-spin
  30.8, guest 5.2, kernel 0.4; walker executor 9.6, renderer 4.5, driver
  3.3. Stock + sampler: guest thread guest-spin 28.3, guest 4.8, kernel
  0.1; walker executor 6.6, renderer 3.3, driver 3.2, kernel 2.3 (the
  ack nap, `xbox_HostNapNs`, which the Deck-shaped `kick_nap=delay` turns
  into sleep).
  Executor time is spread over the pushbuffer method handlers
  (`nv2a_pb_exec_method`, `nv2a_pb_scan`, `nv2a_rt_own_stale`,
  `nv2a_ff_method`).
- Neither GPU is the limit: the walker (executor plus renderer plus driver)
  is the chain the guest waits on.

For a system-wide view (driver, kernel and GPU queue), WPR from the Windows
Performance Toolkit records beside the game (`wpr -start CPU -start GPU
-filemode`, then `wpr -stop X.etl`); open the ETL in WPA on the PC, or
take a per-module text table with `xperf -i X.etl -a profile -detail`. On
this scene it cost about 6% of walk time and no flips/s, and wrote 2 GB in
90 s, so keep the ETL on the PC. It shows what the in-process sampler
cannot: kernel-mode time. The game process spent 65% of its CPU in
`cat_recomp.exe`, 17% in `ntoskrnl.exe` (the waits' and spins' syscalls),
6% in the NVIDIA user driver and 5% in `ntdll`, over the whole run.
