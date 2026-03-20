# xboxrecomp Gap Analysis vs xemu

Comprehensive comparison of what xemu implements vs what xboxrecomp provides.
Prioritized by impact on Blood Wake and Wreckless (both launch-era titles).

## Status Legend
- DONE = Fully implemented in xboxrecomp
- ADDED = Added in this analysis pass
- PARTIAL = Scaffolded but incomplete
- MISSING = Not implemented, needed
- N/A = Not needed for static recomp

## GPU / NV2A

| Feature | xemu | xboxrecomp | Status | Priority |
|---------|------|-----------|--------|----------|
| NV2A register read/write (PMC, PFB, PTIMER, etc.) | Full | Full | DONE | - |
| MMIO interception (VEH-based) | N/A (LLE) | Full | DONE | - |
| Push buffer parsing (PFIFO DMA pusher) | Full | Stub | N/A | Low (D3D8 API intercept instead) |
| PGRAPH → D3D11 method translator | N/A | From burnout3 | ADDED | High |
| Push buffer replay | N/A | From burnout3 | ADDED | Medium |
| **Register combiners (pixel shaders)** | Full (8 stages, RGB/alpha, final combiner) | Full (8 stages, HLSL generation, 128-entry cache) | DONE | - |
| **Vertex shader microcode translation** | Full (MAC+ILU ops, 192 constants, 12 temps) | **NONE** (FVF only) | MISSING | **CRITICAL** |
| **Texture unswizzling (Z-order/Morton)** | Full | Full | ADDED | High |
| Texture format coverage (66 formats) | Full | 17 formats + 3 new | PARTIAL | High |
| Palettized textures (P8 with palette lookup) | Full | Format mapped, no palette | PARTIAL | Medium |
| Signed texture formats (R6G5B5, etc.) | Full | Missing | MISSING | Low |
| YUV texture formats | Full | Missing | MISSING | Low |
| Mipmapping | Full chain | Level 0 only | PARTIAL | Medium |
| Cube textures | Full | Missing | MISSING | Low |
| Volume textures | Full | Missing | MISSING | Low |
| Anti-aliasing modes | Full | Missing | MISSING | Low |
| Render target format negotiation | Full | Basic | PARTIAL | Medium |

## D3D8 Translation Layer

| Feature | Status | Notes |
|---------|--------|-------|
| Device creation/reset/present | DONE | D3D11 backend |
| Fixed-function vertex transform (FVF) | DONE | HLSL vertex shader |
| Texture stage states (4 stages) | DONE | MODULATE, ADD, SELECTARG |
| Render states (blend, depth, stencil, cull) | DONE | ~20 states translated |
| Vertex/index buffer management | DONE | System memory staging |
| DrawPrimitive/DrawIndexedPrimitive | DONE | All primitive types |
| Quad list support | DONE | Converted to tri list |
| Viewport/scissor | DONE | |
| **Xbox pixel shader (register combiners)** | DONE | d3d8_combiners.c: 8 stages + final, HLSL cache |
| **Xbox vertex shader (NV2A microcode)** | MISSING | Need microcode→HLSL translation |
| Multi-texture (beyond stage 0) | PARTIAL | States tracked, shader limited |
| Bump mapping / normal mapping | MISSING | Needs combiners |
| Environment mapping | MISSING | Needs combiners + tex coord gen |
| Per-pixel fog | MISSING | |
| Hardware T&L lighting | MISSING | Light state stored, not applied |

## Kernel

| Feature | Status | Notes |
|---------|--------|-------|
| Memory management (147 ordinals) | DONE | Win32 heap backend |
| File I/O with path translation | DONE | Xbox paths → host filesystem |
| Threading | DONE | Single-thread cooperative model |
| Synchronization (events, semaphores, waits) | DONE | Win32 primitives |
| Timers and DPCs | DONE | |
| Crypto (SHA, RC4, RSA, DES) | DONE | Full implementation |
| Object manager | DONE | Basic reference counting |
| I/O manager | DONE | IRP stubs |
| HAL (IRQL, perf counters, PCI) | DONE | Simulated, not enforced |
| **EEPROM data** | ADDED | Region, language, video standard populated |
| **AV pack / video mode detection** | ADDED | Returns HDTV/component, 480p capable |
| **SMBus / SMC** | ADDED | Version, tray state, AV pack, temperature |
| Xbox Live / network | Stub | PhyGetLinkState returns "no link" |
| USB/OHCI gamepad | N/A | Bypassed via XInput |
| DVD/disc drive | N/A | Host filesystem instead |

## Audio

| Feature | Status | Notes |
|---------|--------|-------|
| DirectSound buffer creation | DONE | Stub buffers accept all calls |
| Buffer Play/Stop/Volume/Frequency | DONE | Routed to APU mixer |
| APU Voice Processor (VP) | DONE | 64 voices, PCM/ADPCM |
| APU GP DSP effects (reverb, chorus) | Stub | Bypassed |
| APU EP final encode | Stub | Direct passthrough |
| Audio mute flag for testing | ADDED | g_audio_muted global |
| 3D positional audio | Stub | SetPosition etc. are no-ops |
| DirectSound streams | Stub | Xbox-specific streaming |
| WMA decoding | Missing | Blood Wake uses WMADEC section |
| I3DL2 environmental reverb | Missing | |
| HRTF 3D audio | Partial | Basic VP support |

## Input

| Feature | Status | Notes |
|---------|--------|-------|
| Controller polling (4 ports) | DONE | XInput backend |
| Digital + analog buttons | DONE | |
| Rumble/vibration | DONE | |
| Headset | Missing | Not needed for gameplay |

## Next Steps (Priority Order)

1. ~~**Register combiner translation**~~ - DONE. d3d8_combiners.c/h: 1,415 lines. Full 8-stage combiner + final combiner, runtime HLSL generation with 128-entry compiled shader cache.

2. **Vertex shader microcode translation** - Port xemu's `vsh-prog.c` vertex shader microcode translator. Blood Wake's water effects and Wreckless's visual effects likely depend on this.

3. **WMA audio decoder** - Blood Wake has a WMADEC section. Need either a software WMA decoder or integration with Windows Media Foundation.

4. **Mipmap support** - Currently only level 0. Add mipmap chain upload in texture creation and UnlockRect.

5. **Multi-texture pixel shader** - Even without full combiners, supporting 2-4 texture stages with basic blending (MODULATE, ADD) in the pixel shader would help many games.
