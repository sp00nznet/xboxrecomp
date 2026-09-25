# APU Audio

A title that links the XDK's DirectSound drives the MCPX APU itself: it writes
voice descriptors, scatter-gather tables and command words, and waits for the
hardware to answer. `src/apu/` emulates the voice processor (VP) and stubs the
GP and EP, and v0.11.0 made it reachable (`RECOMP_AC97_READY`, the VEH hook,
`g_apu_state`) and fixed the 32-to-2 mixbin mixdown. On *X-Men Legends* that
still produced silence at the wrong speed. This page is what stood between a
reachable APU and a playable one, in the order it was found. Getting DirectSound
through its own initialisation — the DSP command doorbell and
`RECOMP_APU_DSP_ACK` — is covered in `apu_dsp.c`; on *X-Men Legends* the
doorbell sat at the same `+0x810` offset from DirectSound's block as on
Wreckless.

## Output: 4x Speed, Then Silence

The first measurement came from the XAudio2 statistics (`xa2_get_stats`):
about 142 dropped blocks a second and an output peak of −99 dB — digital
silence. Two defects in the XAudio2 branch of the monitor frame (`apu_core.c`):

- it cleared `monitor.frame_buf` before mixing, throwing away the 256 samples
  the DSP stage had just written, and
- it submitted 1,024 samples for each 256-sample frame — one frame every
  5.333 ms — so it produced audio at four times real time and the queue
  overflowed.

The frame now mixes the test tone and `mixer_render` on top of the DSP output,
submits exactly `MIXER_FRAME_SAMPLES`, then clears. The XAudio2 ring is 12
buffers of headroom rather than 3. Result: no drops, no underruns — and still
silence, because the voice processor had nothing to play (below).

Every output path now ends in the same hearing safety: a master volume
(`xa2_set_master_volume`, default 0.5) and a `16384 · tanh(x · vol / 16384)`
soft limiter, so output never exceeds −6 dBFS whatever the mix does. A title
under bring-up produces full-scale noise more often than music.

## The APU's Interrupt Was a Stub

`pci_irq_assert` in the APU model was empty, so vector 5 never reached the
ISR DirectSound connects. DirectSound learns about voices, front-end traps and
position notifications from that ISR, and without it:

- no voice ever started: the VP reported 0 active voices, mix peak 0.00;
- every voice **stop** cost 0.5 s. Stopping a voice sets its `0x8000` flag and
  loops until the APU acknowledges; the fallback force-releases after
  5,000,000 × 100 ns. Combat and NPC spawns stop voices constantly, so frames
  took 0.5, 1 or 2 s. Cutting that grace to zero removed the stalls and broke
  the startup movie, which depends on the voice really having stopped.

The APU now raises its line with `xbox_set_irq_line(5, level)`, front-end traps
raised inside `se_frame` are delivered too, and the kernel's timer thread calls
the connected ISR while the line is up. Delivering the interrupt exposed the
next problem at once: DirectSound's ISR crashed walking its voice list, a
back-link reading 0, because it ran in parallel with game threads that had
raised IRQL to protect that list. IRQL is enforced now; both are described in
[Kernel Replacement: Device interrupts and IRQL](kernel-replacement.md#device-interrupts-and-irql).
`irq 5 -> ISR claimed it` at boot means the ISR runs.

## Sound Data Read From the Wrong Memory

With voices running, they played silence (or noise, for streams):
`peak 0.000` in the voice trace. `ldl_le_phys` and friends read
`ram_base + (P & 0x03FFFFFF)`, treating an APU physical address as a guest
address. DirectSound fills its SGE and SSL tables with `MmGetPhysicalAddress`
of **contiguous** allocations, which this runtime maps at
`XBOX_CONTIG_BASE + P`. `apu_phys()` in `apu_shim.h` resolves P through the
contiguous arena when `xbox_ContiguousIsPhysical(P)`, the same rule the
pushbuffer executor and the OHCI model use — see
[Memory Layout: The Contiguous Arena](memory-layout.md#the-contiguous-arena).

## A Voice With Nothing to Play

`voice_resample` returned 0, not −1, when a voice had no samples, and
`voice_process` retried until the count went negative — forever, holding the
APU lock. The title froze with the APU thread in `voice_get_samples` and a
DirectSound thread in `voice_lock`. The loop stops on 0 as well, and the APU
thread yields its lock once per block so a guest register write cannot starve.

## Pitch

`voice_resample` was a placeholder that copied source samples straight to the
48 kHz output, whatever the voice's pitch. 22.05 kHz speech and effects played
2.18 times too fast, an octave up; 44.1 kHz music slightly fast. Voices are now
resampled linearly: each output sample steps `1/rate` through the source, state
resets on voice-on, and the source is fetched 32 samples at a time.

## Pacing

The APU thread paced itself by the wall clock, and `throttle()` reset its
schedule whenever it fell more than a frame behind — forgetting every stall.
Against a sound card with its own clock that is a slow drift, and the XAudio2
queue ran dry about once a second, heard as crackle (`und 1` in a stats line).
With XAudio2 active, the APU now renders whenever fewer than 8 blocks (43 ms)
are queued, so the sound card's clock drives it, and the thread runs at
`THREAD_PRIORITY_HIGHEST`.

The stub DSP's mixdown summed every bin (`RECOMP_APU_MIXDOWN_ALL`) and then
hard-clamped. Summing 32 bins goes past full scale — 1.33 in *X-Men Legends*'
intro movies — and a hard clamp there is audible distortion. It uses `tanh`
now, near-linear below about 0.5.

## Checking a Title

| Symptom | Look at |
|---|---|
| silence, `voices 0` | is the ISR claimed at boot? |
| voices active, `peak 0.000` | physical addresses: `RECOMP_APU_TRACE=1` should show `sounding` voices |
| 0.5 s frames when sounds stop | interrupt delivery |
| chipmunk voices | `RECOMP_APU_TRACE=1` prints each loud voice's rate: 2.177 = 22.05 kHz, 1.088 = 44.1 kHz |
| crackle | `xa2_get_stats`: underruns and drops should stay at 0 |
| freeze with `mcpx_apu_vp_frame` in one thread and `voice_lock` in another | the empty-voice spin |

`RECOMP_APU_TRACE=1` prints APU front-end methods and, once a second, a voice
summary: unpaused, sounding, and their rates.
