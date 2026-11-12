"""Read a store trace (--trace-stores; src/kernel/recomp_store_trace.c).

    python -m tools.recomp.store_trace FILE                  writers summary
    python -m tools.recomp.store_trace FILE --addr 0x00100000+16
    python -m tools.recomp.store_trace FILE --rows [--csv]   every row

The summary has one line per writer (function and store va): how many rows,
the sizes, the addresses it touched, the flips it wrote in and the values
it left. Rows are in `seq` order, the order the stores happened in.
"""

import argparse
import csv
import struct
import sys
from collections import namedtuple

MAGIC = b"XBRSTRC1"
ROW = struct.Struct("<5IHBBQ")
KINDS = {0: "store", 1: "string", 2: "enter", 3: "exit", 4: "owned"}

Row = namedtuple("Row", "va addr func frame seq size kind thread value")


def parse_ranges(spec):
    """'LO+LEN' / 'LO-HI' / 'ADDR', comma-separated, as [lo, hi) pairs."""
    out = []
    for part in filter(None, (p.strip() for p in spec.replace(";", ",").split(","))):
        if "+" in part:
            lo, n = part.split("+", 1)
            lo = int(lo, 0)
            out.append((lo, lo + int(n, 0)))
        elif "-" in part:
            lo, hi = part.split("-", 1)
            out.append((int(lo, 0), int(hi, 0)))
        else:
            lo = int(part, 0)
            out.append((lo, lo + 1))
    return out


def read(path):
    """Every row of the file, in seq order."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != MAGIC:
        raise ValueError(f"{path}: not a store trace")
    row_size, version = struct.unpack_from("<II", data, 8)
    if row_size != ROW.size or version != 1:
        raise ValueError(f"{path}: row size {row_size} version {version}, "
                         f"this reader knows {ROW.size}/1")
    body = data[16:]
    whole = len(body) - len(body) % ROW.size
    rows = [Row(*t) for t in ROW.iter_unpack(body[:whole])]
    rows.sort(key=lambda r: r.seq)
    return rows


def overlaps(row, ranges):
    lo, hi = row.addr, row.addr + row.size
    return any(lo < b and hi > a for a, b in ranges)


def value_hex(row):
    n = min(row.size, 8)
    return f"{row.value & ((1 << (8 * n)) - 1):0{2 * n}X}" if n else ""


def writers(rows):
    """{(func, va): summary dict}, by first appearance."""
    out = {}
    for r in rows:
        w = out.get((r.func, r.va))
        if w is None:
            w = out[(r.func, r.va)] = {
                "func": r.func, "va": r.va, "count": 0, "sizes": set(),
                "addrs": set(), "kinds": set(), "threads": set(),
                "first": r.frame, "last": r.frame, "values": set(),
            }
        w["count"] += 1
        w["sizes"].add(r.size)
        w["addrs"].add(r.addr)
        w["kinds"].add(KINDS.get(r.kind, str(r.kind)))
        w["threads"].add(r.thread)
        w["first"] = min(w["first"], r.frame)
        w["last"] = max(w["last"], r.frame)
        if len(w["values"]) < 64:
            w["values"].add(value_hex(r))
    return out


def _fmt_set(s, fmt="{:X}", limit=4):
    items = sorted(s)
    text = ",".join(fmt.format(x) for x in items[:limit])
    return text + (f",+{len(items) - limit}" if len(items) > limit else "")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--addr", help="only rows overlapping these ranges")
    ap.add_argument("--func", help="only rows from these functions (hex, comma)")
    ap.add_argument("--frames", help="only rows in flips A-B")
    ap.add_argument("--rows", action="store_true", help="every row, not the summary")
    ap.add_argument("--csv", action="store_true", help="rows as CSV")
    args = ap.parse_args(argv)

    rows = read(args.trace)
    if args.addr:
        ranges = parse_ranges(args.addr)
        rows = [r for r in rows if overlaps(r, ranges)]
    if args.func:
        funcs = {int(x, 16) for x in args.func.split(",") if x}
        rows = [r for r in rows if r.func in funcs]
    if args.frames:
        a, _, b = args.frames.partition("-")
        lo, hi = int(a, 0), int(b, 0) if b else int(a, 0)
        rows = [r for r in rows if lo <= r.frame <= hi]

    if args.rows:
        if args.csv:
            w = csv.writer(sys.stdout)
            w.writerow(["seq", "frame", "thread", "func", "va", "addr",
                        "size", "kind", "value"])
            for r in rows:
                w.writerow([r.seq, r.frame, r.thread, f"0x{r.func:08X}",
                            f"0x{r.va:08X}", f"0x{r.addr:08X}", r.size,
                            KINDS.get(r.kind, r.kind), value_hex(r)])
        else:
            for r in rows:
                print(f"{r.seq:10d} f{r.frame:<7d} t{r.thread:<2d} "
                      f"sub_{r.func:08X} @{r.va:08X} [{r.addr:08X}] "
                      f"{r.size:3d} {KINDS.get(r.kind, r.kind):6s} {value_hex(r)}")
        return 0

    ws = writers(rows)
    print(f"{len(rows)} rows, {len(ws)} writers "
          f"in {len({w['func'] for w in ws.values()})} functions")
    print(f"{'function':12s} {'store va':10s} {'rows':>8s} {'flips':>13s} "
          f"{'size':6s} {'thr':4s} addresses / values")
    for w in sorted(ws.values(), key=lambda w: (w["func"], w["va"])):
        print(f"sub_{w['func']:08X} {w['va']:08X}   {w['count']:8d} "
              f"{w['first']:6d}-{w['last']:<6d} "
              f"{_fmt_set(w['sizes'], '{}'):6s} {_fmt_set(w['threads'], '{}'):4s} "
              f"{_fmt_set(w['addrs'])} = {_fmt_set(w['values'], '{}', 3)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
