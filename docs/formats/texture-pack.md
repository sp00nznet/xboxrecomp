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

A P8 texture's cache key folds its palette into the hash, so P8 textures
never match a pack entry.

Test vector, shared with the CLI's Python port: the 1034 bytes `0..255`
four times followed by `xboxrecomp` hash to `0x0B060D91FB09F186`; the
empty input to `0x40500CA3D9E617F3`; 64 zero bytes to `0x69597731194007AB`.

## Layout

All fields little-endian. Version 2 adds entry kinds, BC3 payloads and a
text scale; the reader takes version 1 and version 2 files.

```
offset  size  field
0       4     magic "XRTP"
4       4     version, 1 or 2
8       4     count
12      4     scale: the factor the art entries were made at (informational;
              logged as "scale N")
16      8     index offset (>= 32)
24      8     v1: reserved, 0 (a reader refuses any other value)
24      4     v2: text scale, the factor the TEXT entries were made at
              (informational; 0 means the header scale)
28      4     v2: reserved, 0

index: count entries of 32 bytes, sorted by hash, strictly increasing
0       8     hash (nv2a_tex_hash of the guest texture)
8       4     width  (1..16384)
12      4     height (1..16384)
16      4     levels (1 .. 1 + floor(log2(max(w, h))))
20      4     flags: v1 0; v2 below (a reader refuses any other value)
24      8     payload offset (>= 32)

payload: per entry, every level, level 0 first, each further level
halving width and height (floor, minimum 1):
  without XRTP_FLAG_BC3  one u32 A8R8G8B8 texel per pixel: the value
                         nv2a_tex_decode produces, which the backends
                         upload as BGRA8 bytes
  with XRTP_FLAG_BC3     the level's BC3 (DXT5) blocks, row of blocks by
                         row, 16 bytes per 4x4 block
```

v2 entry flags:

| Bits | Name | Meaning |
|---|---|---|
| 0-1 | kind | `0` art (HUD pieces and other images), `1` font (the atlas the glyphs are drawn from), `2` text (an image of pre-rendered text); `3` is refused |
| 4 | `XRTP_FLAG_BC3` | the payload is BC3 blocks |

Every other bit is refused, so a later meaning is never misread by this
reader. The kind is the unit a player turns off (`textures.font`,
`textures.text`, [config.md](../runtime/config.md)): a
turned-off entry is not found, and the guest texture goes up as without a
pack. The header scale describes the art; each entry carries its own size,
so text built at 2x and art at 4x share one pack.

A BC3 entry's width and height must be multiples of 4, so every level the
backends create from it is whole blocks; the reader refuses the file
otherwise ("BC3 entry not a multiple of 4"). BC3 is a quarter of the bytes
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
texels are read on first use and kept for the process. The sampler state
comes from the guest's registers as always, so a replacement is sampled
with the guest's filter and wrap modes and, since swizzled and DXT
textures use normalised coordinates, needs no other change. A linear
texture (texel coordinates) would be sampled at guest texel positions and
should not be put in a pack.

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

`candidate` marks a swizzled or DXT texture without mips, which a pack
can replace; `linear/mip` one it should not hold. A texture the pack does
hold is tagged with why it went up from the guest: ` (text off)`,
` (font off)` or ` (art off)` for a kind switched off, ` (pack off)`, or
` (entry refused)` when the backend could not use the entry (a GPU without
BC3, a size the device refuses, a failed read). The list is what an
owner checks after a run to see what a pack still misses.

Tests: `tests/tex_pack` (ctest `tex_pack`), `tests/host_opts_live` (turning
kinds off and on at a flip).
