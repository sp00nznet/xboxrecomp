# Getting the Guest XBE Out of a BC Package (SVOD)

[ms-fusion-corpus.md](ms-fusion-corpus.md) §5 notes that Microsoft's byte-dense address map is a
coverage oracle for our own function detection, and `tools/fusion/coverage_oracle.py` implements
the grading. It takes two arguments:

```
py -3 -m tools.fusion.coverage_oracle <module.dll> <our_functions.json>
```

The second is *our* detection for the donor title — which means you need to run our pipeline over
the donor's original Xbox `default.xbe`. That XBE is not in the recompiled DLL. It is inside the
package's SVOD container, and the assumption that the container was sealed is why the oracle sat
in the tree with nothing to feed it. It is not sealed. `tools/fusion/svod.py` reads it.

**White-room framing.** This documents a container format and ships a reader. The guest XBE it
produces is the user's own property, read out of a package they bought; nothing in this repository
is derived from Microsoft data, and no container, XBE or fixture extracted from one is committed —
`tools/fusion/test_svod.py` tests the arithmetic and the parser against synthetic input only.

Measured on all four shipped BC titles on 2026-09-19.

---

## 1. Container layout

```
Content/Game/DefaultPackage            0xB000 PIRS header
Content/Game/DefaultPackage.data/Data0000 … DataNNNN
```

Every full `Data` file is `0xA290000` bytes = 41,616 blocks of `0x1000`:

```
block 0                                L1 hash block
then 203 × [ 1 L0 hash block + 204 data blocks ]

1 + 203 × (1 + 204) == 41,616          exactly — no slack, no trailer
    203 × 204       == 41,412          logical data blocks per full file
```

The final `Data` file is short and stops mid-group; there is no padding to a group boundary.

Logical block *n* resolves to:

```
file   = n // 41412
offset = (2 + (n % 41412) // 204 * 205 + (n % 41412) % 204) * 0x1000
```

**The hash blocks are checkable, and that is the cheap proof the stride is right.** For a hash
block at physical `H`:

```
sha1(data[H+0x1000 : H+0x2000]) == data[H : H+20]
```

Each hash block's first entry covers the block immediately after it. A wrong group size fails on
the second hash block, so this costs two SHA-1s to disprove and 64 to be confident.
`svod.py verify` does it.

---

## 2. The inner filesystem

XDVDFS. `MICROSOFT*XBOX*MEDIA` sits at logical offset 0 — physical `0x2000` of `Data0000`, i.e.
immediately after the L1 and first L0 hash blocks. The volume descriptor gives:

| offset | field |
|---|---|
| `+0x00` | `MICROSOFT*XBOX*MEDIA` |
| `+0x14` | u32 LE root directory sector |
| `+0x18` | u32 LE root directory table size |
| `+0x7EC` | `MICROSOFT*XBOX*MEDIA` again |

A directory table is a binary tree of entries, each 14 fixed bytes then the ASCII name:

```
struct.unpack('<HHIIBB', d[p:p+14])  ->  (left, right, start_sector, size, attr, namelen)
```

`left` and `right` are offsets to the subtree nodes **in 4-byte units** from the start of the
table; 0 means no child.

**`attr` is not a file/directory discriminator beyond bit 4.** Across the four titles plain files
carry `0x21` (Crimson Skies), `0x20` (its PNGs) and `0x80` (Blinx, Conker) with no pattern — code
that keys off "is it `0x20`" drops files on some titles and not others. `0x10` reliably means
directory.

**Walk the tree from the volume descriptor; do not scan for `default.xbe`.** Most raw ASCII hits
for that string in a container are path strings *inside* XBEs, and the 14 bytes before them decode
as garbage sectors and sizes.

---

## 3. The sector base is per title

Sector numbers in the filesystem are sectors of the *original disc*. The SVOD stream is a slice of
that disc, and `base` is the sector the slice starts at:

```
logical_byte = (sector - base) * 2048
```

| Title | base | root dir sector | `default.xbe` sector |
|---|---:|---:|---:|
| Fuzion Frenzy | 32 | 34 | 66 |
| Crimson Skies: High Road to Revenge | 32 | 34 | 462,518 |
| BLiNX: The Time Sweeper | 880,036 | 1,693,790 | 1,693,792 |
| Conker: Live & Reloaded | 956,202 | 1,711,833 | 1,711,836 |

For the two titles whose slice is the whole game partition it is the XDVDFS constant 32. For the
other two it is not, and **assuming 32 reads the wrong data rather than failing** — the read
succeeds, it is just 800,000 sectors off.

`svod.py` derives it. The candidates come from the PIRS header (below) and from 32, and each is
tested by using it:

1. the root directory table at `(root_sector - base) * 2048` must parse;
2. it must contain `default.xbe` — every Xbox game partition has one;
3. that entry's own sector, resolved with the same base, must start `XBEH`.

Step 1 alone is not enough, and Conker is the counterexample: its directory tables are packed into
consecutive sectors, so bases 956,201 through 956,207 *all* parse into plausible entry lists
(`ArialUni.Ttf` / `Sound.xsb`, `Audio` / `aid` / `fmv`, `Boot` / `Multiplayer` / `Singleplayer`, …).
Only 956,202 also resolves `default.xbe` to an XBE header.

### The PIRS header hint

The XContent SVOD volume descriptor at `+0x379` of the 0xB000 header carries a data-block offset at
`+0x1C`. `2 × <that value read little-endian, 3 bytes>` lands on the base or two sectors past it:

| Title | device features `+0x18` | value | 2 × value | true base |
|---|---|---:|---:|---:|
| Fuzion Frenzy | `0x00` | 16 | 32 | 32 |
| Crimson Skies | `0x00` | 16 | 32 | 32 |
| BLiNX | `0x40` | 440,019 | 880,038 | 880,036 |
| Conker | `0x40` | 478,102 | 956,204 | 956,202 |

The block *count* in the same descriptor at `+0x19` is big-endian and matches the logical block
total to within one, so the byte order of the offset field is genuinely inconsistent with its
neighbour — or our reading of one of them is. Four samples is not enough to claim the two-sector
gap on the `0x40` (enhanced GDF layout) titles is anything but a unit convention we have not
pinned down, so `svod.py` treats these as hints that shorten the search and proves whichever one
it takes with the gate above.

---

## 4. The validation gate — this is the part that matters

A flat contiguous copy of the `Data` files, hash blocks and all, **still parses as a valid XBE**.
It reports the correct title, build date, base address, image size, section names and even the
correct XDK library version, because all of that lives in the first `0x1000` bytes and the first
data block is genuine. Three separate extractions passed a header-only check on 2026-09-19 and
were wrong every `0x1000` bytes after the first.

`xbe_gate()` in `svod.py` implements the tells that actually fire. Run against the flat copy of
Fuzion Frenzy's `default.xbe` as a negative control:

```
ok   magic    b'XBEH'
ok   header   base=0x00010000 headers=0x974 image=0x316F20
FAIL thunks   no key gives a thunk run
FAIL tls      data 0x43020000..0x3D088889 zerofill=1986098990
```

and against the correct extraction of the same file:

```
ok   magic    b'XBEH'
ok   header   base=0x00010000 headers=0x974 image=0x316F20
ok   thunks   111 retail ordinals at VA 0x00253360
ok   tls      data 0x00000000..0x00000000 zerofill=12
```

- **thunks** — the strongest. The kernel import table is a run of `0x8000xxxx` ordinals terminated
  by 0, and it must be at the VA the header declares at `+0x158` XOR the retail (`0x5B6D40B6`) or
  debug (`0xEFB1F152`) key. Corruption displaces it and the run is not there. Equivalently: kernel
  imports resolve to real names instead of `Unknown_NNNN`.
- **tls** — the TLS directory's zero-fill size is a few bytes. A corrupt read gives ~1.9 billion,
  or `0x20202020` where a text block has landed on the directory.
- **header** — passes on corrupt input. Kept because it is free, not because it proves anything.

A header-only check is not a check. If you add a title, run `svod.py verify` first and read the
gate output on the extraction before trusting the XBE.

---

## 5. Using it

```bash
py -3 -m tools.fusion.svod verify  "<package>"                 # hash blocks + tree walk
py -3 -m tools.fusion.svod ls      "<package>" [inner/path]
py -3 -m tools.fusion.svod extract "<package>" default.xbe out.xbe
```

`<package>` is the package root, its `Content/Game` directory, or the `DefaultPackage.data`
directory itself. Then the oracle becomes runnable:

```bash
py -3 -m tools.disasm out.xbe --text-only
py -3 -m tools.fusion.coverage_oracle xefu_<hash>….dll tools/disasm/output/functions.json
```

Verified by extracting `default.xbe` from all four titles and comparing SHA-1 against independent
extractions: byte-identical on all four (2,834,432 / 3,219,456 / 44,728,320 / 7,774,208 bytes).
