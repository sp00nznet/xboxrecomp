"""Inventory basic blocks for a flat guest-VA dispatch experiment.

This does not change production code generation.  It measures the shape and
memory cost of the block-dispatch architecture described in the Ficl/Fission
teardown and emits a machine-readable manifest that can be consumed by an
experimental backend.

Usage:

    py -3 -m tools.recomp.block_dispatch default.xbe \
        --functions tools/disasm/output/functions.json \
        --output block_dispatch.json

The manifest contains one entry per recovered basic block, its owning function,
end address, successors, and a dense slot index based on guest virtual address.
"""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import asdict, dataclass

from . import config
from .translator import FunctionTranslator


@dataclass(frozen=True)
class BlockRecord:
    start: int
    end: int
    owner: int
    successors: tuple[int, ...]
    instructions: int


def _parse_addr(value):
    if isinstance(value, str):
        return int(value, 0)
    return int(value)


def _normalize_functions(raw):
    """Normalize addresses while retaining evidence used for CFG ownership."""
    out = {}
    items = raw.items() if isinstance(raw, dict) else ((0, item) for item in raw)
    for key, item in items:
        if not isinstance(item, dict):
            continue
        if not any(field in item for field in ("start", "address", "_addr")):
            try:
                key = _parse_addr(key)
            except (TypeError, ValueError):
                continue
        start = _parse_addr(item.get("start", item.get("address", item.get("_addr", key))))
        if "end" in item:
            end = _parse_addr(item["end"])
        else:
            # Some pipeline outputs describe a function as {address, size}
            # rather than {start, end}.  Treat both shapes equivalently.
            end = start + _parse_addr(item.get("size", 0))
        if start and end > start:
            out[start] = {**item, "_addr": start, "start": start, "end": end}
    return dict(sorted(out.items()))


def _dedupe_records(records):
    """Choose one canonical owner when recovered functions overlap.

    Bring-up can temporarily recover an outer function and a later-starting
    nested/overlapping function that both contain the same basic-block VA.
    For a shared block start, the later owner start is the more specific
    recovery because it is closer to that block.  Prefer it deterministically.
    """
    by_start = {}
    for record in sorted(records, key=lambda r: (r.start, -r.owner, r.end)):
        by_start.setdefault(record.start, record)
    return [by_start[va] for va in sorted(by_start)]


def collect_blocks(xbe_data: bytes, functions, disasm=None):
    """Inventory a normalized function database with production recovery.

    An injected disassembler must implement the production Disassembler API,
    including disassemble_cfg(), resync, and extra_leaders.
    """
    translator = FunctionTranslator(xbe_data, dict(functions))
    if disasm is not None:
        translator.disasm = disasm
    translator.discover_static_indirect_targets()
    translator.discover_cfg_ownership()
    records = []
    for start, info in sorted(translator.func_db.items()):
        if start in translator.owned_function_starts:
            continue
        _, blocks = translator.decode_function(start, info["end"])
        for block in blocks:
            records.append(BlockRecord(
                start=block.start,
                end=block.end,
                owner=start,
                successors=tuple(sorted(set(block.successors))),
                instructions=len(block.instructions),
            ))
    return _dedupe_records(records)


def dispatch_stats(records, pointer_size=8):
    """Compute dense-table cost and useful block-size statistics."""
    if not records:
        return {
            "blocks": 0,
            "code_base": None,
            "code_end": None,
            "span_bytes": 0,
            "dense_slots": 0,
            "dense_table_bytes": 0,
            "instructions": 0,
            "avg_instructions_per_block": 0.0,
        }
    base = min(r.start for r in records)
    end = max(r.end for r in records)
    slots = end - base
    insns = sum(r.instructions for r in records)
    return {
        "blocks": len(records),
        "code_base": base,
        "code_end": end,
        "span_bytes": end - base,
        "dense_slots": slots,
        "dense_table_bytes": slots * pointer_size,
        "instructions": insns,
        "avg_instructions_per_block": insns / len(records),
    }


def build_manifest(records):
    stats = dispatch_stats(records)
    base = stats["code_base"] or 0
    return {
        "format": 1,
        "stats": stats,
        "blocks": [
            {
                **asdict(r),
                "successors": list(r.successors),
                "dense_slot": r.start - base,
            }
            for r in records
        ],
    }


def _fmt_size(value):
    units = ("B", "KiB", "MiB", "GiB")
    size = float(value)
    for unit in units:
        if size < 1024.0 or unit == units[-1]:
            return f"{size:.1f} {unit}"
        size /= 1024.0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("xbe", help="path to the title's default.xbe")
    ap.add_argument("--functions", required=True, help="path to functions.json")
    ap.add_argument("-o", "--output", default="block_dispatch.json")
    args = ap.parse_args(argv)

    config.configure_from_xbe(args.xbe)
    with open(args.xbe, "rb") as fh:
        xbe_data = fh.read()
    with open(args.functions, "r", encoding="utf-8") as fh:
        functions = _normalize_functions(json.load(fh))

    records = collect_blocks(xbe_data, functions)
    manifest = build_manifest(records)
    with open(args.output, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")

    stats = manifest["stats"]
    print(f"functions: {len(functions)}")
    print(f"blocks: {stats['blocks']}")
    print(f"instructions: {stats['instructions']}")
    print(f"average instructions/block: {stats['avg_instructions_per_block']:.2f}")
    print(f"guest code span: {_fmt_size(stats['span_bytes'])}")
    print(f"byte-indexed x64 dispatch table: {_fmt_size(stats['dense_table_bytes'])}")
    print(f"manifest: {os.path.abspath(args.output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
