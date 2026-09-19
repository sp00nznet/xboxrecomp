"""Read the SVOD container in a Microsoft Xbox BC package and extract from it.

tools/fusion/coverage_oracle.py grades our function-boundary detection against
Microsoft's `PrecompiledSymbolTable`, which names *guest* functions. To use it
you need the guest binary those names point at -- the donor title's original
Xbox `default.xbe`. That XBE is not in the recompiled DLL; it is inside the
package's SVOD container, which is why the oracle has sat in the tree with no
way to feed it a second argument. This reads the container.

The container is not encrypted. It is a plain XDVDFS filesystem with SHA-1
hash blocks interleaved into it at a fixed stride, so a flat `dd` of the Data
files yields a file that *still parses* -- correct title, build date, base
address, section names, even the right XDK library version -- and is wrong
every 0x1000 bytes. See `xbe_gate()` for the checks that actually catch that.

White-room: the guest XBE is the user's own property, read out of a package
they own. Nothing here ships Microsoft data; this is a reader, and the
repository carries no container, no XBE and no test fixture derived from one.

    py -3 -m tools.fusion.svod ls      <package_dir> [inner_path]
    py -3 -m tools.fusion.svod extract <package_dir> <inner_path> <out>
    py -3 -m tools.fusion.svod verify  <package_dir>

Layout, measured on all four shipped BC titles (Fuzion Frenzy, Blinx, Crimson
Skies, Conker L&R) on 2026-09-19. A package is:

    Content/Game/DefaultPackage             0xB000 PIRS header
    Content/Game/DefaultPackage.data/Data0000..N

Every full Data file is 0xA290000 bytes = 41,616 blocks of 0x1000:

    block 0                             L1 hash block
    then 203 x [1 L0 hash block + 204 data blocks]

    1 + 203 * (1 + 204) == 41,616                      exactly, no slack
    203 * 204           == 41,412 logical blocks per full file

The hash blocks are checkable and that is the cheap proof the stride is right:
for a hash block at physical H, `sha1(data[H+0x1000:H+0x2000])` equals
`data[H:H+20]` -- each hash block's first entry covers the block after it.
`verify_hashes()` does this; it is the first thing to run against a new title.

Inside, `MICROSOFT*XBOX*MEDIA` sits at logical offset 0 (physical 0x2000 of
Data0000). The XDVDFS volume descriptor gives the root directory's sector at
+0x14 and its size at +0x18.

The sector base is per title, and this is the part that bites:

    Fuzion Frenzy    base      32    root dir sector        34
    Crimson Skies    base      32    root dir sector        34
    Blinx            base 880,036    root dir sector 1,693,790
    Conker L&R       base 956,202    root dir sector 1,711,833

    logical_byte = (sector - base) * 2048

Sector numbers in the filesystem are original-disc sectors; the SVOD stream is
a slice of that disc and `base` is where the slice starts. For the two titles
whose slice is the whole game partition it is the XDVDFS constant 32. For the
other two it is not, and assuming 32 silently reads the wrong data rather than
failing. `derive_base()` solves for it instead -- see there for the gate.
"""
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
SECTOR = 0x800

# One hash block then 204 data blocks, 203 such groups after the L1 block.
DATA_BLOCKS_PER_GROUP = 204
GROUPS_PER_FILE = 203
BLOCKS_PER_FILE = 1 + GROUPS_PER_FILE * (1 + DATA_BLOCKS_PER_GROUP)      # 41,616
DATA_BLOCKS_PER_FILE = GROUPS_PER_FILE * DATA_BLOCKS_PER_GROUP           # 41,412
FULL_FILE_BYTES = BLOCKS_PER_FILE * BLOCK                                # 0xA290000

XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XDVDFS_DEFAULT_BASE = 32     # the on-disc sector of the volume descriptor
ATTR_DIRECTORY = 0x10


# ---- block mapping --------------------------------------------------------
def logical_to_physical(n):
    """Logical data block n -> (Data file index, byte offset in that file).

    Skips the L1 block at 0 and the L0 block that opens each group. Pure
    arithmetic: no file is consulted, so a caller can check the mapping
    against the hash blocks before trusting any data it reads.
    """
    if n < 0:
        raise ValueError(f"negative logical block {n}")
    file_index, within = divmod(n, DATA_BLOCKS_PER_FILE)
    group, in_group = divmod(within, DATA_BLOCKS_PER_GROUP)
    block = 2 + group * (1 + DATA_BLOCKS_PER_GROUP) + in_group
    return file_index, block * BLOCK


def hash_block_offsets(file_index=0):
    """Physical offsets of the hash blocks in a Data file, in order.

    Block 0 is the L1 block; each group then opens with its L0 block. Only the
    stride matters to us -- we never validate the tree, just use the hashes as
    a witness that the data blocks are where the arithmetic says.
    """
    yield 0
    for g in range(GROUPS_PER_FILE):
        yield (1 + g * (1 + DATA_BLOCKS_PER_GROUP)) * BLOCK


# ---- directory entries ----------------------------------------------------
class DirEntry:
    """One XDVDFS directory entry: 14 fixed bytes then the ASCII name.

        <HHIIBB  ->  left, right, start_sector, size, attr, namelen

    `left`/`right` are offsets to the subtree nodes *in 4-byte units* from the
    start of the table; 0 means no child. `attr` is DOS-ish, and only the
    directory bit is worth reading: the four titles use 0x21, 0x20 and 0x80
    for plain files with no pattern, so anything that keys off "is it 0x20"
    will drop files on some titles and not others. 0x10 is reliable.
    """
    __slots__ = ("name", "sector", "size", "attr")

    def __init__(self, name, sector, size, attr):
        self.name = name
        self.sector = sector
        self.size = size
        self.attr = attr

    @property
    def is_dir(self):
        return bool(self.attr & ATTR_DIRECTORY)

    def __repr__(self):
        return f"DirEntry({self.name!r}, sector={self.sector}, size={self.size})"


def parse_directory_table(table):
    """Walk an XDVDFS directory table into a list of DirEntry, or raise.

    Raising on anything malformed is the point: this doubles as the predicate
    `derive_base()` uses to reject a wrong base. A table read at the wrong
    sector is overwhelmingly likely to fail one of these -- a child offset
    past the end, a name with a byte outside printable ASCII, or a cycle.
    """
    entries = []
    seen = set()
    stack = [0]
    while stack:
        pos = stack.pop()
        if pos in seen:
            raise ValueError(f"cycle in directory table at +0x{pos:x}")
        if pos + 14 > len(table):
            raise ValueError(f"entry at +0x{pos:x} past end of {len(table)}-byte table")
        seen.add(pos)
        left, right, sector, size, attr, namelen = struct.unpack_from("<HHIIBB", table, pos)
        if namelen == 0 or pos + 14 + namelen > len(table):
            raise ValueError(f"bad name length {namelen} at +0x{pos:x}")
        raw = table[pos + 14:pos + 14 + namelen]
        if any(b < 0x20 or b > 0x7E for b in raw):
            raise ValueError(f"non-printable name at +0x{pos:x}: {raw!r}")
        entries.append(DirEntry(raw.decode("ascii"), sector, size, attr))
        for child in (left, right):
            if child:
                stack.append(child * 4)
    return entries


# ---- base derivation ------------------------------------------------------
def derive_base(read, root_sector, root_size, candidates):
    """Find the sector base: the disc sector the logical stream starts at.

    `read(offset, length)` reads the logical stream. Each candidate base is
    tested by actually using it, because a wrong base does not fail loudly:

      1. the root directory table at (root_sector - base) * 2048 must parse;
      2. it must contain `default.xbe` -- every Xbox game partition has one;
      3. that entry's own sector, resolved with the same base, must start
         `XBEH`.

    Step 1 alone is not enough. Conker's directory tables are packed into
    consecutive sectors, so bases of 956,201 through 956,207 all parse into
    plausible-looking entry lists; only 956,202 has a root that also resolves
    default.xbe to an XBE header. Steps 2 and 3 are what make it unambiguous.
    """
    for base in candidates:
        if base > root_sector:
            continue
        try:
            table = read((root_sector - base) * SECTOR, root_size)
            if len(table) < root_size:
                continue
            entries = parse_directory_table(table)
        except ValueError:
            continue
        for e in entries:
            if e.name.lower() == "default.xbe" and e.sector >= base:
                if read((e.sector - base) * SECTOR, 4) == b"XBEH":
                    return base
    raise ValueError("no sector base resolves the root directory to a disc with default.xbe")


def header_base_hints(package_header_path):
    """Candidate bases read out of the 0xB000 PIRS header, nearest first.

    The XContent SVOD volume descriptor at +0x379 carries a data-block offset
    at +0x1C. On all four titles `2 * <that value as little-endian u24>` lands
    on the true base or two sectors past it:

        Fuzion / Crimson   feature 0x00   value      16   base      32
        Blinx              feature 0x40   value 440,019   base 880,036
        Conker             feature 0x40   value 478,102   base 956,202

    Four samples is not enough to claim the off-by-one on the enhanced-layout
    titles is anything but a unit convention we have not pinned down, so these
    are hints that shorten the search, never an answer. `derive_base()` still
    proves whichever one it takes.
    """
    try:
        with open(package_header_path, "rb") as f:
            f.seek(0x379)
            desc = f.read(0x24)
    except OSError:
        return []
    if len(desc) < 0x1F:
        return []
    seed = 2 * int.from_bytes(desc[0x1C:0x1F], "little")
    return [seed, seed - 2, seed + 2, seed - 4, seed + 4]


# ---- the container --------------------------------------------------------
class SvodImage:
    """The de-interleaved logical stream of a BC package, as a filesystem.

        img = SvodImage("/path/to/BC/Fuzion Frenzy")
        img.verify_hashes()                      # prove the stride
        for path, e in img.walk():
            print(path, e.size)
        img.extract("default.xbe", "default.xbe")

    Accepts the package root, its Content/Game directory, or the
    DefaultPackage.data directory itself.
    """

    def __init__(self, path, base=None):
        self.data_dir = self._find_data_dir(path)
        self.header_path = os.path.join(os.path.dirname(self.data_dir), "DefaultPackage")
        self.paths = [os.path.join(self.data_dir, n)
                      for n in sorted(os.listdir(self.data_dir)) if n.startswith("Data")]
        if not self.paths:
            raise ValueError(f"no DataNNNN files in {self.data_dir}")
        self.sizes = [os.path.getsize(p) for p in self.paths]
        self._handles = {}

        vd = self.read(0, SECTOR)
        if vd[:len(XDVDFS_MAGIC)] != XDVDFS_MAGIC:
            raise ValueError(f"no XDVDFS volume descriptor at logical 0 in {self.data_dir}")
        self.root_sector, self.root_size = struct.unpack_from("<II", vd, 0x14)
        self.base = base if base is not None else derive_base(
            self.read, self.root_sector, self.root_size, self._base_candidates())

    @staticmethod
    def _find_data_dir(path):
        for rel in ("", "Content/Game", "Game"):
            cand = os.path.join(path, rel, "DefaultPackage.data") if rel else \
                os.path.join(path, "DefaultPackage.data")
            if os.path.isdir(cand):
                return cand
        if os.path.isdir(path) and os.path.basename(path).endswith(".data"):
            return path
        raise ValueError(f"no DefaultPackage.data under {path}")

    def _base_candidates(self):
        seen, out = set(), []
        for b in header_base_hints(self.header_path) + [XDVDFS_DEFAULT_BASE]:
            if b >= 0 and b not in seen:
                seen.add(b)
                out.append(b)
        return out

    # ---- raw logical stream ----------------------------------------------
    def _file(self, index):
        if index not in self._handles:
            self._handles[index] = open(self.paths[index], "rb")
        return self._handles[index]

    def read(self, offset, length):
        """Read `length` bytes from the logical (de-interleaved) stream.

        Coalesces whole groups: the 204 data blocks of a group are physically
        contiguous, so a large read is a handful of 835,584-byte reads rather
        than one syscall per 0x1000. Short at end of image.
        """
        out = bytearray()
        while length > 0:
            n, within = divmod(offset, BLOCK)
            file_index, phys = logical_to_physical(n)
            if file_index >= len(self.paths):
                break
            # how far to the end of this group, and of this file
            in_group = n % DATA_BLOCKS_PER_GROUP
            run = (DATA_BLOCKS_PER_GROUP - in_group) * BLOCK - within
            avail = self.sizes[file_index] - (phys + within)
            take = min(length, run, avail)
            if take <= 0:
                break
            f = self._file(file_index)
            f.seek(phys + within)
            chunk = f.read(take)
            if not chunk:
                break
            out += chunk
            offset += len(chunk)
            length -= len(chunk)
            if len(chunk) < take:
                break
        return bytes(out)

    @property
    def logical_size(self):
        """Bytes of logical data, counting the short final Data file."""
        total = 0
        for i, size in enumerate(self.sizes):
            blocks = size // BLOCK
            if blocks >= BLOCKS_PER_FILE:
                total += DATA_BLOCKS_PER_FILE * BLOCK
                continue
            groups, rem = divmod(max(blocks - 1, 0), 1 + DATA_BLOCKS_PER_GROUP)
            total += (groups * DATA_BLOCKS_PER_GROUP + max(rem - 1, 0)) * BLOCK
        return total

    def verify_hashes(self, limit=64):
        """Check `limit` hash blocks of Data0000: sha1 of the block that
        follows each one equals its first 20 bytes. Returns (ok, checked).

        This is the whole justification for the stride constants, and it is
        cheap -- a wrong group size fails on the second hash block.
        """
        f = self._file(0)
        ok = checked = 0
        for h in hash_block_offsets(0):
            if h + 2 * BLOCK > self.sizes[0] or checked >= limit:
                break
            f.seek(h)
            head = f.read(BLOCK)
            body = f.read(BLOCK)
            checked += 1
            ok += hashlib.sha1(body).digest() == head[:20]
        return ok, checked

    # ---- filesystem -------------------------------------------------------
    def read_table(self, entry_or_sector, size=None):
        if isinstance(entry_or_sector, DirEntry):
            sector, size = entry_or_sector.sector, entry_or_sector.size
        else:
            sector = entry_or_sector
        return parse_directory_table(self.read((sector - self.base) * SECTOR, size))

    def root(self):
        return self.read_table(self.root_sector, self.root_size)

    def listdir(self, path=""):
        """Entries of an inner directory. `path` uses '/' and is case-insensitive."""
        entries = self.root()
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            match = next((e for e in entries if e.name.lower() == part.lower()), None)
            if match is None:
                raise FileNotFoundError(path)
            if not match.is_dir:
                raise NotADirectoryError(path)
            entries = self.read_table(match)
        return sorted(entries, key=lambda e: e.name.lower())

    def walk(self, path=""):
        """Yield (inner_path, DirEntry) for every file and directory, depth first."""
        for e in self.listdir(path):
            full = f"{path}/{e.name}" if path else e.name
            yield full, e
            if e.is_dir and e.size:
                yield from self.walk(full)

    def find(self, path):
        parent, _, name = path.replace("\\", "/").rpartition("/")
        for e in self.listdir(parent):
            if e.name.lower() == name.lower():
                return e
        raise FileNotFoundError(path)

    def open_bytes(self, path):
        e = self.find(path)
        data = self.read((e.sector - self.base) * SECTOR, e.size)
        if len(data) != e.size:
            raise ValueError(f"{path}: wanted {e.size} bytes, got {len(data)} "
                             f"-- image truncated or base wrong")
        return data

    def extract(self, path, out_path):
        data = self.open_bytes(path)
        with open(out_path, "wb") as f:
            f.write(data)
        return len(data)


# ---- output validation ----------------------------------------------------
_XBE_KERNEL_THUNK_KEYS = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}


def _xbe_va_to_off(data, va):
    base, header_size = struct.unpack_from("<II", data, 0x104)
    if base <= va < base + header_size:
        return va - base
    count, sec_addr = struct.unpack_from("<II", data, 0x11C)
    for i in range(min(count, 64)):
        off = sec_addr - base + i * 0x38
        if off + 0x14 > len(data):
            break
        _, vaddr, vsize, raw, rsize = struct.unpack_from("<5I", data, off)
        if vaddr <= va < vaddr + vsize:
            return raw + (va - vaddr)
    return None


def xbe_gate(data):
    """Sanity checks that catch a *structurally plausible* bad extraction.

    A flat contiguous copy of the Data files -- hash blocks and all -- still
    reports the right title, build date, base address, section names and XDK
    library version, because all of that lives in the first 0x1000 bytes and
    the first data block is genuine. Three extractions passed a header check
    on 2026-09-19 and were wrong. These are the tells that fired:

      thunks   the kernel import table is a run of 0x8000xxxx ordinals ending
               in 0, and it must actually be at the VA the header declares
               (+0x158, XOR'd with the retail or debug key). Corruption
               displaces it and the run is not there.
      tls      the TLS directory's zero-fill size is a few bytes; a corrupt
               read gives ~1.9 billion, or reads 0x20202020 out of a text
               block that has landed where the directory should be.

    Returns {check: (bool, detail)}. `thunks` is the strong one -- it is the
    check that failed on every bad extraction and passed on every good one.
    """
    out = {}
    out["magic"] = (data[:4] == b"XBEH", repr(data[:4]))
    if len(data) < 0x1000:
        return out
    base, header_size, image_size = struct.unpack_from("<3I", data, 0x104)
    out["header"] = (0x10000 <= base <= 0x80000000 and 0 < header_size <= len(data)
                     and image_size >= header_size,
                     f"base=0x{base:08X} headers=0x{header_size:X} image=0x{image_size:X}")

    raw_thunk = struct.unpack_from("<I", data, 0x158)[0]
    detail, good = "no key gives a thunk run", False
    for kind, key in _XBE_KERNEL_THUNK_KEYS.items():
        va = raw_thunk ^ key
        off = _xbe_va_to_off(data, va)
        if off is None or off + 8 > len(data):
            continue
        n = 0
        while off + 4 * n + 4 <= len(data):
            word = struct.unpack_from("<I", data, off + 4 * n)[0]
            if word == 0:
                break
            if word & 0xFFFF0000 != 0x80000000:
                n = 0
                break
            n += 1
        if n >= 8:
            good, detail = True, f"{n} {kind} ordinals at VA 0x{va:08X}"
            break
        if n:
            detail = f"only {n} ordinals at VA 0x{va:08X} ({kind})"
    out["thunks"] = (good, detail)

    tls_va = struct.unpack_from("<I", data, 0x12C)[0]
    if tls_va == 0:
        out["tls"] = (True, "no TLS directory")
    else:
        off = _xbe_va_to_off(data, tls_va)
        if off is None or off + 0x18 > len(data):
            out["tls"] = (False, f"TLS VA 0x{tls_va:08X} maps nowhere")
        else:
            start, end, _index, _cb, zerofill, _ch = struct.unpack_from("<6I", data, off)
            out["tls"] = (end >= start and zerofill < 0x100000,
                          f"data 0x{start:08X}..0x{end:08X} zerofill={zerofill}")
    return out


# ---- CLI ------------------------------------------------------------------
def _open(argv_path):
    img = SvodImage(argv_path)
    print(f"{img.data_dir}: {len(img.paths)} Data files, "
          f"{img.logical_size / 2**20:,.0f} MB logical, base {img.base:,}, "
          f"root sector {img.root_sector:,} (+0x{img.root_size:x})", file=sys.stderr)
    return img


def cmd_ls(package, path=""):
    img = _open(package)
    for full, e in img.walk(path):
        kind = "d" if e.is_dir else "-"
        print(f"{kind} attr=0x{e.attr:02X} sector={e.sector:>9,} {e.size:>12,}  {full}")


def cmd_extract(package, inner, out):
    img = _open(package)
    n = img.extract(inner, out)
    print(f"wrote {n:,} bytes to {out}")
    with open(out, "rb") as f:
        head = f.read(min(n, 64 << 20))
    if head[:4] == b"XBEH":
        for name, (ok, detail) in xbe_gate(head).items():
            print(f"  {'ok  ' if ok else 'FAIL'} {name:8} {detail}")


def cmd_verify(package):
    img = _open(package)
    ok, checked = img.verify_hashes()
    print(f"hash blocks: {ok}/{checked} verify "
          f"(sha1 of the following block == first 20 bytes)")
    files = sum(1 for _, e in img.walk() if not e.is_dir)
    print(f"directory tree walks: {files:,} files")
    if ok != checked:
        raise SystemExit("hash mismatch -- block stride is wrong for this package")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
    elif sys.argv[1] == "ls":
        cmd_ls(*sys.argv[2:4])
    elif sys.argv[1] == "extract":
        cmd_extract(sys.argv[2], sys.argv[3], sys.argv[4])
    elif sys.argv[1] == "verify":
        cmd_verify(sys.argv[2])
    else:
        print(__doc__)
