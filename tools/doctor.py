"""Bring-up health report for an xboxrecomp title.

The doctor combines static pipeline artifacts, recompilation statistics, indirect
call feedback and an optional runtime log into one compact report.  It is meant
to answer the first bring-up question: "what is still unsupported or quietly
wrong enough to investigate next?"

Usage:

    py -3 tools/doctor.py
    py -3 tools/doctor.py --runtime-log game.log --icall-feedback icall-feedback.txt
    py -3 tools/doctor.py --json doctor.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
from collections import Counter


DEFAULTS = {
    "functions": os.path.join("tools", "disasm", "output", "functions.json"),
    "identified": os.path.join("tools", "func_id", "output", "identified_functions.json"),
    "abi": os.path.join("tools", "abi_analysis", "output", "abi_functions.json"),
    "recomp": os.path.join("tools", "recomp", "output", "summary.json"),
}


def _valid_address(value):
    if isinstance(value, str):
        try:
            value = int(value, 0)
        except ValueError:
            return False
    return type(value) is int and 0 <= value <= 0xFFFFFFFF


def _load_json_with_status(path, kind):
    """Return (value, status), distinguishing absent from malformed input."""
    if not path or not os.path.exists(path):
        return None, "missing"
    try:
        with open(path, "r", encoding="utf-8") as fh:
            value = json.load(fh)
        if kind == "recomp":
            if not isinstance(value, dict):
                raise ValueError("expected recompiler summary")
            _flatten_recomp_stats(value)
        else:
            if not isinstance(value, (list, dict)):
                raise ValueError("expected function entries")
            entries = (value.items() if isinstance(value, dict)
                       else ((None, entry) for entry in value))
            address_field = "address" if kind == "abi" else "start"
            for key, entry in entries:
                if not isinstance(entry, dict) or not _valid_address(entry.get(address_field, key)):
                    raise ValueError("expected function address")
                if kind == "identified" and not isinstance(entry.get("category", "unknown"), str):
                    raise ValueError("expected category string")
        return value, "ok"
    except (OSError, ValueError):
        return None, "invalid"


def _count_entries(value):
    if isinstance(value, (list, dict)):
        return len(value)
    return 0


def _flatten_recomp_stats(stats):
    """Normalize single-batch and per-category recompiler summary shapes."""
    out = {"total": 0, "translated": 0, "failed": 0, "unimplemented": Counter()}
    if stats is None:
        return out
    if not isinstance(stats, dict):
        raise ValueError("expected recompiler summary")
    batches = [stats] if "total" in stats else stats.values()
    for batch in batches:
        if not isinstance(batch, dict):
            raise ValueError("expected category summary")
        for key in ("total", "translated", "failed"):
            count = batch.get(key)
            if type(count) is not int or count < 0:
                raise ValueError("expected nonnegative translation count")
            out[key] += count
        unimplemented = batch.get("unimplemented", {})
        if not isinstance(unimplemented, dict):
            raise ValueError("expected unimplemented mnemonic map")
        for mnemonic, addrs in unimplemented.items():
            if isinstance(addrs, list):
                if not all(_valid_address(addr) for addr in addrs):
                    raise ValueError("expected unimplemented instruction addresses")
                out["unimplemented"][mnemonic] += len(addrs)
            elif type(addrs) is int and addrs >= 0:
                out["unimplemented"][mnemonic] += addrs
            else:
                raise ValueError("expected unimplemented instruction count")
    out["unimplemented"] = dict(out["unimplemented"].most_common())
    return out


def parse_icall_feedback(path):
    """Parse the crash-tolerant `VA flags` feedback file written by the runtime."""
    result = {"targets": 0, "resolved": 0, "unresolved": 0, "both": 0, "unresolved_vas": []}
    if not path or not os.path.exists(path):
        return result
    seen = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) < 2:
                    continue
                try:
                    # Runtime feedback uses %08X: hexadecimal without a 0x
                    # prefix.  Base 16 also accepts the prefixed form used by
                    # hand-authored/debug files.
                    va = int(parts[0], 16)
                    flags = int(parts[1], 0)
                except ValueError:
                    continue
                seen[va] = seen.get(va, 0) | flags
    except OSError:
        return result
    result["targets"] = len(seen)
    for va, flags in sorted(seen.items()):
        if flags & 1:
            result["resolved"] += 1
        if flags & 2:
            result["unresolved"] += 1
            result["unresolved_vas"].append(va)
        if flags == 3:
            result["both"] += 1
    return result


_LOG_PATTERNS = (
    # The runtime's common spelling is "[ICALL] Failed to resolve VA ...", so
    # ICALL can appear before the failure word.  Lookaheads deliberately make
    # term order irrelevant.
    ("unresolved_icall", re.compile(
        r"^(?=.*(?:icall|indirect call))(?=.*(?:unresolved|failed|failed to resolve)).*?(0x[0-9a-fA-F]+)",
        re.I)),
    ("kernel_stub", re.compile(
        r"^(?=.*(?:kernel|xboxkrnl))(?=.*(?:stub|unimplemented|unsupported|unresolved|failed))"
        r".*?((?:kernel|xboxkrnl|stub|unimplemented|unsupported|unresolved|failed).*)",
        re.I)),
    ("d3d_unsupported", re.compile(r"(?:D3D8|D3D|NV2A).*?(?:unsupported|unimplemented|unknown).*?([^\r\n]+)", re.I)),
    ("audio_unsupported", re.compile(r"(?:DSOUND|DirectSound|APU|audio|WMA).*?(?:unsupported|unimplemented|stub).*?([^\r\n]+)", re.I)),
    ("unhandled_instruction", re.compile(r"(?:unhandled|unimplemented).*?(?:instruction|mnemonic).*?\b([A-Za-z][A-Za-z0-9]+)\b", re.I)),
)


def parse_runtime_log(path):
    """Aggregate every matching warning payload from a runtime log.

    Keep the complete counters here so priority totals cannot be changed by a
    presentation limit.  Callers that display individual payloads can slice the
    already-sorted dictionaries without losing the category total.
    """
    counters = {name: Counter() for name, _ in _LOG_PATTERNS}
    if not path or not os.path.exists(path):
        return {name: {} for name, _ in _LOG_PATTERNS}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                for name, pattern in _LOG_PATTERNS:
                    match = pattern.search(line)
                    if match:
                        value = " ".join(match.group(1).strip().split())[:160]
                        if name == "kernel_stub":
                            ordinal = re.search(r"\bordinal\s+\d+\b", line, re.I)
                            if ordinal:
                                value = " ".join(ordinal.group().lower().split())
                        counters[name][value] += 1
    except OSError:
        pass
    return {name: dict(counter.most_common()) for name, counter in counters.items()}


def build_report(functions=None, identified=None, abi=None, recomp=None,
                 icall_feedback=None, runtime_log=None):
    artifacts = {
        "functions": _load_json_with_status(functions, "functions"),
        "identified": _load_json_with_status(identified, "identified"),
        "abi": _load_json_with_status(abi, "abi"),
        "recomp": _load_json_with_status(recomp, "recomp"),
    }
    fns = artifacts["functions"][0]
    ids = artifacts["identified"][0] or []
    if isinstance(ids, dict):
        ids = ids.values()
    abi_data = artifacts["abi"][0]
    recomp_data = artifacts["recomp"][0]
    report = {
        "pipeline": {
            "functions": _count_entries(fns),
            "identified_functions": sum(entry.get("category", "unknown") not in ("unknown", "")
                                        for entry in ids),
            "abi_functions": _count_entries(abi_data),
            "missing_artifacts": [name for name, (_, status) in artifacts.items()
                                  if status == "missing"],
            "invalid_artifacts": [name for name, (_, status) in artifacts.items()
                                  if status == "invalid"],
        },
        "recompiler": _flatten_recomp_stats(recomp_data),
        "icalls": parse_icall_feedback(icall_feedback),
        "runtime": parse_runtime_log(runtime_log),
    }
    report["priorities"] = rank_priorities(report)
    return report


def rank_priorities(report):
    priorities = []
    recomp = report["recompiler"]
    icalls = report["icalls"]
    runtime = report["runtime"]
    if recomp["failed"]:
        priorities.append({"severity": "high", "area": "recompiler",
                           "reason": f"{recomp['failed']} recovered functions failed translation"})
    unimpl_total = sum(recomp["unimplemented"].values())
    if unimpl_total:
        top = next(iter(recomp["unimplemented"]), "unknown")
        priorities.append({"severity": "high", "area": "cpu",
                           "reason": f"{unimpl_total} unimplemented instructions remain; top mnemonic: {top}"})
    if icalls["unresolved"]:
        priorities.append({"severity": "high", "area": "control-flow",
                           "reason": f"{icalls['unresolved']} observed indirect targets were unresolved"})
    runtime_classes = (
        ("unresolved_icall", "control-flow", "high"),
        ("kernel_stub", "kernel", "medium"),
        ("d3d_unsupported", "graphics", "medium"),
        ("audio_unsupported", "audio", "medium"),
        ("unhandled_instruction", "cpu", "medium"),
    )
    for key, area, severity in runtime_classes:
        count = sum(runtime.get(key, {}).values())
        if count:
            priorities.append({"severity": severity, "area": area,
                               "reason": f"{count} runtime warning hit(s) matched {key}"})
    invalid = report["pipeline"].get("invalid_artifacts", [])
    if invalid:
        priorities.append({"severity": "medium", "area": "pipeline",
                           "reason": "invalid/unreadable artifacts: " + ", ".join(invalid)})
    if report["pipeline"]["missing_artifacts"]:
        priorities.append({"severity": "info", "area": "pipeline",
                           "reason": "missing artifacts: " + ", ".join(report["pipeline"]["missing_artifacts"])})
    if not priorities:
        priorities.append({"severity": "info", "area": "bring-up",
                           "reason": "no known blockers found in supplied artifacts/logs"})
    return priorities


def _print_report(report):
    p = report["pipeline"]
    r = report["recompiler"]
    i = report["icalls"]
    print("xboxrecomp doctor")
    print("=================")
    print(f"Recovered functions : {p['functions']}")
    print(f"Identified functions: {p['identified_functions']}")
    print(f"ABI entries          : {p['abi_functions']}")
    print(f"Translated functions : {r['translated']}/{r['total']} ({r['failed']} failed)")
    print(f"Unimplemented insns  : {sum(r['unimplemented'].values())}")
    print(f"ICALL targets        : {i['targets']} ({i['unresolved']} unresolved)")
    if r["unimplemented"]:
        print("\nTop unimplemented instructions:")
        for mnemonic, count in list(r["unimplemented"].items())[:10]:
            print(f"  {count:6d}  {mnemonic}")
    print("\nNext things to investigate:")
    for item in report["priorities"]:
        print(f"  [{item['severity'].upper():6}] {item['area']}: {item['reason']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--functions", default=DEFAULTS["functions"])
    ap.add_argument("--identified", default=DEFAULTS["identified"])
    ap.add_argument("--abi", default=DEFAULTS["abi"])
    ap.add_argument("--recomp", default=DEFAULTS["recomp"])
    ap.add_argument("--icall-feedback")
    ap.add_argument("--runtime-log")
    ap.add_argument("--json", metavar="PATH", help="also write the full report as JSON")
    args = ap.parse_args(argv)
    report = build_report(args.functions, args.identified, args.abi, args.recomp,
                          args.icall_feedback, args.runtime_log)
    _print_report(report)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
            fh.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
