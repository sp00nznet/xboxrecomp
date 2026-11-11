"""
tools/abi_analysis/__main__.py

Recovers calling-convention / parameter-count / return-type / frame-shape
info for every recompiled function, so tools.recomp can generate more
accurate C signatures instead of falling back to its cdecl/0-param/int-or-void
defaults for everything.

Usage (matches the other pipeline tools' style):

    py -3 -m tools.abi_analysis game_files/default.xbe

    py -3 -m tools.abi_analysis game_files/default.xbe \
        --functions tools/disasm/output/functions.json \
        --identified tools/func_id/output/identified_functions.json \
        --output-dir tools/abi_analysis/output \
        -v

Writes tools/abi_analysis/output/abi_functions.json by default, which is
exactly where tools.recomp looks for it (see tools/recomp/__main__.py's
--abi-dir default).
"""

import argparse
import json
import os
import sys

from .analyzer import AbiAnalyzer
from .xbe_min import XbeFile


def find_data_files(disasm_dir=None, func_id_dir=None, overrides=None):
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    disasm_dir = disasm_dir or os.path.join(base, "disasm", "output")
    func_id_dir = func_id_dir or os.path.join(base, "func_id", "output")
    paths = {
        "functions": os.path.join(disasm_dir, "functions.json"),
        "identified": os.path.join(func_id_dir, "identified_functions.json"),
    }
    overrides = overrides or {}
    for key, val in overrides.items():
        if val:
            paths[key] = val
    return paths


def _load_json_list(path, label):
    if not os.path.exists(path):
        print(f"ERROR: {label} not found at {path}", file=sys.stderr)
        sys.exit(1)
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict):
        # Tolerate either a bare list or a {"functions": [...]} wrapper.
        for key in ("functions", "items", "entries"):
            if key in data and isinstance(data[key], list):
                return data[key]
        # Fall back to treating dict values as the entries.
        return list(data.values())
    return data


def main():
    parser = argparse.ArgumentParser(
        prog="python -m tools.abi_analysis",
        description="Heuristic ABI recovery (calling convention, param "
                     "count, return type, frame shape) for recompiled "
                     "Xbox functions.")
    parser.add_argument("xbe_path", help="Path to default.xbe")
    parser.add_argument("--disasm-dir",
                         help="Directory holding tools.disasm output "
                              "(default: tools/disasm/output)")
    parser.add_argument("--func-id-dir",
                         help="Directory holding tools.func_id output "
                              "(default: tools/func_id/output)")
    parser.add_argument("--functions",
                         help="Path to functions.json (overrides --disasm-dir)")
    parser.add_argument("--identified",
                         help="Path to identified_functions.json "
                              "(overrides --func-id-dir)")
    parser.add_argument("--output-dir",
                         help="Output directory (default: "
                              "tools/abi_analysis/output)")
    parser.add_argument("--output",
                         help="Full output path (overrides --output-dir); "
                              "default: <output-dir>/abi_functions.json")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(args.xbe_path):
        print(f"ERROR: XBE not found at {args.xbe_path}", file=sys.stderr)
        sys.exit(1)

    paths = find_data_files(
        disasm_dir=args.disasm_dir,
        func_id_dir=args.func_id_dir,
        overrides={"functions": args.functions, "identified": args.identified},
    )

    functions = _load_json_list(paths["functions"], "functions.json")
    identified = _load_json_list(paths["identified"], "identified_functions.json")

    identified_by_addr = {}
    for entry in identified:
        addr = entry.get("start")
        if addr is None:
            continue
        addr = int(addr, 16) if isinstance(addr, str) else int(addr)
        identified_by_addr[addr] = entry

    if args.verbose:
        print(f"Loaded {len(functions)} functions, "
              f"{len(identified_by_addr)} classifications")
        print(f"Loading XBE from {args.xbe_path} ...")

    xbe = XbeFile(args.xbe_path)

    if args.verbose:
        print(f"XBE base=0x{xbe.base_address:08X}, "
              f"{len(xbe.sections)} sections")
        for s in xbe.sections:
            print(f"  {s}")

    analyzer = AbiAnalyzer(xbe, functions, identified_by_addr,
                            verbose=args.verbose)
    results = analyzer.analyze_all()

    output_dir = args.output_dir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "output")
    output_path = args.output or os.path.join(output_dir, "abi_functions.json")
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)

    thiscall_count = sum(1 for r in results
                          if r["calling_convention"].startswith("thiscall"))
    print(f"Wrote {len(results)} ABI entries to {output_path} "
          f"({thiscall_count} detected as thiscall)")


if __name__ == "__main__":
    main()
