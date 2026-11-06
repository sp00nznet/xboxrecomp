"""Deterministic bounded x86 semantic case generator."""
import argparse
import json
import os
import random
import subprocess
from pathlib import Path
from .case import Case
from .runner import run_unicorn, RunnerUnavailable
from .compare import first_divergence, format_divergence

OPS = ("01d8", "11d8", "29d8", "19d8", "39d8", "85d8", "40", "48",
       "d1e0", "d1e8", "d1f8", "d1d0", "d1d8", "b001", "b401", "0fb6c0", "0fbec0",
       "0fb7c0", "0fbfc0", "0f94c0", "0f45c3", "f7e3", "0fafc3", "f7f3", "f7fb",
       "50", "58", "d1d0", "d1d8", "f3a4", "f3ab", "f3a6", "f3ae",
       "c8000000", "c9", "d9e8", "d9fa", "0f57c0", "0f58c1")
REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eflags")

def make_case(seed, count):
    rng = random.Random(seed)
    code = "".join(rng.choice(OPS) for _ in range(count))
    state = {name: rng.getrandbits(32) for name in REGS}
    state.update({"esp": 0x8FF0, "ebp": 0x8F00, "esi": 0x8100,
                  "edi": 0x8200, "ecx": 4, "ebx": 3})
    return Case({"name": f"x86-semantics-{seed}", "code": code, "entry_eip": 0x1000,
        "stop": {"instructions": count}, "state": state,
        "memory": [{"address": 0x8000, "data": "00" * 4096}], "calls": [], "seed": seed}).validate()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=10)
    parser.add_argument("--instructions", type=int, default=16)
    parser.add_argument("--out", default="cases/fuzz")
    parser.add_argument("--recomp-command", help="adapter command; receives XBOXRECOMP_DIFF_CASE/OUT")
    parser.add_argument("--stop-on-failure", action="store_true")
    args = parser.parse_args()
    try:
        for offset in range(args.cases):
            case = make_case(args.seed + offset, args.instructions)
            path = Path(args.out) / case.data["name"]
            path.mkdir(parents=True, exist_ok=True)
            case.save(path / "case.json")
            oracle = run_unicorn(case)
            oracle_path = path / "oracle.json"
            oracle_path.write_text(json.dumps(oracle, indent=2) + "\n", encoding="utf-8")
            if args.recomp_command:
                recomp_path = path / "recomp.json"
                environment = os.environ.copy()
                environment["XBOXRECOMP_DIFF_CASE"] = str(path / "case.json")
                environment["XBOXRECOMP_DIFF_OUT"] = str(recomp_path)
                subprocess.run(args.recomp_command, shell=True, check=True, env=environment)
                divergence = first_divergence(oracle, json.loads(recomp_path.read_text(encoding="utf-8")))
                if divergence:
                    print(format_divergence(divergence))
                    print(f"saved failing case: {path}")
                    if args.stop_on_failure:
                        return 1
        print(f"generated {args.cases} deterministic cases from seed {args.seed}")
        return 0
    except RunnerUnavailable as error:
        print(f"cases generated; oracle skipped: {error}")
        return 0

if __name__ == "__main__":
    raise SystemExit(main())
