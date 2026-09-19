"""The SVOD reader has three pieces of load-bearing logic, and none of them
fail loudly when they are wrong -- a wrong block stride, a wrong sector base or
a lenient directory parser all produce a file that still looks like an XBE.
So each is pinned here against arithmetic or synthetic input:

  - the block mapping, including the 1 + 203*205 == 41,616 identity the stride
    rests on, and that data and hash blocks tile a Data file exactly;
  - the directory-entry parser, against a handcrafted table and against the
    malformations it has to reject for base derivation to work at all;
  - base derivation, against a synthetic image carrying decoy tables of the
    kind Conker's packed directory sectors actually produce.

No BC package is needed or referenced; the repository ships none.
"""
import struct

import pytest

from tools.fusion.svod import (BLOCK, SECTOR, BLOCKS_PER_FILE, DATA_BLOCKS_PER_FILE,
                               DATA_BLOCKS_PER_GROUP, GROUPS_PER_FILE, FULL_FILE_BYTES,
                               derive_base, hash_block_offsets, logical_to_physical,
                               parse_directory_table, xbe_gate)


# ---- block mapping --------------------------------------------------------
def test_block_counts_tile_the_file_exactly():
    # The whole stride rests on this: an L1 block plus 203 groups of
    # (1 hash + 204 data) is the file, with nothing left over.
    assert 1 + GROUPS_PER_FILE * (1 + DATA_BLOCKS_PER_GROUP) == BLOCKS_PER_FILE == 41_616
    assert GROUPS_PER_FILE * DATA_BLOCKS_PER_GROUP == DATA_BLOCKS_PER_FILE == 41_412
    assert FULL_FILE_BYTES == 0xA290000


def test_first_data_block_is_at_0x2000():
    # Block 0 is L1, block 1 is the first group's L0; the volume descriptor
    # therefore lands at physical 0x2000 of Data0000.
    assert logical_to_physical(0) == (0, 0x2000)


def test_group_boundary_skips_the_hash_block():
    # last data block of group 0, then the first of group 1: 2 blocks apart,
    # because an L0 hash block sits between them.
    assert logical_to_physical(203) == (0, (2 + 203) * BLOCK)
    assert logical_to_physical(204) == (0, (2 + 205) * BLOCK)


def test_file_boundary_rolls_over():
    last = DATA_BLOCKS_PER_FILE - 1
    assert logical_to_physical(last) == (0, (BLOCKS_PER_FILE - 1) * BLOCK)
    assert logical_to_physical(DATA_BLOCKS_PER_FILE) == (1, 0x2000)


def test_data_and_hash_blocks_partition_the_file():
    data = {logical_to_physical(n)[1] // BLOCK for n in range(DATA_BLOCKS_PER_FILE)}
    hashes = set(o // BLOCK for o in hash_block_offsets())
    assert len(data) == DATA_BLOCKS_PER_FILE          # no two logical blocks alias
    assert not data & hashes                          # no data block is a hash block
    assert data | hashes == set(range(BLOCKS_PER_FILE))   # and together, no gaps


def test_negative_block_rejected():
    with pytest.raises(ValueError):
        logical_to_physical(-1)


# ---- directory entries ----------------------------------------------------
def _entry(name, sector, size, attr, left=0, right=0):
    raw = struct.pack("<HHIIBB", left, right, sector, size, attr, len(name)) + name.encode()
    return raw + b"\xff" * (-len(raw) % 4)


def _table(rows):
    """Lay entries out as a right-leaning spine; offsets are in 4-byte units."""
    blobs, offsets, pos = [], [], 0
    for name, sector, size, attr in rows:
        offsets.append(pos)
        pos += len(_entry(name, sector, size, attr))
    for i, (name, sector, size, attr) in enumerate(rows):
        nxt = offsets[i + 1] // 4 if i + 1 < len(rows) else 0
        blobs.append(_entry(name, sector, size, attr, right=nxt))
    out = b"".join(blobs)
    return out + b"\xff" * (-len(out) % SECTOR)


def test_parses_a_handcrafted_table():
    tab = _table([("default.xbe", 462518, 3219456, 0x21),
                  ("media", 2027275, 104, 0x10),
                  ("coverart.jpg", 428987, 91590, 0x20)])
    by_name = {e.name: e for e in parse_directory_table(tab)}
    assert set(by_name) == {"default.xbe", "media", "coverart.jpg"}
    assert by_name["default.xbe"].sector == 462518
    assert by_name["default.xbe"].size == 3219456
    assert by_name["media"].is_dir
    # attr is not a file/dir discriminator beyond bit 4: the shipped titles use
    # 0x21, 0x20 and 0x80 for plain files. Only 0x10 means directory.
    assert not by_name["default.xbe"].is_dir and not by_name["coverart.jpg"].is_dir


def test_child_offsets_are_in_four_byte_units():
    # Two entries linked by right=<offset/4>. If the parser read the offset as
    # bytes it would land mid-entry and see garbage.
    tab = _table([("a", 1, 1, 0x20), ("bb", 2, 2, 0x20)])
    assert struct.unpack_from("<H", tab, 2)[0] * 4 == 16
    assert {e.name for e in parse_directory_table(tab)} == {"a", "bb"}


def test_rejects_name_running_past_the_table():
    tab = bytearray(_table([("default.xbe", 1, 1, 0x20)])[:16])
    with pytest.raises(ValueError):
        parse_directory_table(bytes(tab))


def test_rejects_non_printable_name():
    tab = bytearray(_table([("default.xbe", 1, 1, 0x20)]))
    tab[14] = 0x00
    with pytest.raises(ValueError):
        parse_directory_table(bytes(tab))


def test_rejects_zero_length_name():
    tab = bytearray(_table([("default.xbe", 1, 1, 0x20)]))
    tab[13] = 0
    with pytest.raises(ValueError):
        parse_directory_table(bytes(tab))


def test_rejects_cycle():
    # A node whose right child is itself. 0 means "no child", so a loop can
    # only be built by pointing at an offset already on the walk -- which is
    # exactly what a table read at the wrong sector produces by accident.
    a = _entry("a", 1, 1, 0x20, right=4)        # -> offset 16
    b = bytearray(_entry("bb", 2, 2, 0x20))
    struct.pack_into("<H", b, 2, 4)             # -> offset 16, itself
    with pytest.raises(ValueError):
        parse_directory_table(a + bytes(b))


# ---- base derivation ------------------------------------------------------
class _Synthetic:
    """A logical stream with a root table, decoys, and an XBE, at chosen sectors."""

    def __init__(self, sectors):
        size = (max(sectors) + 4) * SECTOR
        self.buf = bytearray(size)
        for sector, payload in sectors.items():
            self.buf[sector * SECTOR:sector * SECTOR + len(payload)] = payload

    def read(self, offset, length):
        if offset < 0:
            return b""
        return bytes(self.buf[offset:offset + length])


def _image():
    # true base 1000, so root sector 1005 is logical sector 5 and the XBE at
    # sector 1010 is logical sector 10.
    return _Synthetic({
        5: _table([("default.xbe", 1010, 4, 0x21), ("media", 1020, 0x800, 0x10)]),
        # decoy that parses but names no default.xbe -- chosen if base were 999
        6: _table([("Sound.xsb", 1030, 16, 0x80)]),
        # decoy that names default.xbe but resolves it to non-XBE bytes under
        # base 998 (sector 1010 -> logical 12, which is empty)
        7: _table([("default.xbe", 1010, 4, 0x21)]),
        10: b"XBEH" + b"\0" * 16,
    })


ROOT_SECTOR, ROOT_SIZE = 1005, SECTOR


def test_derives_the_true_base_over_parsing_decoys():
    img = _image()
    assert derive_base(img.read, ROOT_SECTOR, ROOT_SIZE, [999, 998, 1000]) == 1000


def test_a_decoy_that_merely_parses_is_not_enough():
    # base 999 alone: the table at logical 6 parses cleanly and would be
    # accepted by a parse-only check.
    img = _image()
    assert parse_directory_table(img.read((ROOT_SECTOR - 999) * SECTOR, ROOT_SIZE))
    with pytest.raises(ValueError):
        derive_base(img.read, ROOT_SECTOR, ROOT_SIZE, [999])


def test_a_decoy_naming_default_xbe_is_rejected_without_the_magic():
    img = _image()
    with pytest.raises(ValueError):
        derive_base(img.read, ROOT_SECTOR, ROOT_SIZE, [998])


def test_base_above_root_sector_is_skipped_not_crashed():
    img = _image()
    assert derive_base(img.read, ROOT_SECTOR, ROOT_SIZE, [2000, 1000]) == 1000


def test_no_candidate_raises():
    img = _image()
    with pytest.raises(ValueError):
        derive_base(img.read, ROOT_SECTOR, ROOT_SIZE, [32])


# ---- output validation ----------------------------------------------------
def _xbe(thunk_words, tls_zerofill=12):
    """A minimal XBE whose thunk table and TLS directory live in the header."""
    base, header_size = 0x10000, 0x1000
    buf = bytearray(header_size)
    buf[0:4] = b"XBEH"
    struct.pack_into("<3I", buf, 0x104, base, header_size, 0x20000)
    struct.pack_into("<II", buf, 0x11C, 0, base + 0x120)      # no sections
    struct.pack_into("<I", buf, 0x12C, base + 0x900)          # TLS directory VA
    struct.pack_into("<I", buf, 0x158, (base + 0x800) ^ 0x5B6D40B6)   # retail key
    for i, w in enumerate(thunk_words):
        struct.pack_into("<I", buf, 0x800 + 4 * i, w)
    struct.pack_into("<6I", buf, 0x900, base, base, 0, 0, tls_zerofill, 0)
    return bytes(buf)


def test_gate_passes_a_well_formed_thunk_run():
    g = xbe_gate(_xbe([0x80000000 | n for n in range(1, 40)] + [0]))
    assert all(ok for ok, _ in g.values()), g


def test_gate_fails_when_the_thunk_run_is_not_at_the_declared_va():
    # what a displaced extraction looks like: the VA in the header is right,
    # the ordinals are not there.
    g = xbe_gate(_xbe([0x41424344] * 40))
    assert g["magic"][0] and g["header"][0]      # still "looks like" an XBE
    assert not g["thunks"][0]


def test_gate_fails_a_wild_tls_zero_fill():
    # the observed corrupt value was ~1.9 billion
    g = xbe_gate(_xbe([0x80000000 | n for n in range(1, 40)] + [0],
                      tls_zerofill=1_986_098_990))
    assert g["thunks"][0] and not g["tls"][0]


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for f in fns:
        f()
        print("ok", f.__name__)
    print(f"{len(fns)} passed")
