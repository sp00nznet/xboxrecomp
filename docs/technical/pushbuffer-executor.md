# Pushbuffer Executor

A title that statically links the XDK's Direct3D never calls the toolkit's
D3D8 layer. It writes NV2A methods into a pushbuffer and kicks the GPU, and the
only way to see its frames is to execute that pushbuffer. `RECOMP_PB_SCAN`
surveys it; `RECOMP_PB_EXEC` executes it, on the NV2A poll thread, into guest
memory with a CPU rasteriser or into a render back end the game project
registers (`nv2a_backend.h`). This page is what it took to go from
`draws 0` to a playable *X-Men Legends* level, in the order the problems
appear.

## Reading the Pushbuffer Like the DMA Engine

The executor used to walk the bytes between the previous and the current
`DMA_PUT` in a straight line:

| Command | Old behaviour |
|---|---|
| `JUMP` | stopped the walk; the target was never read |
| `CALL` / `RETURN` | skipped; the subroutine's methods never ran |
| PUT below the last PUT (the ring wrapped) | the whole segment was skipped |

XDK D3D sends state blocks as CALLs into pre-built pushbuffers, and every
title's ring wraps many times a second, so the executor rendered with stale
state: wrong textures, world-space geometry pushed through D3D's two-instruction
pre-transformed program (`mov oPos, v0; mov oT0, v9`).

`nv2a_pb_scan.c` now keeps its own GET — a physical address, like the register
— and walks the way the DMA engine does: it follows `JUMP` (old and new style),
`CALL` and `RETURN` (one level of subroutine, as on NV2A), and stops when GET
reaches PUT. A jump or call outside RAM means the walk is out of step; it snaps
GET to PUT and logs `[PB] bad target ...`.

`DMA_GET` advances only once the walker has reached `DMA_PUT`. It used to be set
to PUT before executing, which tells D3D the ring space is free before anything
has read it. If the title moves GET itself, the walker resyncs, and it starts at
the title's first GET, so device state sent before the first kickoff (the depth
function, for one) is not lost. What D3D does with GPU progress once something
really consumes the ring is in
[D3D8LTCG Device Context](d3d8ltcg-device-context.md#when-something-does-read-the-pushbuffer).

## Kickoffs Must Not Wait for a Frame

`CDevice::KickOff` flushes the write-combine buffers before advancing
`DMA_PUT`:

```
[nv2a + 0x100410] |= 0x10000;
while ([nv2a + 0x100410] & 0x10000) ;
```

On hardware the bit clears at once. Here it was cleared from the `NV2A_ACK`
table, once per pass of `nv2a_ack_thread` — the same pass that executes and
draws the new pushbuffer segment. Each kickoff waited up to a frame, and a
level load is thousands of kickoffs long: *X-Men Legends*' mission load looked
frozen for minutes. The busy and idle flags are now acknowledged by their own
`nv2a_flag_thread`, which does nothing else; the frame rate rose about sevenfold.
The `DMA_GET` mirror stays in the executor's loop on purpose, for the reason
above.

The same thread paid for `getenv()` on every method — it scans the whole
environment, and a 3D frame is millions of methods. Debug switches are read
once.

## Fixed-Function Transform and Anti-Aliasing

The executor drew only batches already in screen pixels, and reported the rest:

```
rasterised 0 triangles; 8073 batches skipped as not screen-space
```

D3D uploads a composite matrix — world × view × projection × viewport — with
methods `0x0680`–`0x06BC`, and a viewport offset with `0x0A20`–`0x0A2C`. The
executor now captures both, with the transform mode, and `fetch_position()`
transforms, divides by w, adds the offset and scales by the anti-aliasing
factor. The 16 floats apply as `clip[i] = Σ M[4i+j] · pos[j]` (translation in
`M[3]`, `M[7]`, `M[11]`), **not** as a row vector times the matrix; the other
order puts every vertex near `(0, 0)`. `RECOMP_FFP_TRACE=1` prints the matrix
and a few raw and transformed vertices, which is how the order was found.

With multisampling on, the surface is 2× (or 2×2) the logical size, and
`SET_SURFACE_FORMAT` bits 12–15 say which. The executor used the logical
640-pixel width with the real 5120-byte pitch, got 8 bytes per pixel, and wrote
`0 pixels`. The clip rectangle is now scaled by that factor too, and the
framebuffer window follows the surface actually drawn.

## Primitives, Quads, Vertex Programs, Index Width

With transforms working, 2D was right and 3D came out as spikes radiating from
single points. Four separate bugs:

| Bug | Detail |
|---|---|
| Primitive codes off by one | 4/5/6/7 were used for triangles/strip/fan/quads. The hardware values — `NV097_SET_BEGIN_END_OP_*` in `nv2a_regs.h` — are 5/6/7/8, with 9 = quad strip and 10 = polygon. Every triangle strip, most of all geometry, was drawn as a fan around its first vertex. |
| Quads as one fan | `QUADS` and `QUAD_STRIP` went through the fan loop around index 0 of the whole batch, which is only right for one quad. |
| Vertex programs ignored | With `SET_TRANSFORM_EXECUTION_MODE` (`0x1E94`) selecting a program, attribute 0 is an object-space position, and it was used as a screen position. Programs (`0x0B00`–`0x0B7C`), constants (`0x0B80`–`0x0BFC`) and the load/start pointers (`0x1E9C`–`0x1EA4`) were not recorded. |
| 16-bit indices | The index list was `uint16_t`, but `DRAW_ARRAYS` starts are 24-bit, so vertices past 65535 wrapped to the start of the buffer. `ARRAY_ELEMENT32` (`0x1808`) was not handled. |

The executor now has one topology function for the rasteriser and the back end
(`for_each_triangle`, quads split per quad and quad strips per pair), 32-bit
indices with batch limits raised to 65536, the `S1`, `S32K` and packed-normal
`CMP` (11:11:10) vertex formats, and a CPU interpreter for vertex-program
microcode, `vp_run()`: MAC and ILU units, `a0` addressing, constant writes and
output registers, with the field layout from xemu's `vsh.c`. A program's own
tail leaves `oPos` in screen space with w still the clip w, so its result feeds
the back end exactly like a fixed-function vertex. Results are cached per vertex
index within a batch. `RECOMP_VP=0` turns the interpreter off, and program
batches are skipped again.

## Vertices Behind the Camera

`fetch_position` returned "no position" for `w <= 1e-6`, and both callers took
that as fatal: `batch_is_screen_space` rejected the whole batch and
`backend_tri` every triangle touching the vertex. A ground mesh is one long
strip, and from a tilted top-down camera some of its vertices are behind the
eye — so *X-Men Legends*' streets and sidewalks vanished and the clear colour
showed through, exactly `(0,0,0)`. Forcing the clear colour to magenta turns
such patches magenta: nothing was drawn.

Only `w = 0` is rejected now. Negative w passes through as a negative `rhw`,
and the back end rebuilds the clip-space position from it and lets the GPU
clip at the near plane, as the NV2A does. The CPU rasteriser, which cannot
clip, skips those triangles itself.

Two things a back end has to get right once such vertices arrive:

- **Clip at the near plane, clamp only at the far side.** With depth clipping
  off and depth clamped to [0, 1], a vertex just in front of the eye projects
  to huge x/y and is never clipped: flat, full-screen bands of colour whenever
  the camera turns.
- **Cull from projected positions.** D3D11 decides facing after clipping, so a
  triangle reaching behind the eye keeps its true winding; the NV2A decides from
  the projected, mirrored one. Decide culling per triangle from the projected
  screen positions and draw with the rasteriser's culling off — identical for
  triangles wholly in front of the eye. The cull state arrives in
  `Nv2aRenderState`.

### Normals facing away

Some of the same meshes store normals that face away from the camera, and that
is authentic: a write watch traced the road normal `(0, -1, 0.03)` to the
title's file reader, and it equals `(v1-v0)×(v2-v0)` of the first triangle. The
model-view matrix mirrors (determinant −1) and culling is off, so with
`max(0, N·L)` those surfaces got only the ambient term. `lit_color` flips an
eye-space normal that points away from the eye — two-sided lighting with the
back material equal to the front.

The flip is per vertex, not per face, and what does this on hardware is not
known: the title writes `0x17C4` (two-sided lighting) as 0, and xemu uses front
colours only. Check against xemu or hardware if a surface ever looks lit from
the wrong side.

## Render Back Ends

`nv2a_backend.h` is the interface. A game project registers `clear`, `draw` and
`flip`, and receives surfaces, already-transformed triangle lists in surface
pixels with per-vertex colour and texel-space UVs, the texture each batch
samples (`nv2a_backend_decode_texture` decodes every format the executor can
sample), and blend, alpha, depth, colour-mask and cull state in NV2A's own
enum values. All calls come from the NV2A poll thread, one at a time, so a back
end can create its window and device lazily and pump messages from `flip`.
Register-combiner state is not passed on yet.

## Switches

| Variable | Does |
|---|---|
| `RECOMP_PB_SCAN` | read-only survey of the pushbuffer; ranks what is not implemented |
| `RECOMP_PB_EXEC` | execute it |
| `RECOMP_PB_EXEC_VERBOSE` | per-batch trace |
| `RECOMP_TRACE_FLIP=<n>` | dump 3000 batches starting at flip n, to inspect a given screen |
| `RECOMP_FFP_TRACE` | composite matrix and sample vertices |
| `RECOMP_VP=0` | skip vertex-program batches instead of interpreting them |
