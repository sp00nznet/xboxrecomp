"""
Self-check for the indirect-branch target feedback loop.

Run: py -3 tools/recomp/test_icall_feedback.py

The properties that matter:
  - a truncated dump still parses (it is written from a dying process)
  - the database is CUMULATIVE across runs; a target seen once is never lost
    because a later run did not reach it. Without that the loop cannot
    converge, it just reports whatever the last run happened to touch.
  - flags accumulate rather than overwrite: a target that was unresolved in
    run 1 and resolved in run 2 must end up as both, so the history is
    visible instead of being silently repaired.
  - the database is directly loadable by tools/disasm --seed-functions, with
    no conversion step.
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.icall_feedback import (  # noqa: E402
    SEEN_RESOLVED, SEEN_UNRESOLVED, _interior_of, load_db, main,
    parse_dump, save_db)
from tools.disasm.__main__ import _load_seed_functions  # noqa: E402


def _dump(tmp, name, body):
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        f.write(body)
    return p


def test_parse_dump_reads_va_and_flags():
    with tempfile.TemporaryDirectory() as tmp:
        p = _dump(tmp, "a.txt",
                  "# icall-feedback v1\n"
                  "# va flags\n"
                  "0011AABB 1\n"
                  "0011CCDD 2\n"
                  "0011EEFF 3\n")
        d = parse_dump(p)
    assert d == {0x0011AABB: 1, 0x0011CCDD: 2, 0x0011EEFF: 3}, d


def test_truncated_dump_still_parses():
    # Process died mid-line. Everything before the tear must survive.
    with tempfile.TemporaryDirectory() as tmp:
        p = _dump(tmp, "t.txt", "# icall-feedback v1\n0011AABB 1\n0011CC")
        d = parse_dump(p)
    assert d == {0x0011AABB: 1}, d


def test_database_is_cumulative_across_runs():
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "out", "targets.json")
        r1 = _dump(tmp, "r1.txt", "0011AAAA 1\n0011BBBB 2\n")
        # Second run reaches a different path and never touches 0011BBBB.
        r2 = _dump(tmp, "r2.txt", "0011AAAA 1\n0011CCCC 1\n")
        assert main(["--db", db, "merge", r1]) == 0
        assert main(["--db", db, "merge", r2]) == 0
        got = load_db(db)
    assert set(got) == {0x0011AAAA, 0x0011BBBB, 0x0011CCCC}, got
    assert got[0x0011BBBB] == SEEN_UNRESOLVED, "run 2 dropped a run 1 target"


def test_flags_accumulate_rather_than_overwrite():
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "out", "targets.json")
        # Unresolved first, resolved later: the history must remain visible.
        assert main(["--db", db, "merge",
                     _dump(tmp, "r1.txt", "00120000 2\n")]) == 0
        assert main(["--db", db, "merge",
                     _dump(tmp, "r2.txt", "00120000 1\n")]) == 0
        got = load_db(db)
    assert got[0x00120000] == (SEEN_RESOLVED | SEEN_UNRESOLVED), got


def test_database_is_directly_loadable_as_seed_functions():
    # The whole point of writing the DB in seed format: no conversion step.
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "out", "targets.json")
        save_db(db, {0x0011AABB: 1, 0x00120000: 2})
        seeds = _load_seed_functions(db)
        raw = json.load(open(db))
    assert sorted(seeds) == [0x0011AABB, 0x00120000], seeds
    # and the extra bookkeeping keys are present but harmless
    assert raw[0]["seen"] == "resolved", raw[0]
    assert raw[1]["seen"] == "unresolved", raw[1]


def test_seeds_drops_unaligned_targets():
    """Measured on Halo 2276: seeding the raw observed set made the title crash
    earlier than before (segfault before the render_cameras.c:458 assert it used
    to reach). The 4 offenders were the only unaligned ones. Filtering to
    16-aligned restored the original milestone, and the next run confirmed those
    same 4 were still the only unresolved targets -- i.e. they are not function
    starts, they are garbage that happens to land in .text."""
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "targets.json")
        out = os.path.join(tmp, "seeds.json")
        save_db(db, {0x001B5540: 2, 0x001D99BA: 2, 0x0024B5FB: 2, 0x00130EC0: 2})
        assert main(["--db", db, "--functions", os.path.join(tmp, "none.json"),
                     "seeds", "--out", out, "--align", "16"]) == 0
        kept = load_db(out)
    assert sorted(kept) == [0x00130EC0, 0x001B5540], [hex(v) for v in kept]


def test_seeds_align_zero_keeps_everything():
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "targets.json")
        out = os.path.join(tmp, "seeds.json")
        save_db(db, {0x001B5540: 2, 0x001D99BA: 2})
        assert main(["--db", db, "--functions", os.path.join(tmp, "none.json"),
                     "seeds", "--out", out, "--align", "0"]) == 0
        assert len(load_db(out)) == 2


def test_seeds_are_not_filtered_against_current_functions_json():
    """Seeds are an INPUT to the pass that rewrites functions.json from scratch.
    Dropping 'already known' targets is circular: next run they are only known
    because they were seeded, and an indirect-only target cannot be re-derived.
    A seed file must be a standalone, idempotent statement of what to seed."""
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "targets.json")
        out = os.path.join(tmp, "seeds.json")
        fns = os.path.join(tmp, "functions.json")
        save_db(db, {0x00130EC0: 1, 0x001B5540: 2})
        with open(fns, "w") as f:
            json.dump([{"start": "0x00130EC0", "end": "0x00130F00"}], f)
        assert main(["--db", db, "--functions", fns,
                     "seeds", "--out", out]) == 0
        kept = load_db(out)
    assert sorted(kept) == [0x00130EC0, 0x001B5540], [hex(v) for v in kept]


def test_zero_flag_entries_are_not_recorded():
    # A zero byte means "never observed"; it must not become a seed.
    with tempfile.TemporaryDirectory() as tmp:
        p = _dump(tmp, "z.txt", "0011AABB 0\n0011CCDD 1\n")
        d = parse_dump(p)
    assert d == {0x0011CCDD: 1}, d


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


def test_a_seed_inside_a_function_body_is_never_emitted():
    """That seed truncates the function containing it.

    This is what cost the Xbox Dashboard its boot: an address at a valid
    instruction boundary inside __heap_init clamped that function's end, so it
    returned without its `pop esi; ret`. The caller came back with its
    callee-saved registers rotated, the CRT heap was never initialised, and
    nothing was logged anywhere. Decoding cleanly does not catch it -- an
    interior address is mid-function, so of course it decodes.
    """
    bodies = [(0x1000, 0x1100), (0x2000, 0x2010), (0x3000, 0x3400)]

    assert _interior_of(0x1080, bodies) == 0x1000
    assert _interior_of(0x33FF, bodies) == 0x3000
    # A start is the function itself, not an interior address -- and it must
    # stay seedable, or the loop stops being idempotent across runs.
    assert _interior_of(0x1000, bodies) is None
    # One past the end belongs to whatever comes next.
    assert _interior_of(0x1100, bodies) is None
    # Gaps are where the seeds worth keeping live.
    assert _interior_of(0x1800, bodies) is None
    assert _interior_of(0x0500, bodies) is None


if __name__ == "__main__":
    _run()

