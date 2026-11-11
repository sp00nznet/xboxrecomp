"""
Self-check for the XDVDFS reader.

Run: py -3 tools/xiso/test_xdvdfs.py

Builds a synthetic disc image in memory rather than depending on a real one, so
this runs anywhere. The properties worth pinning:

  - the partition base is found by probing for the magic, not guessed from file
    size, because images come as bare game partitions and with a video partition
    in front;
  - directory entries are walked per-sector rather than by following the binary
    tree's left/right links, so a padded or malformed link cannot silently drop
    a whole subtree. Losing a file quietly is worse than being slow;
  - a non-XDVDFS file fails loudly instead of returning an empty listing, which
    would otherwise read as "this disc has no files on it".
"""

import io
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.xiso.xdvdfs import Xiso, XisoError, SECTOR, MAGIC  # noqa: E402

ATTR_DIR = 0x10


def _entry(name, sector, size, attr=0):
    nb = name.encode("ascii")
    e = struct.pack("<HHIIBB", 0, 0, sector, size, attr, len(nb)) + nb
    return e + b"\x00" * ((-len(e)) % 4)


def _build_image(base=0, files=(("default.xbe", b"XBEH-payload"),),
                 subdir=None):
    """Lay out: descriptor at sector 32, root dir at 33, file data from 34."""
    root_sector = 33
    data_sector = 34
    root = b""
    blobs = {}
    sec = data_sector
    for name, payload in files:
        root += _entry(name, sec, len(payload))
        blobs[sec] = payload
        sec += max(1, (len(payload) + SECTOR - 1) // SECTOR)
    sub_entries = b""
    if subdir:
        subname, subfiles = subdir
        sub_sector = sec
        sec += 1
        for name, payload in subfiles:
            sub_entries += _entry(name, sec, len(payload))
            blobs[sec] = payload
            sec += max(1, (len(payload) + SECTOR - 1) // SECTOR)
        root += _entry(subname, sub_sector, SECTOR, ATTR_DIR)
        blobs[sub_sector] = sub_entries

    total = sec + 2
    img = bytearray(base + total * SECTOR)
    desc = bytearray(SECTOR)
    desc[0:len(MAGIC)] = MAGIC
    struct.pack_into("<II", desc, 0x14, root_sector, SECTOR)
    img[base + 32 * SECTOR: base + 33 * SECTOR] = desc
    img[base + root_sector * SECTOR: base + root_sector * SECTOR + len(root)] = root
    for s, payload in blobs.items():
        img[base + s * SECTOR: base + s * SECTOR + len(payload)] = payload
    return bytes(img)


def _write(tmp, data, name="t.iso"):
    p = os.path.join(tmp, name)
    with open(p, "wb") as f:
        f.write(data)
    return p


def test_reads_a_bare_game_partition():
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, _build_image(base=0))
        with Xiso(p) as iso:
            assert iso.base == 0, hex(iso.base)
            e = iso.find("default.xbe")
            assert e is not None and e.size == len(b"XBEH-payload")
            assert iso.read(e) == b"XBEH-payload"


def test_finds_a_shifted_partition_base():
    # Redump-style: video partition first. Must be located by probing, not by
    # assuming offset 0.
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, _build_image(base=0x30600))
        with Xiso(p) as iso:
            assert iso.base == 0x30600, hex(iso.base)
            assert iso.find("default.xbe") is not None


def test_lookup_is_case_insensitive():
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, _build_image())
        with Xiso(p) as iso:
            assert iso.find("DEFAULT.XBE") is not None
            assert iso.find("Default.Xbe") is not None
            assert iso.find("nope.xbe") is None


def test_walks_subdirectories():
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, _build_image(
            files=(("default.xbe", b"A"),),
            subdir=("media", (("intro.bik", b"BIK1"), ("logo.bik", b"BIK2")))))
        with Xiso(p) as iso:
            names = sorted((d + "/" + e.name).lstrip("/") for d, e in iso.walk())
        assert names == ["default.xbe", "media/intro.bik", "media/logo.bik"], names


def test_extract_writes_exact_bytes():
    payload = bytes(range(256)) * 12   # spans more than one sector
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, _build_image(files=(("default.xbe", payload),)))
        out = os.path.join(tmp, "out", "default.xbe")
        with Xiso(p) as iso:
            iso.extract(iso.find("default.xbe"), out)
        assert open(out, "rb").read() == payload


def test_non_xdvdfs_fails_loudly():
    # An empty listing would read as "this disc has no files", which is worse.
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, b"not an xbox disc" * 4096)
        try:
            Xiso(p)
        except XisoError as e:
            assert "XDVDFS" in str(e), str(e)
        else:
            raise AssertionError("expected XisoError")


def test_unpack_refuses_names_that_escape_the_output_dir():
    # Names come from the disc, so an entry like '../x' must not write outside
    # the output directory. Checked on every host: '../' escapes everywhere.
    from tools.xiso.__main__ import main as xiso_main
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, _build_image(files=(
            ("default.xbe", b"XBEH"), ("../escape.bin", b"X"),
            ("a\\..\\b.bin", b"Y"))))
        out = os.path.join(tmp, "out")
        assert xiso_main(["unpack", p, "-o", out]) == 0
        with open(os.path.join(out, "default.xbe"), "rb") as f:
            assert f.read() == b"XBEH"
        assert not os.path.exists(os.path.join(tmp, "escape.bin"))
        assert not os.path.exists(os.path.join(tmp, "b.bin"))


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
