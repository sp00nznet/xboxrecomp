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
| Push buffer parsing (PFIFO DMA pusher) | Full | Walks JUMP/CALL/RETURN and ring wraps like the DMA engine; executed under `RECOMP_PB_EXEC` ([Pushbuffer Executor](pushbuffer-executor.md)) | PARTIAL | Medium (titles with statically linked XDK D3D) |
| PGRAPH → D3D11 method translator | N/A | From burnout3 | ADDED | High |
| Push buffer replay | N/A | From burnout3 | ADDED | Medium |
| **Register combiners (pixel shaders)** | Full (8 stages, RGB/alpha, final combiner) | Full (8 stages, HLSL generation, 128-entry cache) | DONE | - |
| **Vertex shader microcode translation** | Full (MAC+ILU ops, 192 constants, 12 temps) | Full (d3d8_vsh.c: parser, HLSL gen, 64-entry cache) | DONE | - |
| **Texture unswizzling (Z-order/Morton)** | Full | Full | ADDED | High |
| Texture format coverage (66 formats) | Full | All Xbox D3DFMT_* constants (33 swizzled + 20 linear) mapped to DXGI | PARTIAL | High |
| Palettized textures (P8 with palette lookup) | Full | P8 expanded to BGRA at upload using the texture's stage palette; `dev_SetPalette` re-bakes any P8 texture bound to that stage (animated/colorized palettes work); P8 surface LockRect returns raw 8-bit indices (write-back re-bakes) | DONE | Medium |
| Signed texture formats (R6G5B5, V8U8, V16U16) | Full | Distinct compact values; V8U8/V16U16 → R8G8_SNORM/R16G16_SNORM; Q8W8V8U8/X8L8V8U8 → R8G8B8A8_SNORM; L6V5U5 sign-extended to R8G8_SNORM at upload | DONE | Low |
| Extended texture formats (float, 16/32-bit pairs, 10-bit, DXN/DXT3A/DXT5A/CTX1, D24FS8/D24X8/D32) | Full | Added distinct compact values + LIN variants; wired through `d3d8_to_dxgi_format`/`d3d8_format_bpp`/swizzle. BGRA-in-memory→RGBA cases (A16B16G16R16, A32B32G32R32) and 10-bit A/R swaps are channel-converted in software at upload (covered by `tests/d3d8_smoke`). Approximate DXGI maps (DXN/DXT3A/DXT5A/CTX1, R11G11B10) still need in-game validation | DONE | High |
| YUV texture formats | Full | YUY2/UYVY converted to BGRA in software at upload | PARTIAL | Low |
| Mipmapping | Full chain | Full chain (auto-generate on Levels=0, per-level upload) | DONE | Medium |
| Cube textures | Full | Implemented (D3D11 Texture2DArray of 6 + TEXTURECUBE SRV; full mip chains, per-face LockRect/UnlockRect upload with unswizzle + format conversion, GetCubeMapSurface) | PARTIAL | Low |
| Volume textures | Full | Implemented (D3D11 Texture3D, full mip chains, 3D Z-order unswizzle, LockBox/UnlockBox + GetVolumeLevel wrappers) | PARTIAL | Low |
| Anti-aliasing modes | Full | MSAA sample counts honored (2/4/9-sample multi/super aliases, quincunx→2); validated via CheckMultisampleQualityLevels with graceful fallback; supersample approximated as MSAA; ResolveSubresource used for MSAA surface LockRect readback | PARTIAL | Low |
| Render target format negotiation | Full | CreateRenderTarget/CreateDepthStencilSurface/SetRenderTarget/Get*/GetBackBuffer + surfaces (render-to-texture works via GetSurfaceLevel; LockRect readback via staging texture copy: sub-rect, non-readonly write-back, MSAA resolve) | PARTIAL | Medium |

## D3D8 Translation Layer

| Feature | Status | Notes |
|---------|--------|-------|
| Device creation/reset/present | DONE | D3D11 backend |
| Fixed-function vertex transform (FVF) | DONE | HLSL vertex shader; per-set D3DFVF_TEXCOORDSIZE float1/3/4 texcoords supported (float4 VS input, variable input-layout formats) |
| Texture stage states (4 stages) | DONE | Full D3DTOP set: MODULATE/2X/4X, ADD/SIGNED/2X, SUBTRACT, BLEND*, DOTPRODUCT3 |
| Render states (blend, depth, stencil, cull) | DONE | ~20 states translated |
| Vertex/index buffer management | DONE | System memory staging |
| DrawPrimitive/DrawIndexedPrimitive | DONE | All primitive types |
| Quad list support | DONE | Converted to tri list |
| Viewport/scissor | DONE | |
| **Xbox pixel shader (register combiners)** | DONE | d3d8_combiners.c: 8 stages + final, HLSL cache |
| **Xbox vertex shader (NV2A microcode)** | DONE | d3d8_vsh.c: 14 MAC + 8 ILU ops, 192 constants, 64-entry cache |
| **Multi-texture (4 stages, full TSS ops)** | DONE | All D3DTOP ops, D3DTA args, 4 samplers bound |
| **Hardware T&L lighting (8 lights)** | DONE | Directional, point, spot; material; global ambient |
| **Vertex fog (linear/exp/exp2)** | DONE | VS fog factor, PS fog color blending |
| **Triangle fan conversion** | DONE | Fan/quad → tri list in DrawPrimitiveUP |
| **DrawPrimitiveUP ring buffer** | DONE | 4MB ring buffer, no per-call buffer create/destroy |
| **Offscreen render targets / surfaces** | PARTIAL | CreateRenderTarget/CreateDepthStencilSurface/SetRenderTarget/Get*/GetBackBuffer; render-to-texture via tex_GetSurfaceLevel sharing the texture's D3D11 resource; surface LockRect readback via D3D11_USAGE_STAGING copy/resolve (sub-rect supported, write-back unless D3DLOCK_READONLY); CreateImageSurface implemented |
| **Cube textures (CreateCubeTexture)** | PARTIAL | D3D11 Texture2D with ArraySize=6 + MISC_TEXTURECUBE, TEXTURECUBE SRV; per-face/level LockRect→unswizzle→convert→UpdateSubresource (subresource = face*levels+level); GetCubeMapSurface shares the array's RTV via TEXTURE2DARRAY; FF + combiner pixel shaders declare TextureCube matching the bound SRV and sample the full float3 TCI/reflection vector |
| **Volume textures (CreateVolumeTexture)** | PARTIAL | D3D11 Texture3D, full mip chains; LockBox/UnlockBox with 3D Z-order unswizzle + per-slice format conversion; GetVolumeLevel returns a ref-counted wrapper delegating to the parent texture; FF + combiner pixel shaders declare Texture3D matching the bound SRV and sample float3 coords |
| **MSAA / supersample modes** | PARTIAL | MultiSampleType honored via d3d8_msaa_sample_count (packed nibble decode); CheckMultisampleQualityLevels-guarded fallback (9→8→4→2→1); supersample approximated as MSAA; MSAA LockRect readback via ResolveSubresource |
| **Palettized textures (SetPalette, P8)** | DONE | Per-stage 256-entry palettes stored; P8 expanded to BGRA at upload; bound textures re-baked on SetPalette; P8 surface LockRect returns raw indices |
| Bump mapping / normal mapping | PARTIAL | Register combiners support bump; TCI normals via TEXCOORDINDEX | 
| Environment mapping | DONE | TCI (camera-space normal/position/reflection, sphere map) + per-stage texture matrix in VS |
| Per-pixel fog (table fog) | DONE | Table fog computed per-pixel from view-space depth (or range with D3DRS_RANGEFOGENABLE); correct D3DFOGMODE |

## Kernel

| Feature | Status | Notes |
|---------|--------|-------|
| Memory management (147 ordinals) | DONE | Win32 heap backend |
| File I/O with path translation | DONE | Xbox paths → host filesystem |
| Threading | DONE | Host threads, started when the creator yields; priorities; suspend at safe points ([details](kernel-replacement.md#threads-priorities-and-irql)) |
| Synchronization (events, semaphores, waits) | DONE | Win32 primitives |
| Timers and DPCs | DONE | |
| Crypto (SHA, RC4, RSA, DES) | DONE | Full implementation |
| Object manager | DONE | Basic reference counting |
| I/O manager | DONE | IRP stubs |
| HAL (IRQL, perf counters, PCI) | DONE | IRQL ≥ DISPATCH takes a process-wide dispatch lock; device interrupts delivered on the timer thread |
| **EEPROM data** | ADDED | Region, language, video standard populated |
| **AV pack / video mode detection** | ADDED | Returns HDTV/component, 480p capable |
| **SMBus / SMC** | ADDED | Version, tray state, AV pack, temperature |
| Xbox Live / network | Stub | PhyGetLinkState returns "no link" |
| USB/OHCI gamepad | PARTIAL | OHCI model enumerates the pad; XAPI's done-queue handler still fails, so replace XAPI input for now ([details](kernel-replacement.md#titles-that-drive-usb-themselves)) |
| DVD/disc drive | N/A | Host filesystem instead |

## Audio

| Feature | Status | Notes |
|---------|--------|-------|
| DirectSound buffer creation | DONE | Stub buffers accept all calls |
| Buffer Play/Stop/Volume/Frequency | DONE | Routed to APU mixer |
| APU Voice Processor (VP) | DONE | 64 voices, PCM/ADPCM, resampled to each voice's pitch; interrupt delivered ([APU Audio](apu-audio.md)) |
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

2. ~~**Vertex shader microcode translation**~~ - DONE. d3d8_vsh.c/h: 1,868 lines. 128-bit instruction parser, HLSL generator for 14 MAC + 8 ILU ops, 64-entry compiled shader cache, 192-constant buffer, input layout management.

3. ~~**Multi-texture pixel shader**~~ - DONE. Full 4-stage TSS with all D3DTOP operations, D3DTA argument resolution (DIFFUSE, CURRENT, TEXTURE, TFACTOR, SPECULAR + COMPLEMENT/ALPHAREPLICATE modifiers), 4 samplers bound per draw.

4. ~~**Hardware T&L lighting**~~ - DONE. Up to 8 lights (directional, point, spot) with material properties, global ambient, specular highlights. World-space normal transform via inverse-transpose matrix.

5. ~~**Vertex fog**~~ - DONE. Linear/exp/exp2 fog modes computed in vertex shader, blended with fog color in pixel shader.

6. ~~**Triangle fan + quad list conversion**~~ - DONE. Fan→tri list and quad→tri list conversion in DrawPrimitiveUP.

7. ~~**DrawPrimitiveUP performance**~~ - DONE. 4MB ring buffer with WRITE_NO_OVERWRITE/WRITE_DISCARD, no per-call buffer create/destroy.

8. **WMA audio decoder** - Blood Wake has a WMADEC section. Need either a software WMA decoder or integration with Windows Media Foundation.

9. ~~**Mipmap support**~~ - DONE. Full chain uploaded: per-level sys_mem, LockRect/UnlockRect per level, auto chain on Levels=0 in CreateTexture, per-level unswizzle.
 
10. ~~**Texture coordinate generation**~~ - DONE. D3DTSS_TEXCOORDINDEX high bits (TCI_PASSTHRU/0x10000..0x40000 incl. sphere map) + per-stage D3DTS_TEXTURE matrices in vertex shader. Default TEXCOORDINDEX = stage index.
 
11. ~~**Per-pixel (table) fog**~~ - DONE. Table fog computed per-pixel from view-space depth (range fog with D3DRS_RANGEFOGENABLE); correct D3DFOGMODE mapping (1=EXP, 2=EXP2, 3=LINEAR) in both VS and PS.
