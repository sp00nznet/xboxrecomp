# Xbox D3D8LTCG Device Context Reference

## Overview

Xbox games using Criterion's RenderWare engine (and potentially other Xbox D3D8 games) use a statically-linked D3D8LTCG library that maintains a ~16KB device context structure. This document maps the critical fields needed to get recompiled D3D8LTCG gen code running.

**Applies to**: Xbox games using XDK D3D8LTCG (Burnout 3, possibly other Criterion/RenderWare titles)

## Device Context Location

- **Static address**: 0x0035D6A0 (in D3D section, NOT heap-allocated)
- **Global pointer**: MEM32(0x35FB48) = device address
- **Size**: ~16KB (0x4000 bytes)

## Critical Field Map

### Push Buffer Ring Management (offsets +0x00 to +0x4C)

| Offset | Size | Name | Description |
|--------|------|------|-------------|
| +0x00 | 4 | `pb_put` | PB write cursor (absolute Xbox VA) |
| +0x04 | 4 | `pb_limit` | PB end boundary (absolute Xbox VA) |
| +0x08 | 4 | `flags` | Device flags — **bit 14 (0x4000) = camera active** |
| +0x0C | 4 | `pb_size_field` | PB size or control field |
| +0x24 | 4 | `pb_ring_start` | PB ring buffer start address |
| +0x28 | 4 | `pb_ring_end` | PB ring buffer end address |
| +0x2C | 4 | `pb_write_seq` | PB write sequence counter |
| +0x30 | 4 | `pb_gpu_read_ptr` | **POINTER to GPU read position** (critical!) |
| +0x34 | 4 | `fence_write_idx` | Fence write index |
| +0x38 | 4 | `fence_mask` | Fence ring mask (entries - 1) |
| +0x3C | 4 | `fence_state` | Fence state |
| +0x40 | 4 | `pb_control` | PB control field |
| +0x44 | 4 | `pb_ring_size` | PB ring buffer total size |
| +0x48 | 4 | `fence_array_ptr` | Pointer to fence entry array |

### Render State (selected offsets)

| Offset | Size | Name | Description |
|--------|------|------|-------------|
| +0x784 | 4 | `render_target` | D3D render target surface pointer |
| +0x794 | 4 | `depth_surface` | D3D depth/stencil surface pointer |
| +0x7A8 | 4 | `back_buffer` | D3D back buffer surface pointer |
| +0x7CC | 4 | `rs_flags_0` | Render state flags (clear to 0) |
| +0x954 | 4 | `viewport_w` | Viewport width |
| +0x958 | 4 | `viewport_h` | Viewport height |
| +0xC60 | 64 | `transform_cache` | Current transform matrix |
| +0xCA0 | 64 | `render_state_matrix` | Render state matrix |
| +0xEF8 | 4 | `timer_accum_0` | Frame timer accumulator 0 |
| +0xEFC | 4 | `timer_accum_1` | Frame timer accumulator 1 |

### Double-Buffered Render Targets (+0x1974)

| Offset | Size | Name | Description |
|--------|------|------|-------------|
| +0x1974 | 4 | `rt_surface_0` | Render target surface (frame 0) |
| +0x1978 | 4 | `rt_surface_1` | Render target surface (frame 1) |

The camera render orchestrator (sub_00351090) alternates:
```c
rt_idx = (frame_counter - 1) & 1;
surface = MEM32(device + rt_idx * 4 + 0x1974);
if (surface == NULL) skip_scene_render();
```

### Callback & State Fields

| Offset | Size | Name | Description |
|--------|------|------|-------------|
| +0x19FC | 4 | `pre_render_cb` | Pre-render callback function pointer (NULL = none) |
| +0x1A00 | 4 | `cb_edi_storage` | EDI value saved before callback |
| +0x1A04 | 4 | `rt_surface_swap` | Render target surface (swap state) |
| +0x1A08 | 4 | `ds_surface_swap` | Depth/stencil surface (swap state) |
| +0x1AD4 | 4 | `rs_flags_1` | Render state flags (clear to 0) |
| +0x1DD0 | 4 | `clear_flag` | Clear state flag |
| +0x2478 | 4 | `frame_counter` | Frame counter (incremented per render) |

## The GPU Read Pointer Problem

The D3D8LTCG code contains a **spin-wait loop** at the push buffer management level:

```c
// At 0x3518A3 in the D3D8LTCG mega-function:
do {
    gpu_read = MEM32(MEM32(device + 0x30));  // deref GPU read ptr
    available = pb_write_seq - gpu_read;
} while (requested > available);  // spin until GPU catches up
```

On real Xbox hardware, the GPU consumes push buffer data and advances the read pointer. In a recompilation without a real GPU, this loop spins forever.

### Solution: Self-Referencing Read Pointer

Point `device+0x30` to `device+0x2C` (the write sequence counter itself):

```c
MEM32(device + 0x30) = device + 0x2C;
```

This makes GPU_read == write_seq → available == 0 → space check always passes → no spin loop. The D3D8LTCG code exits immediately at its first buffer space check.

## D3D8LTCG Stub Functions

The D3D8LTCG library is a single giant function (0x34C2E0-0x360A54, ~83KB) with many mid-entry points. In recompilation, each entry point becomes a separate function. Many are stubs that must clean the Xbox stack correctly.

**Critical**: Empty stubs `void sub_XXXX(void) { }` corrupt the Xbox stack because the caller pushes a return address (and possibly params) that the callee must pop.

### Calling Convention Reference

| Function | Params | Stack Cleanup | Notes |
|----------|--------|---------------|-------|
| sub_003505A0 | 1 | `esp += 8` | |
| sub_003507D0 | 0 | `esp += 4` | |
| sub_003508B0 | 0 | `esp += 4` | |
| sub_00350950 | 0 | `esp += 4` | |
| sub_00350BD0 | 0 | `esp += 4` | |
| sub_00350C10 | 1 | `esp += 8` | End camera |
| sub_00350DB0 | 0 | `esp += 4` | |
| sub_00350EC0 | 0 | `esp += 4` | |
| sub_00351050 | 0 | `esp += 4` | |
| sub_00351180 | 2 | `esp += 12` | |
| sub_003513F0 | 0 | `esp += 4` | |
| sub_00351490 | 0 | gen code | Begin camera (62K, writes NV2A state) |
| sub_00351550 | 1 | `esp += 8` | PB fence check |
| sub_00351580 | 0 | `esp += 4` | PB allocate |
| sub_00351680 | 0 | `esp += 4` | |
| sub_00351700 | 0 | `esp += 4` | PB get position |
| sub_00351770 | 0 | manual/gen | Scene render (62K) |
| sub_003518E0 | 2 | `esp += 12` | PB kickoff (manual override) |
| sub_00351A30 | 0 | `esp += 4` | |
| sub_00351BD0 | 0 | `esp += 4` | |
| sub_00351DB0 | 1 | `esp += 8` | |

### How to Determine Calling Convention

For any D3D8LTCG mid-entry stub, find a call site in gen code:
```c
PUSH32(esp, param1);     // parameter 1 (if any)
PUSH32(esp, param2);     // parameter 2 (if any)
PUSH32(esp, 0);          // return address (ALWAYS present)
sub_XXXXXXXX();          // call
```

Count the PUSH32 instructions before the `PUSH32(esp, 0); sub_XXX()` pair:
- 0 extra pushes → 0 params → `esp += 4`
- 1 extra push → 1 param → `esp += 8`
- 2 extra pushes → 2 params → `esp += 12`

**Caveat**: Some pushes before a call are caller-saves (the caller pops them later), not parameters. Check whether the caller has a matching POP after the call returns.

## Device Context Initialization Sequence

For a recompiled Xbox game using D3D8LTCG:

1. **Allocate PB**: 4MB push buffer at a known Xbox VA
2. **Load xemu snapshot**: Copy 16KB device context from xemu debug session
3. **Apply PB fixups** (post-snapshot):
   ```c
   MEM32(dev + 0x00) = pb_start;      // PB write cursor
   MEM32(dev + 0x04) = pb_end;        // PB limit
   MEM32(dev + 0x24) = pb_start;      // PB ring start
   MEM32(dev + 0x28) = pb_end;        // PB ring end
   MEM32(dev + 0x2C) = 0;             // Write sequence counter
   MEM32(dev + 0x30) = dev + 0x2C;    // GPU read → write (no spin!)
   MEM32(dev + 0x34) = 0;             // Fence write index
   MEM32(dev + 0x38) = 3;             // Fence mask (4 entries)
   MEM32(dev + 0x44) = pb_size;       // PB ring size
   MEM32(dev + 0x48) = dev + 0x3000;  // Fence array → safe area
   MEM32(dev + 0x19FC) = 0;           // No pre-render callback
   MEM32(dev + 0x1974) = 0x3A1F;      // RT surface 0 (non-NULL!)
   MEM32(dev + 0x1978) = 0x3A25;      // RT surface 1 (non-NULL!)
   ```
4. **Per-frame**: Force camera active flag: `MEM32(dev + 8) |= 0x4000`

## Capturing Device Context from xemu

Use a GDB RSP client connected to xemu (port 1234):
```python
# Halt CPU, read 16KB at device address
device_data = gdb.read_memory(0x0035D6A0, 16384)
# Write as C array header
with open('d3d_device_snapshot.h', 'w') as f:
    f.write('static const unsigned char d3d_device_snapshot[] = {\n')
    for i in range(0, len(device_data), 16):
        f.write('    ' + ', '.join(f'0x{b:02X}' for b in device_data[i:i+16]) + ',\n')
    f.write('};\n')
```

See `tools/xemu_debug/capture_d3d_device.py` for the full capture script.
