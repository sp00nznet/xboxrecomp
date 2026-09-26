"""Per-site indirect-call records merge cumulatively and keep saturation."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.icall_feedback import (  # noqa: E402
    load_sites, merge_sites, parse_sites_dump, sites_dump_beside)


def _write(tmp, name, body):
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        f.write(body)
    return p


def test_dump_parses_targets_and_saturation_and_tolerates_truncation():
    with tempfile.TemporaryDirectory() as tmp:
        p = _write(tmp, "icall_sites.dump",
                   "# icall-sites v1\n0003C349 00011C80 00012000\n"
                   "0003C371 00011C80 +\n0003C3")
        d = parse_sites_dump(p)
        assert d[0x3C349] == ({0x11C80, 0x12000}, False)
        assert d[0x3C371] == ({0x11C80}, True)
        assert len(d) == 2


def test_merge_is_cumulative_and_saturation_sticks():
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "out", "icall_sites.json")
        one = _write(tmp, "a_sites.dump", "0003C349 00011C80\n0003C371 00011C80 +\n")
        two = _write(tmp, "b_sites.dump", "0003C349 00012000\n0003C371 00011C80\n")
        merge_sites(db, [one])
        merge_sites(db, [two])
        got = load_sites(db)
        assert got[0x3C349] == ({0x11C80, 0x12000}, False)
        assert got[0x3C371] == ({0x11C80}, True)


def test_sites_dump_sits_beside_the_targets_dump():
    assert sites_dump_beside("/x/icall_targets.dump") == "/x/icall_sites.dump"
    assert sites_dump_beside("/x/odd.dump") == "/x/odd.dump.sites"
