# Texture dump (`XRTD`)

`RECOMP_DEBUG=tex_dump=<dir>` makes the Metal and D3D11 backends write every
texture they upload into `<dir>` (created if missing), once per texel hash.
It is how an owner sees what a game draws and gets the decoded texels a
texture pack ([texture-pack.md](texture-pack.md)) is built from. The dump
holds the game's own texels: it stays on the machine of the owner of the
game files and is not something to distribute.

The CPU rasteriser has no uploads; under the same entry it keeps writing its
first-use BMPs with the value as a file prefix, and says once that it does
not write this dump.

## Key

The file name is the upload's texel hash, the one the backend's texture
cache uses and passes to the pack lookup (texture-pack.md, "Key"), as 16
upper-case hex digits. A dumped `<hash>` is the key of the pack entry that
would replace that texture. A P8 texture's hash has its palette folded in
(`nv2a_stage_hash`), so an entry built from its dump replaces that one palette:
the same indices under another palette are another hash and another dump.

## Files

Per hash:

- `<hash>.xtd`: a 64-byte header, then the decoded texels.
- `<hash>.json`: the facts about the texture (below).

Rewritten at each summary (every 600 flips and at a normal exit), when
anything changed; a run stopped by a signal (SIGINT from a bench timeout)
keeps the last 600-flip summary's, the `.xtd` and `.json` files being
written at the upload:

- `counts.json`: `{"<hash>": <uploads>, ...}`, every upload of each hash the
  dump saw, repeats included.
- `volatile.json`: `[{"offset", "width", "height", "hashes": [8 hashes]}]`,
  the addresses the dump gave up on.

Each file is written to `<name>.tmp` and renamed, so a killed run never
leaves half a file under its real name.

## `.xtd` layout

All fields little-endian.

```
offset  size  field
0       4     magic "XRTD"
4       4     version (1)
8       8     texel hash
16      4     width (level 0)
20      4     height (level 0)
24      4     levels
28      4     guest format (NV097_SET_TEXTURE_FORMAT_COLOR)
32      4     pitch (0 for swizzled and DXT)
36      4     flags: 1 swizzled, 2 linear, 4 DXT, 8 palette (P8)
40      4     first flip (the backend's present count when first seen)
44      4     data offset (64)
48      8     texel count, every level
56      8     reserved (0)
64      ...   texels
```

The texels are what `nv2a_tex_decode` produced: A8R8G8B8 words (B, G, R, A
in byte order), level 0 first, each level half the last in each axis (at
least 1), rows top to bottom. That is the payload layout of a raw pack
entry, so a dumped texture goes into a pack unchanged.

## `.json` sidecar

```json
{
  "hash": "1122334455667788",
  "width": 256, "height": 256, "levels": 9,
  "format": "DXT1", "format_id": 12, "pitch": 0,
  "layout": "dxt",
  "palette": false, "palette_len": 0,
  "wrap_u": "wrap", "wrap_v": "clamp_edge",
  "min_filter": "linear_mip_point", "mag_filter": "linear",
  "lod_min": 0, "lod_max": 8, "cube": false,
  "offset": "0x00A41000",
  "first_flip": 312,
  "texels": true
}
```

- `format`: the D3D name of the guest format, `LIN_` for the linear ones,
  `fmtXX` for one without a name; `format_id` is the register value.
- `layout`: `swizzled`, `linear` or `dxt`.
- `wrap_u`, `wrap_v`: `wrap`, `mirror`, `clamp_edge`, `border`, `clamp`, as
  sampled.
- `min_filter`: `point`, `linear`, `point_mip_point`, `linear_mip_point`,
  `point_mip_linear`, `linear_mip_linear`, `convolution`; `mag_filter`:
  `point`, `linear`, `convolution`.
- `offset`: the guest address at the first upload.
- `texels`: whether a `.xtd` holds the texels (below).

## What is not written

Counted in the summary instead:

- **Linear textures**: the sidecar only, `"texels": false`. Linear textures
  are movie frames and CPU-written surfaces, which a pack leaves out.
- **Volatile addresses**: one guest address and size (offset, width, height)
  that took more than 8 distinct hashes is given up on. Its first 8 stay
  written; it is named in `volatile.json`, and nothing more of it is
  written, not even a hash seen there before. A palette or a few animation
  frames swapped in place stay under the limit; a surface the CPU rewrites
  each frame does not.
- **Render targets**: a stage bound to a render target has no guest texels
  to hash. Binds and distinct targets (offset, size, format, up to 1024) are
  counted.
- **Over size**: a texture wider or taller than a pack entry may be (16384).

## Limits and errors

- **Byte cap**: one run writes at most 2048 MiB of `.xtd` files
  (`NV2A_TEX_DUMP_MAX_BYTES`, header and texels). That is far past a
  title's texture set; a BLiNX 2 level with its hub is about 25 MB. At the
  cap the dump stops with one line, `[TEXDUMP] dump stopped: it reached its
  2048 MiB cap; ...`. The uploads after it are only counted, and the
  summary line ends in `; stopped: <why>`. The same happens when the dump
  cannot grow its tables (out of memory).
- **A failed write** counts as an error and leaves the hash unwritten, so
  its next upload tries again, up to 3 times. A `.xtd` whose sidecar could
  not be written is removed: a `.xtd` never sits without its `.json`.
- **The exit summary** waits about 200 ms for the dump's lock. On Windows,
  `ExitProcess` can end the walker thread while it holds the lock. When the
  lock does not come, the summary is skipped with one line, and
  `counts.json` stays as the last 600-flip summary wrote it.

## The log

On: `[TEXDUMP] dumping each uploaded texture once per hash to <dir>`. Every
600 flips and at exit:

```
[TEXDUMP] flip 1800: 412 written (5321040 texels), 30211 repeat uploads, 3 linear (sidecar only), 2 volatile addresses (118 uploads skipped), 2400 render-target binds skipped (4 targets), 0 over size, 0 errors; <dir>
```

A directory that cannot be made turns the dump off with one line. Off, the
backends' only added work per upload is one test.

## The magnification trace

`RECOMP_TRACE=tex_mag` records how far each texture is ever magnified on
screen, what its upscale can be for. It works with or without the dump. With
the dump, each hash's `<hash>.json` also has these fields, rewritten by every
summary:

```
"mag_max": 12.000,     highest host pixels per source texel
"mag_scale1": 4.000,   mag_max / render_scale, the figure a pack is built for
"mag_draws": 3127,     readings folded in
"render_scale": 3
```

Without it, each hash gets one line at exit:
`[TEXDUMP] mag <hash> mag_max=... mag_scale1=... draws=... render_scale=...`.
Each summary and exit print `[TEXDUMP] tex_mag: N textures, highest X (<hash>),
K draws past the frame's slots; an upper bound: occluded fragments count`.

Under the trace the combiner shader of a program batch takes one variant
(`NV2ACombinerState.tex_mag`, in its cache key, so the stock text is untouched)
that reads each 2D stage's unclamped LOD and keeps the largest with an atomic,
at one pixel in 64 of a screen tile, in a slot per draw id and stage of a device
buffer read a frame or more behind (8192 draws a frame; more are counted). The
slot holds `(32 - lod) * 1024`, so `2^-lod` is the pixels per texel of what is
bound; a source texel is `scale` texels of a pack entry, so the fold multiplies
by the entry's scale. A pre-transformed batch takes no variant: its figure comes
from up to 64 of its triangles on the CPU (`nv2a_tex_mag_triangle`).

Not measured: 3D, cube, bump and dot-mapped stages, and batches that don't
reach the combiner shader or are not triangles. The figure is an upper bound:
an occluded fragment counts. Off, the only added work is one test per draw.
