# Environment Variables

> Every `RECOMP_*` switch the runtime and tools read, what it does, and what it defaults to.
>
> **Read this when:** a title needs a bring-up switch, a log is asking you to set one, or you want to know what a variable you found in a script does.
>
> Part of the [Documentation Index](../INDEX.md)

Almost everything here is **off by default**. A switch that changes behaviour is a per-title opt-in: set it for the title that needs it and leave it unset for the rest, so one title's fix never moves another's output. Debug switches only print or dump; they never change what the title does.

How to read the tables:

- **Default** is what happens with the variable unset. "off" means the variable has to be present (or non-zero, where the entry says so). "on" means it is active until you set `0`.
- **Where** is the source file that reads it, so the code is one search away.
- Values are text. A flag is "set" when the variable exists, unless the entry says `1` or non-zero is required.

## Bring-up and diagnostics

Start here when a title does not run. See [Step 6: Iterative Debugging](../pipeline/06-debugging.md) for the workflow these belong to.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_WATCHDOG_SECS=N` | off | Dumps registers, PUT/GET and the guest stack when the title stops making progress for N seconds. | `xbox_memory_layout.c` |
| `RECOMP_WATCH=<va>` | off | Names the guest code that writes a value at an address (a callback slot, a "ready" flag). Wrap the address in `[` `]` to watch the word it points to. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_LEN` | off | Length in bytes of the watched range. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_DEPTH` | 14 | Frames printed per watch hit; identical consecutive return addresses are folded. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_RAW` | off | Also prints the raw stack frame, to see which object the writer was working on. | `xbox_memory_layout.c` |
| `RECOMP_KERNEL_WATCH=<va>` | off | Reports a guest address whenever a kernel bridge call changes it. | `kernel_bridge.c` |
| `RECOMP_KERNEL_WATCH_ALL` | off | With the above: prints every sample, so a value that was already wrong at the first call shows. | `kernel_bridge.c` |
| `RECOMP_KERNEL_LOG_BUDGET=N` | 200 | Number of kernel-call log lines kept before the log goes quiet. | `kernel_bridge.c` |
| `RECOMP_IRQL_TRACE` | off | Prints the first IRQL transitions; a lower from a level the raise did not set is a calling-convention bug. | `kernel_hal.c` |
| `RECOMP_UNIMPL_TRAP=1` | off | Stops at the first untranslated instruction that is actually reached, instead of logging it. | `templates/new-game/src/recomp_manual.c` |
| `RECOMP_STUB_LOG=1` | off | Prints each unresolved stub the first time it runs, with return address and `esp`. Read by the generated stubs file. | `tools/recomp/translator.py` |
| `RECOMP_TRACE_BUDGET=N` | 400000 | Lines of the call trace kept; a larger file is better than a hole. | `recomp_trace.c` |
| `RECOMP_TRACE_PROFILE=N` | 20000000 | Prints the call profile every N calls (and at exit). | `recomp_trace.c` |
| `RECOMP_TRACE_ARGS=N` | off | Adds N stack arguments to each trace line. | `recomp_trace.c` |
| `RECOMP_TRACE_DEREF` | off | Follows pointer arguments one level in the trace. | `recomp_trace.c` |
| `RECOMP_CS_TRACE_CRT[=all]` | off | Traces critical-section activity (CRT sections only, or all). | `kernel_rtl.c` |
| `RECOMP_CS_WATCH=<va>` | off | Traces one critical section by address. | `kernel_rtl.c` |
| `RECOMP_POKE=va[:val][,...]` | off | Holds guest globals at a value. A bring-up probe, not a fix. | `xbox_memory_layout.c` |
| `RECOMP_PEEK=<va>[,...]` | off | Prints guest words at each report, both as a number and as a pointer. | `nv2a_pb_exec.c` |
| `RECOMP_PEEK_CHAIN` | off | Follows a pointer chain level by level and says where a null breaks it. | `nv2a_pb_exec.c` |
| `RECOMP_TRAP_NULL` | off | Protects the null page so a null dereference faults. | `xbox_memory_layout.c` |
| `RECOMP_FORCE_RETURN` | off | Turns on the early return of functions generated with `--force-return`; without it they behave normally. | `xbox_memory_layout.c` |
| `RECOMP_MISSING_FILES` | list | `0` turns the missing-file report off, `all` lists every miss, otherwise the default report. | `kernel_missing.c` |
| `RECOMP_MISSING_FILES_LIST=<path>` | off | Writes the missing-file list to that path on exit. | `kernel_missing.c` |
| `RECOMP_CMDLINE=<text>` | off | Command line handed to the title, as a launcher would (`+map intro`). | `kernel_bridge.c` |

## Memory and kernel

Opt-ins that change how the kernel layer behaves. Each fixes a class of titles; none is a default. See [Xbox Kernel Replacement](../technical/kernel-replacement.md) and [Memory Layout Reproduction](../technical/memory-layout.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_SAVE_DIR=<dir>` | off | Root for the title's `UDATA`, `TDATA` and cache folders; created if missing. Absolute. Saves left in the game folder are then not used. | `kernel_path.c` |
| `RECOMP_HEAP_RECLAIM` | off | Gives freed guest heap back. | `xbox_memory_layout.c` |
| `RECOMP_EXT_VMA` | off | Extended virtual-memory areas above the mirrors. | `guest_vmem.c`, `xbox_memory_layout.c` |
| `RECOMP_CONTIG_AVOID_IMAGE=1` | off | Keeps contiguous allocations out of the image range. | `xbox_memory_layout.c` |
| `RECOMP_WIN_PLACEHOLDER` | off | Reserves the guest window with placeholders on Windows hosts where fixed bases are taken. | `xbox_memory_layout.c` |
| `RECOMP_TITLE_KEVENTS=1` | off | Host events for events a title builds inline instead of through `KeInitializeEvent`. | `kernel_bridge.c` |
| `RECOMP_GUEST_LOCK=1` | off | Only one guest thread runs guest code at a time. | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_QUANTUM=ms` | 4 | With the lock: milliseconds before the lock is handed over (0 = at every call). | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_CPU_TRACE[=ms]` | off | With the lock: periodic trace of who holds it. | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_CPUS` | all | `all`, or a core number: pin the title's threads to one core. | `kernel_thread.c` |
| `RECOMP_CS_MODE=single` | per-section | Backs every critical section with one host lock. | `kernel_rtl.c` |
| `RECOMP_WORKERS` | off | Runs requests of the title's worker threads inline. A bisecting tool for concurrency bugs. | `kernel_bridge.c` |
| `RECOMP_ASYNC_IO` | off | Reports a read on an asynchronous handle as still in flight. The data is read synchronously either way. | `kernel_bridge.c` |
| `RECOMP_DISPATCH_GATE=1` | off | The one-CPU gate: guest code runs only at safe points while interrupts are delivered. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_POINTS` | on with the gate | `0` disables safe-point delivery under the gate. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_FORCE` | off | Forces delivery at a safe point. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_MS=N` | 250 | Milliseconds the gate waits for a safe point. | `kernel_hal.c` |
| `RECOMP_DPC_ON_RAISE` | on with the gate | `0` stops DPCs running when IRQL is raised. | `kernel_hal.c` |
| `RECOMP_CPUID_NO_SIMD` | off | Hides SIMD from `cpuid`. | `kernel_hal.c` |
| `RECOMP_AC97_READY=1` | off | Reports the AC'97 codec ready and routes the APU aperture to the emulated APU. | `xbox_memory_layout.c`, `templates/new-game/src/main.c` |
| `RECOMP_DSP_ACK=va[,...]` | off | Guest addresses to acknowledge as the DSP's doorbell. | `xbox_memory_layout.c` |

## Graphics: pushbuffer executor

The executor is itself opt-in: set `RECOMP_PB_EXEC=1` to execute the pushbuffer. See [Pushbuffer Executor](../technical/pushbuffer-executor.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_PB_EXEC` | off | Executes the title's pushbuffer. | `nv2a_pb_scan.c`, `xbox_memory_layout.c` |
| `RECOMP_PB_SCAN` | off | Read-only survey of the pushbuffer; ranks what is not implemented. | `nv2a_pb_scan.c`, `xbox_memory_layout.c` |
| `RECOMP_PB_WALK=dma` | off | Walks the pushbuffer as the DMA engine does, which delivers D3D's software-method interrupt. | `nv2a_pb_scan.c` |
| `RECOMP_NV2A_TRACE` | off | Shows whether `DMA_PUT` moves at all. | `xbox_memory_layout.c` |
| `RECOMP_PB_REPORT_MS=N` | 10000 | Milliseconds between `[GPU]` reports (minimum 100). | `xbox_memory_layout.c` |
| `RECOMP_PB_EXEC_VERBOSE` | off | Per-batch trace. | `nv2a_pb_exec.c` |
| `RECOMP_PB_UNHANDLED_ALL` | top 10 | Lists every method the executor does not model, not only the top ten. | `nv2a_pb_exec.c` |
| `RECOMP_VBLANK[=paced]` | no vblanks | Raises vblanks: a ~50 Hz tick, or `paced` for the 60 Hz schedule. | `kernel_bridge.c` |
| `RECOMP_FLIP_PACING[=edge]` | off | Holds each flip to the vblank count. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_FLIP_NORMALS` | off | Lights the side of a fixed-function mesh that faces the viewer. Not hardware behaviour. | `nv2a_pb_exec.c` |
| `RECOMP_CLIP_NEAR_Z` | on | `0` turns off the near-plane clip. | `nv2a_pb_exec.c` |
| `RECOMP_SWIZZLED_RT` | on | `0` stops rendering into swizzled render targets. | `nv2a_pb_exec.c` |
| `RECOMP_NO_COMBINERS` | off | Skips the register combiners. | `nv2a_pb_exec.c` |
| `RECOMP_NO_VSH` | off | Skips vertex programs. | `nv2a_pb_exec.c` |
| `RECOMP_RASTER_THREADS=N` | auto | Worker threads for the software rasteriser. | `nv2a_pb_exec.c` |
| `RECOMP_RASTER_TEST` | off | Bring-up aid: draws a magenta triangle after every clear, to prove the rasteriser and presenter work. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_TRACE` | off | Matrix and first vertices of the first fixed-function batches. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_TRACE=N` | 0 | Prints N vertex-program batches. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_DUMP` | off | Vertex-program constant traffic. | `nv2a_vsh_interp.c` |
| `RECOMP_FRAME_TRACE` | off | Traces one frame in detail. | `nv2a_pb_exec.c` |
| `RECOMP_FRAME_TRACE_METHODS` | off | With the above: every method of the traced frame, less the bulk uploads. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_STATE` | off | Texture state per report. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_DUMP=<prefix>` | off | Writes each distinct texture as a BMP, first use only. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_DUMP_EVERY=N` | 0 | Dumps every Nth texture bind instead. | `nv2a_pb_exec.c` |
| `RECOMP_FB_DUMP=<prefix>` | off | Writes the draw surface as a BMP at each report. | `nv2a_pb_exec.c`, `fb_present.c` |
| `RECOMP_FB_DUMP_FLIPS` | off | `1` dumps every flip; any other value names a file whose existence turns dumping on. | `nv2a_pb_exec.c` |
| `RECOMP_FIND_NAN` | off | Scans for NaN matrices at every report. | `nv2a_pb_exec.c` |
| `RECOMP_FIND_QUAD` | off | Scans once for quad vertices. | `nv2a_pb_exec.c` |

## Video output

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_FB_WINDOW` | off | Opens a window showing the frame buffer. | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_FB_VA=<va>` | follows the draw surface | Pins the window to one guest address. | `fb_present.c` |
| `RECOMP_FB_WINDOW_DUMP_EVERY=N` | 0 | With `RECOMP_FB_DUMP`: dumps what the window shows every N frames. | `fb_present.c` |
| `RECOMP_WINDOW_SCALE=1..8` | 2 | Client-area scale. | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_FULLSCREEN` | off | Starts fullscreen. | `fb_present.c` |
| `RECOMP_HEADLESS` | off | No window and a dummy audio driver. | `fb_present_sdl.c`, `apu_sdl2.c` |
| `RECOMP_CLOSE_KEEPS_RUNNING=1` | off | Closing the window only closes it instead of exiting the process. | `fb_present.c` |
| `RECOMP_D3D_PRESENT` | off | Presents through a D3D11 swap chain. | `gpu_present.c` |
| `RECOMP_PRESENT_VSYNC` | on | `0` turns vsync off. | `gpu_present.c`, `fb_present_sdl.c` |
| `RECOMP_PRESENT_FILTER` | default | Sampler filter for the D3D11 present. | `gpu_present.c` |
| `RECOMP_FMV_HOST` | off | Plays a movie through the host player when the title asks to. A substitute for the title's own output. | `kernel_bridge.c` |
| `RECOMP_FMV_DUMP=<prefix>` | off | Writes two frames of a movie as BMP evidence. | `video_pump.c` |

## Audio

See the [APU](../../src/apu/README.md) and [DirectSound](../../src/audio/README.md) module references.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_APU_TRACE` | off | Voice statistics and host playback accounting. | `apu_core.c`, `apu_vp.c`, `apu_mmio_hook.c` |
| `RECOMP_AUDIO_HOST` | off | Host playback accounting without the rest of the trace. | `apu_core.c` |
| `RECOMP_APU_RING` | off | Ring monitor: finds a streaming writer that falls behind the play cursor. | `apu_vp.c` |
| `RECOMP_APU_SOLO=<voice>` | off | Mutes every other voice, to find where a sound comes from. | `apu_core.c` |
| `RECOMP_APU_VOICE_DUMP=<voice\|all>` | off | Dumps voices as raw PCM to `RECOMP_APU_VOICE_DUMP_FILE`. | `apu_vp.c` |
| `RECOMP_AUDIO_WAV=<path>` | off | Records exactly what the device is given. | `apu_core.c` |
| `RECOMP_AUDIO_WAV_SECS=N` | unlimited | Caps the recording. | `apu_core.c` |
| `RECOMP_APU_HRTF` | on | `0` turns HRTF for 3D voices off. | `apu_vp.c` |
| `RECOMP_APU_MIXDOWN_ALL` | on | `0` limits the mixdown. | `apu_dsp.c` |
| `RECOMP_APU_DSP_ACK` | auto | DSP doorbell acknowledgement: `auto` derives it, `0`/`off` disables, or a list of addresses. | `apu_dsp.c` |
| `RECOMP_AUDIO_BUF_COUNT`, `RECOMP_AUDIO_BUF_SAMPLES` | built-in | Device buffer count and size. | `apu_sdl2.c`, `apu_xaudio2.c` |

## Input

See [xbox_input](../../src/input/README.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_KEYBOARD` | off | Keyboard acts as a controller. | `keyboard.c`, `usb_gamepad.c` |
| `RECOMP_MKB` | off | Keyboard-and-mouse layer; tuned by `RECOMP_MKB_SENS`, `_DZ`, `_ACC`, `_FULL`, `_INVERT_Y`, `_SWAP_STICKS` or an `RECOMP_MKB_INI` file. | `mkb_input.c` |
| `RECOMP_KEY_TRACE` | off | Prints the first 40 key-downs. | `keyboard_sdl.c`, `fb_present.c` |
| `RECOMP_INPUT_DIAG` | off | Reports whether the input chain is wired. | `usb_gamepad.c` |
| `RECOMP_RUMBLE` | on | `0` turns rumble off. | `ohci.c` |
| `RECOMP_USB` | off | Enables the USB controller model. | `ohci.c` |
| `RECOMP_USB_PADS=1..4` | 1 | Number of pads plugged. | `ohci.c` |
| `RECOMP_USB_PORT=0..3` | 2 with four ports | Root port the pad appears on. | `ohci.c` |
| `RECOMP_USB_HC`, `RECOMP_USB_NDP` | model default | Host-controller selection and port count. | `ohci.c` |
| `RECOMP_USB_TRACE`, `RECOMP_USB_STATS` | off | Controller trace; completions every 5 s. | `ohci.c` |
| `RECOMP_PAD_LIVE=<file>`, `RECOMP_PAD_SCRIPT`, `RECOMP_PAD_PRESS` | off | Test input: a file read live, a script, or a periodic press. | `usb_gamepad.c` |
