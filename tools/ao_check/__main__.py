"""
tools/ao_check/__main__.py

Count the frames scene AO missed in a run traced with ao_frames: the
`ao: flip N ... ran= ord= want= at= ... fb=` lines both backends log.

A frame is missed when it had depth events (ord > 0), ran no pass (ran=0),
fired at no event (at=0) and took no flip fallback (fb=0). A frame with no usable depth fit (ok=0:
menus, a fit the trigger refuses) never fires by design, and the trigger
does not count it as missed either, so it is skipped. Scene starts
are not counted: a frame with want=0 (no memo yet, after a load) starts a
warm-up of WARMUP frames, as the trigger learns the scene's event counts
in that time (the Deck's Stage 1 missed 74 frames in steady
play while the PC missed none).

Usage:

    python3 -m tools.ao_check game.log [--warmup 8]

Exits 1 when any steady frame was missed, listing the first ones.
"""

import argparse
import re
import sys

LINE = re.compile(
    r"ao: flip (\d+) \S+ ran=(\d+) ord=(\d+) want=(\d+) at=(\d+) ok=(\d+) .*?fb=(\d+)"
)
WARMUP = 8


def misses(lines, warmup=WARMUP):
    """Return (steady frames, missed flip numbers) for the ao: lines."""
    steady, missed, warm = 0, [], 0
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        flip, ran, ord_, want, at, ok, fb = (int(g) for g in m.groups())
        if not want:
            warm = 0
            continue
        warm += 1
        if warm <= warmup:
            continue
        steady += 1
        if ord_ and ok and not ran and not at and not fb:
            missed.append(flip)
    return steady, missed


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("log")
    ap.add_argument("--warmup", type=int, default=WARMUP)
    a = ap.parse_args(argv)
    with open(a.log, errors="replace") as f:
        steady, missed = misses(f, a.warmup)
    print(f"ao: {steady} steady frames, {len(missed)} missed")
    if missed:
        print("  first: " + " ".join(str(x) for x in missed[:20]))
    if not steady:
        print("  no ao: lines past the warm-up (ao_frames trace off?)")
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
