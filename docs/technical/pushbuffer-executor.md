# Pushbuffer Executor

A title that statically links the XDK's Direct3D never calls the toolkit's
D3D8 layer. It writes NV2A methods into a pushbuffer and kicks the GPU, and the
only way to see its frames is to execute that pushbuffer. `RECOMP_PB_SCAN`
surveys it; `RECOMP_PB_EXEC` executes it, on the NV2A poll thread, into guest
memory with a CPU rasteriser or into a render back end the game project
registers (`nv2a_backend.h`).

The rasteriser itself — vertex programs (`nv2a_vsh_interp.c`), register
combiners (`nv2a_combiner.c`), four texture stages, near-plane clipping and a
threaded fill (`nv2a_pb_exec.c`) — arrived in v0.13.0. This page covers what
sits around it, in the order the problems appeared while bringing up
*X-Men Legends*: reading the ring, answering the title while it runs,
fixed-function transform and lighting, anti-aliased surfaces, and the back-end
interface.

## Reading the Pushbuffer Like the DMA Engine

The executor used to walk the bytes between the previous and the current
`DMA_PUT` in a straight line, following `CALL`s since v0.13.0 but still treating
each PUT-to-PUT span on its own:

| Case | Straight-line behaviour |
|---|---|
| `JUMP` | ended the span; the target was read only if it was a `CALL`'s |
| PUT below the last PUT (the ring wrapped) | the ring's bounds were learned from the PUTs seen, so the first lap lost a segment and every later wrap scanned "a little short of the end" |
| the title writes `DMA_GET` | not noticed |

XDK D3D sends state blocks as CALLs into pre-built pushbuffers and the ring
wraps many times a second, so every lost segment is state the executor never
saw: wrong textures, world-space geometry pushed through D3D's
two-instruction pre-transformed program (`mov oPos, v0; mov oT0, v9`).

`nv2a_pb_scan.c` now keeps its own GET — a physical address, like the register
— and walks the way the DMA engine does: it follows `JUMP` (old and new style),
`CALL` and `RETURN` (one level of subroutine, as on NV2A), and stops when GET
reaches PUT. The ring's wrap is itself a JUMP back to the start, so following
it replaces learning the ring's bounds. A jump or call outside RAM means the
walk is out of step; it snaps GET to PUT and logs `[PB] bad target ...` with
the last commands it decoded. If the title moves GET itself (XDK D3D resets the
ring by writing `DMA_PUT` and `DMA_GET` together) the walker resyncs, and it
starts at the title's first GET, so device state sent before the first
kickoff — the depth function, for one — is not lost.

`tests/nv2a_pb_walk` checks the walk with a stubbed executor: `CALL` and
`RETURN`, `JUMP`, non-increasing methods, a resync, and a bad target.

## Answering the Title While It Runs

XDK D3D asks the GPU questions while it submits, and the executor has to
answer them from inside the walk, because the thread that executes the ring is
the thread that would otherwise answer.

- **Kickoff.** Every kickoff sets `0x100410` bit 16 and spins until it clears.
  With the bits cleared only between passes of the poll loop, one pass can be a
  whole rendered frame, so a level load — thousands of kickoffs — looked
  frozen. The walker now clears them every 256 words
  (`xbox_Nv2aAckBusyBits`).
- **Fence mirrors.** A project that registers `xbox_Nv2aMirrorFence` has the
  poll thread copy the title's "submitted" fence into its "consumed" one. The
  walk now follows every CALL and JUMP, so a pass can be long, and a mirror
  ticked only between passes lags the title by a whole walk. D3D then takes its
  fence wait, which a mirrored fence cannot satisfy (Burnout 3 hung there at its
  first menu). `xbox_Nv2aAckBusyBits` ticks the mirrors as well, so they keep
  pace from inside the walk.
- **Semaphore.** `BACK_END_WRITE_SEMAPHORE_RELEASE` (`0x1D70`) writes its value
  where `nv2a_pb_set_semaphore_target()` was told, once everything before it
  has run. `SET_SEMAPHORE_OFFSET` is not added: in *X-Men Legends* it held
  `0xFF000000` at times. How a title's fence wait uses it is in
  [D3D8LTCG Device Context](d3d8ltcg-device-context.md#when-something-does-read-the-pushbuffer).
- **Method `0x0310` on subchannel 5.** D3D's `KickOff` ends every kick with it,
  carrying `((pushbuffer address * 8 | fence & 0x1F) << 2) | wrap & 3`. The
  GPU latches it in PGRAPH `0x400B10`, and D3D waits for the fence bits there.
  The executor latches the value. Without it `0x400B10` read 0 and the wait
  ended only when the semaphore was a multiple of 32.
- **`DMA_GET`.** It is the GPU's fetch pointer — nothing at or past it has been
  read — and D3D's fence wait relies on that: when its fence is far enough
  ahead of GET it patches a "wake me" software method into the pushbuffer at
  the fence and sleeps on an event. GET used to move only when a walk reached
  PUT, so it lagged the executor by up to a whole kick; D3D then patched
  commands the executor had already passed and slept for good. The walker now
  publishes its position before every command, and finishes on PUT. A GET that
  is not the value it last wrote means the title moved it (the ring reset), and
  the walk follows it rather than overwriting it.

## Switches Read Once

The NV2A poll thread paid for `getenv()` on every method — it scans the whole
environment, and a 3D frame is millions of methods. Debug switches are read
once, and the answer "not set" is cached as well as "set": a cache that only
remembered "set" ran `getenv` on every pass of the default path, where neither
`RECOMP_PB_SCAN` nor `RECOMP_PB_EXEC` is set. The method inventories (the
survey's and the unhandled-method ranking) are indexed by method instead of
searched: the survey's search was about 40% of the thread in a *X-Men Legends*
level.

## Fixed-Function Transform

Batches drawn with `SET_TRANSFORM_EXECUTION_MODE` (`0x1E94`) on "fixed" used to
count as "not screen-space" and be skipped unless their positions already
happened to be pixels:

```
rasterised 0 triangles; 8073 batches skipped as not screen-space
```

D3D uploads a composite matrix — world × view × projection × viewport — with
methods `0x0680`–`0x06BC`, and a viewport offset with `0x0A20`–`0x0A2C`, which
the executor keeps in the constant file at `c[59]`. With a composite matrix
uploaded and a fixed-function mode, `ffp_vertex()` transforms attribute 0,
divides by w and adds the offset, and hands the result on in the shape a vertex
program produces. So a fixed-function batch shares everything after the
transform with a program batch: near-plane clipping, the four texture stages,
the combiners and the depth buffer. The 16 floats apply as
`clip[i] = Σ M[4i+j] · pos[j]` (translation in `M[3]`, `M[7]`, `M[11]`),
**not** as a row vector times the matrix; the other order puts every vertex
near `(0, 0)`. `RECOMP_FFP_TRACE` prints the matrix and a few raw and
transformed vertices, which is how the order was found.

Texture coordinates are taken from the attribute streams as they are, with no
texture-coordinate generation or texture matrix; there is no fog coordinate,
specular, or point sprite.

### Lighting

With `SET_LIGHTING_ENABLE` on, the diffuse colour is computed per vertex from
the normal and up to eight lights, and the vertex's own colour stream is
ignored unless `SET_COLOR_MATERIAL` routes it in. Using the colour stream
anyway is what drew every lit mesh — characters, streets — black. XDK D3D
pre-multiplies the material into the light colours and folds emission and
ambient into `SCENE_AMBIENT_COLOR`, so the hardware sum is

```
colour = scene_ambient + Σ atten_i · (ambient_i + diffuse_i · max(0, N·L_i))
```

in eye space, alpha from `SET_MATERIAL_ALPHA`. The registers are read from a
mirror of the last value written to every 3D-class method (`s_reg`), because
lights and material colours are the kind of state the executor does not model
one field at a time. Normals go through the model-view matrix itself rather than
the inverse transpose (exact for rotations and uniform scale); there is no
specular, no spot cone, no back-face colour and no skinning.

### Normals facing away

Some meshes store normals that face away from the camera, and that is
authentic: a write watch traced the road normal `(0, -1, 0.03)` to the title's
file reader (*X-Men Legends*), and it equals `(v1-v0)×(v2-v0)` of the first
triangle. The model-view matrix mirrors (determinant −1) and culling is off, so
with `max(0, N·L)` those surfaces got only the ambient term.

What makes such a mesh look right on hardware is not established:
*X-Men Legends* writes `0x17C4` (two-sided lighting) as 0, and xemu uses front
colours only. So by default `lit_color` lights the normal as stored.
`RECOMP_FFP_FLIP_NORMALS` flips an eye-space normal that points away from the
eye instead — two-sided lighting with the back material equal to the front, per
vertex and not per face. It is a compatibility option, not hardware behaviour:
a game project sets it for a mesh it has seen need it, so it cannot change how
another title is lit. Check against xemu or hardware if a surface ever looks
lit from the wrong side.

## Anti-Aliased Surfaces

With multisampling on, the surface is 2× (or 2×2) the logical size, and
`SET_SURFACE_FORMAT` bits 12–15 say which. The executor used the logical
640-pixel width with the real 5120-byte pitch, got 8 bytes per pixel, and wrote
`0 pixels`. The clip rectangle is now scaled by that factor, and so are the
positions that come out of a vertex program or the composite matrix (the
viewport the title programs is in logical pixels). The near-plane clipper takes
its viewport scale and offset from the same place, so clipped triangles land
where unclipped ones do. The framebuffer window takes every other pixel of a
2×-wide surface instead of showing it black, and the host-side depth buffer is
wide enough for 1280×960.

## Index Width

The index list was `uint16_t` and held 4096 entries, but `DRAW_ARRAYS` starts
are 24-bit, so vertices past 65535 wrapped to the start of the buffer and big
batches were cut short. Indices are 32-bit now, batches may hold 65536, and
`ARRAY_ELEMENT32` (`0x1808`) is handled. An index comes from the pushbuffer, so
`fetch_attr` checks every vertex read against guest memory (ordinary RAM and the
contiguous window); a read outside it returns no vertex and the batch is dropped.
`SET_VERTEX_DATA_ARRAY_FORMAT` size 7
(`3W`) reads as three components.

## Render Back Ends

`nv2a_backend.h` is the interface. A game project registers `clear`, `draw` and
`flip`, and receives:

- the colour surface and its anti-aliasing factor;
- triangle lists in surface pixels, **already clipped at the near plane** by the
  executor, with per-vertex colour, `1/w`, and texel-space UVs — whichever path
  produced them (program, fixed-function or pre-transformed);
- the texture the batch samples (`nv2a_backend_decode_texture` decodes every
  format the executor can sample), with its mip level count and filter word;
- blend, alpha, depth, colour-mask, cull and stencil state in the NV2A's own
  enum values.

All calls come from the NV2A poll thread, one at a time, so a back end can
create its window and device lazily and pump messages from `flip`.

Things a back end should get right. These come from a Direct3D 11 back end
written against this interface for one title; that back end is not in this
tree, so treat them as advice from one implementation, not as tested behaviour
of the code here:

- **Window z is compared as it is.** `SET_CLIP_MIN`/`MAX` only clip. A title
  narrows the range for a low-detail layer so it is cut off near the eye; a back
  end that rescales each draw's z over its own range makes that layer about 237
  times deeper (the figure from that back end's title) and puts it in front of
  everything near.
- **Stencil.** A title can mark pixels with a stencil-writing mesh and then draw
  a full-screen quad that only the marked pixels pass. A back end that ignores
  `stencil_*` draws that quad over the whole surface.
- **Cull from projected positions.** D3D11 decides facing after clipping, so a
  triangle that reaches behind the eye keeps its true winding; the NV2A decides
  from the projected, mirrored one. Decide culling per triangle from the
  projected screen positions and draw with the rasteriser's culling off.

Not passed on yet: the register-combiner state, and texture stages 1–3 (a back
end sees stage 0 and the diffuse colour), so effects that need a second stage
or a combiner program only appear on the CPU rasteriser. Extend `Nv2aBatch`
when it matters rather than adding callbacks.

## Switches

| Variable | Does |
|---|---|
| `RECOMP_PB_SCAN` | read-only survey of the pushbuffer; ranks what is not implemented |
| `RECOMP_PB_EXEC` | execute it |
| `RECOMP_PB_WALK=dma` | walk the pushbuffer like the DMA engine (JUMPs, wraps, GET per command, the `0x0310` latch) instead of scanning each submitted segment; see [The DMA Walk Is Opt-In](#the-dma-walk-is-opt-in) |
| `RECOMP_PB_EXEC_VERBOSE` | per-batch trace |
| `RECOMP_FFP_TRACE` | composite matrix and sample vertices of the first fixed-function batches |
| `RECOMP_FFP_FLIP_NORMALS` | light the side of a fixed-function mesh that faces the viewer, even when the title left two-sided lighting off. Not hardware behaviour: a compatibility option for a game project to turn on, off by default |

## The DMA Walk Is Opt-In

`RECOMP_PB_WALK=dma` turns on the DMA-engine walk; without it the executor
scans each segment between the previous `DMA_PUT` and the new one, as it did
before the walk existed.

The walk is the more faithful of the two, and that is the reason it is opt-in.
Told exactly how far the GPU has got (`DMA_GET` per command and the `0x0310`
latch), XDK D3D's fence code takes its "GPU far behind" path: it patches a
software method (`NO_OPERATION` with a value) into the pushbuffer and sleeps on
an event that the GPU's interrupt for that method sets. Until the runtime
delivers that interrupt, a title that runs under the segment scan can freeze
under the walk on the first screen whose pushbuffer makes the walk long
enough. Burnout 3 did, at its Crash Nav menu, with the main thread in
`KeWaitForSingleObject` under D3D's fence wait.

Turn the walk on for a project that delivers the GPU's software-method
interrupt, or to bring up a title the segment scan draws wrongly.
