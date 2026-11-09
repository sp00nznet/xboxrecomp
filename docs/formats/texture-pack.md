# Texture pack (`XRTP`)

A texture pack holds host-side replacements for guest textures, keyed by
the hash the GPU backends compute for every texture they upload. The
enhancements layer loads one through `textures.pack`
([config.md](../runtime/config.md)); the Metal and D3D11
backends look each upload's texel hash up in it and, on a hit, create the
host texture at the pack entry's size from the pack's texels instead of
the decoded guest texels. The guest texture, the game's files and the CPU
rasteriser are untouched.

A pack is made from a game's own files, so it belongs to the owner of
those files: it is generated on their machine (`xbr enhance hud` in
xboxrecomp-cli) and is not something to distribute.

## Key

The key of an entry is `nv2a_tex_hash` (`src/kernel/nv2a_backend_common.c`:
a four-lane FNV-1a over 32-byte groups, the tail byte by byte, the lanes
folded at the end; seed `NV2A_TEX_HASH_SEED`) over the bytes
`nv2a_tex_extent` gives for the guest texture: for a swizzled or DXT
texture, every level packed (for DXT, the block bytes); for a linear one,
`pitch * (h - 1) + w * bytes_per_texel`. For a DDS file that the game loads
with D3DX as it is, that is the file's data block, which is how a pack can
be built offline from the game's files.

A P8 texture's key folds its palette in (`nv2a_stage_hash`): the hash of the
indices, xor 31 times the hash of the palette's `4 * entries` bytes, with the
same function and seed. An entry whose hash comes from the runtime dump
(texture-dump.md) therefore matches that one palette, and the same indices
under another palette miss. The pack holds decoded texels, never a palette.
The archive path can't name such a hash, since an archive reader has no
palette in hand, so P8 is reached only through a dump.

A v5 recolour entry (`XRTP_FLAG_RECOLOUR`, below) is keyed by the first term
alone, the index plane's hash (`nv2a_stage_plane_hash`), and serves every
palette of that plane. Lookup order for a 2D upload (`nv2a_stage_pack_find`):
the full hash's exact entry first, then, for a P8 texture and only when the
pack holds a recolour entry, the plane hash's recolour entry. A pack without
recolour entries never computes a plane hash. The texture caches stay keyed
by the full hash, so a (plane, palette) is recoloured when it goes up and
then hits the cache like any texture until that cache evicts it. A plane
whose palette keeps changing (a palette fade or cycle) is not recoloured: see
"Volatile palettes" below.

Test vector, shared with the CLI's Python port: the 1034 bytes `0..255`
four times followed by `xboxrecomp` hash to `0x0B060D91FB09F186`; the
empty input to `0x40500CA3D9E617F3`; 64 zero bytes to `0x69597731194007AB`.

## Layout

All fields little-endian. Version 2 adds entry kinds, BC3 payloads and a
text scale. Version 3 adds kind 3 (world), BC1 payloads and the `classes`
word. Version 4 adds the `XRTP_FLAG_DETILE_HEX` mark on world entries
(`textures.detile`). Version 5 adds `XRTP_FLAG_RECOLOUR` entries. Version 6
adds the `XRTP_FLAG_MIP_BLEND` mark. The reader takes all six, each by its
own rules. A writer uses v5 only for a pack that holds a recolour entry, and
v6 only for one that holds a mip blend mark, so every other pack stays
readable by older runtimes.

```
offset  size  field
0       4     magic "XRTP"
4       4     version, 1 to 6
8       4     count
12      4     scale: the factor the art entries were made at (informational;
              logged as "scale N")
16      8     index offset (>= 32)
24      8     v1: reserved, 0 (a reader refuses any other value)
24      4     v2 to v6: text scale, the factor the TEXT entries were made at
              (informational; 0 means the header scale)
28      4     v2: reserved, 0
28      4     v3 to v6: classes, a bit per kind present (1 << kind); informational,
              the reader does not check it

index: count entries of 32 bytes, sorted by hash, strictly increasing
0       8     hash (nv2a_tex_hash of the guest texture; of its index
              plane for a recolour entry)
8       4     width  (1..16384)
12      4     height (1..16384)
16      4     levels (1 .. 1 + floor(log2(max(w, h))))
20      4     flags: v1 0; v2 to v6 below (a reader refuses any other value)
24      8     payload offset (>= 32)

payload: per entry, every level, level 0 first, each further level
halving width and height (floor, minimum 1):
  no block flag          one u32 A8R8G8B8 texel per pixel: the value
                         nv2a_tex_decode produces, which the backends
                         upload as BGRA8 bytes
  XRTP_FLAG_BC3          the level's BC3 (DXT5) blocks, row of blocks by
                         row, 16 bytes per 4x4 block
  XRTP_FLAG_BC1 (v3)     the level's BC1 (DXT1) blocks, always opaque, row of
                         blocks by row, 8 bytes per 4x4 block
  XRTP_FLAG_RECOLOUR (v5) one u32 per pixel: bits 0-7 i0, 8-15 i1, 16-23 w,
                         24-31 zero (as bytes: i0, i1, w, 0)
```

Entry flags:

| Bits | Name | Meaning |
|---|---|---|
| 0-1 | kind | `0` art (HUD pieces and other images), `1` font (the atlas the glyphs are drawn from), `2` text (an image of pre-rendered text); `3` world (textures of the 3D scene), v3 to v6 |
| 4 | `XRTP_FLAG_BC3` | the payload is BC3 blocks |
| 5 | `XRTP_FLAG_BC1` | the payload is BC1 blocks, v3 to v6; never with bit 4 |
| 6 | `XRTP_FLAG_DETILE_HEX` | v4 to v6, world entries only (a reader refuses it on another kind): with `textures.detile` on, the entry samples hex-tiled (`src/d3d/nv2a_hex_tile.h`). The payload is unchanged; a writer marks only organic tiles, never bricks or planks. Only a stage that wraps on both axes samples hex-tiled (`nv2a_hex_wrap_ok`); clamp, mirror and border keep the stock read. If the hex shader does not compile, the draw keeps the stock shader |
| 7 | `XRTP_FLAG_RECOLOUR` | v5 and v6: a P8 index plane's entry (see Recolour entries); never with bit 4 or 5 (a reader refuses the file) |
| 8 | `XRTP_FLAG_MIP_BLEND` | v6: under a guest texture of one level, sample the entry's chain linearly between levels with a LOD bias, instead of the nearest level (see below). The payload is unchanged; any kind |

Each version refuses what it does not define (a v2 file with kind 3 or bit 5,
any file with another bit), so a later meaning is never misread by this
reader. A runtime from before v4 refuses a v4 pack whole (bit 6 is a flag it
does not know), so a toolkit that reads v4 must ship before, or with, the
tool that writes it; the same holds for v5 and bit 7, and v6 and bit 8. The kind is the unit a player turns off (`textures.font`,
`textures.text`, `textures.world`, [config.md](../runtime/config.md)): a
turned-off entry is not found, and the guest texture goes up as without a
pack. The header scale describes the art; each entry carries its own size,
so text built at 2x and art at 4x share one pack.

A block entry's width and height must be multiples of 4, so every level the
backends create from it is whole blocks; the reader refuses the file
otherwise ("block-compressed entry not a multiple of 4"). BC1's test vector,
which the CLI's writer is to share: the block `00F81F0000555555` has endpoints
0xF800 and 0x001F, its first row index 0 (opaque red, `FFFF0000`), the other
three index 1 (opaque blue, `FF0000FF`). BC3 is a quarter of the bytes
of A8R8G8B8 on disk and in video memory, and is uploaded as it is. Test
vector, shared with the CLI's encoder: the 16 texels whose RGBA bytes are
`0aff00002ff2031f54e50c2f79d81b3f9ecb304fc3be4b00e8b16c6f0da4937f3297c08f578af39f7c7d2cafa1706bbfc663b0cfeb56fbdf10494cef353ca3ff`
encode to `ff00f9ed94e42601ffe98017d5efbbb2` and decode to
`10f3000010f3002410f300245ab655485ab655485ab65500a479aa6d5ab655915ab65591a479aa915ab655b6a479aab6a479aadaef3cffda5ab655ffa479aaff`
(alpha exact; the toolkit's decoder expands 5- and 6-bit colour by
`v * 255 / max`, so its colour may differ by 1 from that bit-replicated
decode).

The reader (`src/kernel/nv2a_tex_pack.c`) checks the magic and version,
the reserved field and the flags, that the index and every entry's payload
lie inside the file, the sizes, and the sort order, and refuses the whole file otherwise; the layer logs
the reason and the run stays stock. The index is read at load; an entry's
payload is read when a backend uploads it and freed as soon as the backend
holds the GPU copy (`nv2a_tex_pack_release`), so a large pack costs VRAM and
not the same bytes again in RAM; a later upload reads it again.
`RECOMP_DEBUG=tex_pack_keep` keeps the payloads instead, for A/B, while the
read entries total at most 512 MB (`NV2A_TEX_PACK_RESIDENT_MAX`); past it the
least recently used entries' CPU copies are freed. That budget also bounds
any payload a backend has not released yet. A re-read checks that the file still has the size and mtime it
had at load: a pack rebuilt in place while the game runs would hold other
bytes at the checked offsets, so it is not read again (said once in the
log), and the textures not held go up from the guest until the next load.
The file is opened before anything is freed, so a pack that cannot be
read keeps what is held.

The sampler state comes from the guest's registers as always, so a replacement
is sampled with the guest's filter and wrap modes and, since swizzled and DXT
textures use normalised coordinates, needs no other change, except the mip
range: the guest's LOD clamps count its own levels, and a pack entry's level 0
is finer. Both samplers follow the entry (`nv2a_stage_lod`). With `scale` the
entry's width over the guest's, when that is an exact multiple of at most 15
(1 otherwise, and the stock clamps), and `shift = log2(scale)`:

- a guest texture that mips keeps its mode, and its top clamp moves to
  `max + shift`; its bottom clamp stays 0 when it was 0, or the entry's finer
  levels could never be reached, and moves by `shift` when the guest had
  clamped its own level 0 away;
- a guest texture that does not mip, under an entry with more than one level,
  samples `[0, shift]` with the nearest level. Without it the entry aliases
  worse than stock, and past `shift` the far end is what the guest showed;
- an entry with one level has nothing to choose from: `[0, 0]`, as before.

An entry marked `XRTP_FLAG_MIP_BLEND` (v6) changes only the second case: the
range stays `[0, shift]`, but the stage blends the two nearest levels and
biases the LOD by -0.5 (`NV2A_STAGE_BLEND_BIAS`; `RECOMP_DEBUG=pack_mip_bias=x`
sets it, -2 to 2, for A/B). Text, the font and HUD art are drawn minified at
fractional ratios (BLiNX 2's font at about 0.81 of its 4x entry at render
scale 2, HUD art at 0.5): the nearest level then snaps to a level up to twice
too coarse or too fine, and a blend half a level sharp follows the ratio. The
CLI marks the chains it builds down to the guest size (`mips = "guest"`),
whose levels are the entry at 1/2^k of its scale, so each level's texel edges
still fall on the guest's. A guest texture that mips, and an entry of one
level, sample as without the mark. D3D11 sets the bias in the sampler
(`MipLODBias`); Metal has none and adds it in the shader (`tex_bias`).

A linear texture (texel coordinates) would be sampled at guest texel positions
and should not be put in a pack.

A sidecar `hud-pack.toml` written by the CLI beside the pack describes
the entries for people (archive entry names, kinds, hashes, sizes, the
scales and kernel, the languages, the game's XBE sha256); the runtime never
reads it.

## Misses

While a pack is loaded, a backend upload the pack did not replace is
counted in a ring of 64 distinct (hash, size, format) keys; when it is
full the oldest gives way. Under `RECOMP_TRACE=tex_pack` the backends
print it every 600 flips, most counted first:

```
[TEXPACK] miss 0123456789ABCDEF 256x64 DXT5 x42 candidate
```

`candidate` marks a swizzled or DXT texture, P8 and mipped ones too, which a
pack can replace; `linear` one it should not hold. A texture the pack does
hold is tagged with why it went up from the guest: ` (text off)`,
` (font off)`, ` (world off)` or ` (art off)` for a kind switched off,
` (pack off)`, ` (palette volatile)` for a recolour entry skipped while its
plane's palette keeps changing, or ` (entry refused)` when the backend could not use the entry
(a GPU without BC1 or BC3, a size the device refuses, a failed read). The list is what an
owner checks after a run to see what a pack still misses.

Tests: `tests/tex_pack` (ctest `tex_pack`), `tests/host_opts_live` (turning
kinds off and on at a flip).

## Recolour entries (v5)

A P8 texture whose palette the game builds at run time (BLiNX 2's character
editor tints each part's palette per character) has a new full hash for
every palette, so a dump can never cover them all. A recolour entry stores
the upscale of the index plane instead of colours: per output texel, two
palette indices and a weight. At upload, with the palette the game has bound
(`nv2a_tex_pack_recolour`, on the CPU in the kernel library, so D3D11 and
Metal share it), each texel is, per A8R8G8B8 channel, alpha included:

```
out = (pal[i0] * (255 - w) + pal[i1] * w + 127) / 255
```

`w = 0` gives `pal[i0]` exactly and `w = 255` gives `pal[i1]`, so a 1x entry
with `w = 0` and `i0` the guest index reproduces the guest decode bit for
bit. An index at or past the palette's length reads magenta
(`0xFFFF00FF`), as in the decode. Test vector, shared with the CLI's
writer: under the palette `FF000000, FFFFFFFF, 80FF0000`, the texels
`00 01 00 00`, `00 01 FF 00`, `00 01 80 00`, `02 02 40 00`, `00 03 00 00`,
`03 00 00 00` (bytes i0, i1, w, 0) recolour to `FF000000`, `FFFFFFFF`,
`FF808080`, `80FF0000`, `FF000000`, `FFFF00FF`.

The reader computes the formula exactly, without a division: red and blue in
one word and alpha and green in another, each lane's `t / 255` as
`(t + 128 + ((t + 128) >> 8)) >> 8`. `tests/tex_pack` checks it against the
formula for every pair of bytes and weight.

Rules a writer follows:

- The hash is `nv2a_tex_hash` over the guest index bytes `nv2a_tex_extent`
  gives (swizzled, every level packed): the stage hash's first term, before
  the palette is folded in.
- The size, levels and kind follow the usual rules; the aspect must match the
  guest texture's. Levels are index pairs too, each picked for its own size,
  never blended from a finer level's colours.
- The entry is uncompressed (no BC flag): blocks would blend indices. On the
  GPU the recoloured texels are A8R8G8B8, 4 bytes per texel (about 2.25 MB
  for a 256x256 at 3x, mips included).
- A recolour entry's payload stays held under the resident budget after its
  upload (release keeps it), since the plane's next palette reads it again.
  The one recolour output buffer, as large as the largest entry recoloured,
  counts in the same budget; release frees it when it is past 8 MiB
  (`NV2A_TEX_PACK_RECOLOUR_KEEP`), so one huge entry does not hold that
  memory for the rest of the run.
- The exact entry of a (plane, palette) pair, built from a dump, wins over the
  recolour entry: keep those where a dump saw the palette, and add the
  recolour entry for the rest.

### Volatile palettes

Each new palette is a new full hash, so a plane whose palette the game
animates (water, fire, a flashing HUD) would be recoloured on the CPU at
every change and add one upscaled texture per palette to the caches. The
backends keep one cache slot per texture, so even a short cycle of palettes
the plane has shown before re-uploads at every step. The reader keeps, per
recolour entry, the palette of its last upload and counts each upload under
another one against a clock that `nv2a_pb_exec` advances at each flip.
When more than 12 changes arrive within 30 flips the plane turns volatile:
`nv2a_tex_pack_find_recolour` returns no entry, so the guest texels go up
(the plane's exact entries still apply), and the miss list tags it
` (palette volatile)`. The texture-cache generation is bumped then, so the
copies already recoloured go up again from the guest and an animation never
mixes the two. The flip that ends 60 flips without a change makes the plane
recoloured again and bumps the generation once more, so its guest copies,
still cached, go up recoloured. A bump re-uploads every cached texture, the
guest's included, and re-reads pack payloads, so each later return of the
same plane waits twice as long (120, 240, ... flips, at most 1920): a fade
that recurs costs fewer bumps each time. Uploads in the two flips after any
bump (a pack load, a switch, a plane turning volatile) re-upload known
textures and are not counted. A plane whose palette changes slowly, such as
a character editor tinting a part as the player picks colours, stays
recoloured. The rule is per plane and keys on no game address.
`nv2a_tex_pack_volatile_count` gives the number of planes volatile now.

When a backend refuses a recolour entry's texture (`tex_alloc` on D3D11,
`tex_make` on Metal), it still calls `nv2a_tex_pack_release`, so a large
recolour buffer is freed on that path too.

The texture dump (texture-dump.md) writes a P8 texture's `plane_hash` and
`palette_argb` in its sidecar, so the CLI joins a dump to a game file's plane
by key and can check a recolour entry against the palettes the game used.
