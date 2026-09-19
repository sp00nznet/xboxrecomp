"""Coverage oracle: grade our function-boundary detection against Microsoft's.

Microsoft's Fission PrecompiledSymbolTable gives exact (guest_start, size) for
the library functions in a title -- ground-truth function boundaries. For a
title we also recompile (Crimson Skies), compare our tools/disasm functions.json
against those starts:

  hit      MS names a function start and we detect a function there
  missed   MS names a start we don't detect at all (a real function boundary we
           lack) -- split by whether it lands INSIDE a function we did detect
           (a merge/under-segmentation error) or in no detected function (a gap)

The missed-inside set is the actionable one: those are the tail-jump /
mid-function-entry cases our detector still merges. Run it after detector changes
to measure movement against ground truth.

    py -3 -m tools.fusion.coverage_oracle <module.dll> <our_functions.json>

`our_functions.json` is our detection for the *donor* title, so it comes from
running the pipeline over that title's original Xbox default.xbe. Get one out
of the package with tools/fusion/svod.py -- the SVOD container is not sealed:

    py -3 -m tools.fusion.svod extract "<package>" default.xbe donor.xbe
    py -3 -m tools.disasm donor.xbe --text-only
"""
import bisect
import json
import sys

from tools.fusion.module import FusionModule


def load_our_functions(path):
    d = json.load(open(path, encoding="utf-8"))
    fs = d["functions"] if isinstance(d, dict) and "functions" in d else d
    it = fs.values() if isinstance(fs, dict) else fs
    spans = []
    for f in it:
        st = f["start"]; va = int(st, 16) if isinstance(st, str) else st
        end = None
        if "end" in f:
            end = f["end"]; end = int(end, 16) if isinstance(end, str) else end
        elif "size" in f and f["size"]:
            sz = f["size"] if isinstance(f["size"], int) else int(str(f["size"]), 0)
            end = va + sz
        spans.append((va, end if end else va + 1))
    spans.sort()
    return spans


def main(module_path, funcs_path):
    m = FusionModule(module_path)
    ours = load_our_functions(funcs_path)
    starts = [s for s, _ in ours]
    our_start_set = set(starts)

    # unique MS function starts (dedupe the repeated small funcs)
    ms_starts = sorted({s.guest_start for s in m.symbols})

    hit = missed_inside = missed_gap = 0
    inside_examples = []
    ms_name = {}
    for s in m.symbols:
        ms_name.setdefault(s.guest_start, s.name)

    for a in ms_starts:
        if a in our_start_set:
            hit += 1
            continue
        # which of our functions contains a, if any?
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and ours[i][0] <= a < ours[i][1]:
            missed_inside += 1
            if len(inside_examples) < 15:
                inside_examples.append((a, ms_name[a], ours[i][0]))
        else:
            missed_gap += 1

    total = len(ms_starts)
    print(f"module: {m.source} [{m.build_tree}]  MS-named function starts: {total:,}")
    print(f"  hit (we detect a start there):      {hit:,}  ({100.0*hit/total:.1f}%)")
    print(f"  missed, inside a detected function: {missed_inside:,}  <- under-segmentation")
    print(f"  missed, in no detected function:    {missed_gap:,}  <- uncovered")
    print("\n  sample missed-inside (MS start : our containing function start):")
    for a, nm, host in inside_examples:
        print(f"    0x{a:08X} {nm:40} inside 0x{host:08X}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
