"""Deterministic differential fuzz cases for the x86 lifter.

This module generates *safe* short instruction sequences and feeds them through
the existing conformance runner.  The native 32-bit x86 execution remains the
oracle; fuzzing only varies instruction combinations and inputs.

Examples::

    py -3 -m tools.conformance.fuzz
    py -3 -m tools.conformance.fuzz --count 250 --seed 0xC0FFEE
    py -3 -m tools.conformance.fuzz --count 1000 --seed 7 --keep -v

The generator deliberately avoids privileged instructions, memory operands,
division and other operations that can fault.  Cases are reproducible from the
printed seed and case index, so a failure can be promoted into ``cases.py`` as a
permanent regression test.
"""

from __future__ import annotations

import argparse
import random
import sys
from typing import Iterable

from .cases import Case

# Boundary-heavy values make narrow-width and flag bugs much easier to hit than
# uniform random 32-bit inputs alone.
_EDGES = (
    0x00000000,
    0x00000001,
    0x00000002,
    0x0000007F,
    0x00000080,
    0x000000FF,
    0x00000100,
    0x00007FFF,
    0x00008000,
    0x0000FFFF,
    0x00010000,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFE,
    0xFFFFFFFF,
    0x12345678,
    0x87654321,
)

_WIDTHS = {
    8: ("al", "cl"),
    16: ("ax", "cx"),
    32: ("eax", "ecx"),
}

_CONDS = ("z", "nz", "b", "ae", "be", "a", "s", "ns", "l", "ge", "le", "g")
# The conformance snippet runner only enables explicit carry tracking when the
# sequence contains ADC/SBB.  Do not manufacture SETcc carry consumers here or
# a correct arithmetic lift can look wrong merely because the harness did not
# request CF bookkeeping.  Carry itself is fuzzed by the dedicated ADC/SBB
# families below.
_NON_CARRY_CONDS = ("z", "nz", "s", "ns", "l", "ge", "le", "g")
_FLAG_PRESERVING_NOISE = (
    "mov edx, edx",
    "mov esi, esi",
    "lea edi, [edi]",
)


def _inputs(rng: random.Random, n: int = 20) -> list[tuple[int, int]]:
    """Return deterministic boundary-biased (eax, ecx) inputs."""
    out: list[tuple[int, int]] = []
    # Guarantee zero/nonzero, signed boundaries and carry-heavy pairs first.
    forced = (
        (0, 0),
        (0, 1),
        (1, 0),
        (0x7FFFFFFF, 1),
        (0x80000000, 1),
        (0xFFFFFFFF, 1),
        (0x00000080, 0x000000FF),
        (0x00008000, 0x0000FFFF),
    )
    out.extend(forced[: min(len(forced), n)])
    while len(out) < n:
        if rng.random() < 0.8:
            a = rng.choice(_EDGES)
            b = rng.choice(_EDGES)
        else:
            a = rng.getrandbits(32)
            b = rng.getrandbits(32)
        out.append((a, b))
    return out


def _with_noise(rng: random.Random, body: list[str]) -> list[str]:
    """Sometimes separate a flag producer from its consumer."""
    if rng.random() < 0.65:
        body.insert(-2, rng.choice(_FLAG_PRESERVING_NOISE))
    return body


def _cmp_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, rhs = _WIDTHS[width]
    op = rng.choice(("cmp", "test"))
    cond = rng.choice(_CONDS)
    body = [f"{op} {lhs}, {rhs}", f"set{cond} dl", "movzx eax, dl"]
    body = _with_noise(rng, body)
    return Case(
        f"fuzz_{index:04d}_{op}_set{cond}_i{width}",
        f"fuzz: {op} i{width} with a later SET{cond.upper()}",
        body,
        _inputs(rng),
    )


def _arith_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, rhs = _WIDTHS[width]
    op = rng.choice(("add", "sub", "and", "or", "xor"))
    cond = rng.choice(_NON_CARRY_CONDS)
    body = [f"{op} {lhs}, {rhs}", f"set{cond} dl", "movzx eax, dl"]
    body = _with_noise(rng, body)
    return Case(
        f"fuzz_{index:04d}_{op}_set{cond}_i{width}",
        f"fuzz: {op} i{width} flag result consumed by SET{cond.upper()}",
        body,
        _inputs(rng),
    )


def _incdec_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    op = rng.choice(("inc", "dec"))
    carry = rng.choice(("stc", "clc"))
    consumer = rng.choice(("adc eax, 0", "sbb eax, 0"))
    body = [carry, f"{op} {lhs}", rng.choice(_FLAG_PRESERVING_NOISE), consumer]
    return Case(
        f"fuzz_{index:04d}_{carry}_{op}_{consumer.split()[0]}_i{width}",
        f"fuzz: {op} i{width} must preserve incoming carry",
        body,
        _inputs(rng),
    )


def _neg_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    consumer = rng.choice(("adc eax, 0", "sbb eax, 0"))
    body = [f"neg {lhs}", rng.choice(_FLAG_PRESERVING_NOISE), consumer]
    return Case(
        f"fuzz_{index:04d}_neg_{consumer.split()[0]}_i{width}",
        f"fuzz: NEG i{width} carry survives flag-preserving instructions",
        body,
        _inputs(rng),
    )


def _shift_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    op = rng.choice(("shl", "shr", "sar", "rol", "ror"))
    # Compare the shifted/rotated value directly.  Counts that mask to zero
    # preserve the incoming flags, while ROL/ROR preserve ZF/SF for every
    # count.  Attaching an arbitrary SETcc would therefore compare harness
    # setup flags rather than the instruction under test and create false
    # positives.  Dedicated cases with explicit flag setup can fuzz those
    # semantics separately.
    body = [f"{op} {lhs}, cl"]
    inputs = _inputs(rng)
    counts = (0, 1, max(1, width - 1), width, 31, 32, 33, 255)
    inputs[: len(counts)] = [(_EDGES[i % len(_EDGES)], c) for i, c in enumerate(counts)]
    return Case(
        f"fuzz_{index:04d}_{op}_i{width}",
        f"fuzz: {op} i{width} count masking and result preservation",
        body,
        inputs,
    )


def _zeroing_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    incoming = rng.choice(("stc", "clc"))
    consumer = rng.choice(("adc eax, 0", "sbb eax, 0"))
    body = [incoming, f"xor {lhs}, {lhs}", rng.choice(_FLAG_PRESERVING_NOISE), consumer]
    return Case(
        f"fuzz_{index:04d}_xorzero_{consumer.split()[0]}_i{width}",
        f"fuzz: XOR self i{width} clears carry before {consumer.split()[0].upper()}",
        body,
        _inputs(rng),
    )


_FAMILIES = (_cmp_case, _arith_case, _incdec_case, _neg_case, _shift_case, _zeroing_case)


def generate_cases(count: int, seed: int) -> list[dict]:
    """Generate ``count`` deterministic conformance Case dictionaries."""
    if count < 1:
        raise ValueError("count must be >= 1")
    rng = random.Random(seed)
    cases = []
    for i in range(count):
        family = rng.choice(_FAMILIES)
        cases.append(family(rng, i))
    return cases


def _case_listing(cases: Iterable[dict]) -> str:
    lines = []
    for case in cases:
        lines.append(f"{case['name']}: " + "; ".join(case["asm"]))
    return "\n".join(lines)


def _format_seed(seed: int) -> str:
    """Return a seed spelling argparse can parse again verbatim."""
    return str(seed) if seed < 0 else f"0x{seed:X}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="python -m tools.conformance.fuzz",
        description="Generate deterministic x86 snippets and compare native execution to the lifter.",
    )
    ap.add_argument("--count", type=int, default=200, help="number of generated snippets (default 200)")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x58424F58,
                    help="PRNG seed, decimal or 0x-prefixed (default 0x58424F58)")
    ap.add_argument("--list", action="store_true", help="print generated snippets without compiling them")
    ap.add_argument("--keep", action="store_true", help="keep conformance build artifacts")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        cases = generate_cases(args.count, args.seed)
    except ValueError as exc:
        ap.error(str(exc))

    seed_text = _format_seed(args.seed)
    print(f"xboxrecomp lifter fuzz seed={seed_text} count={len(cases)}")
    if args.list:
        print(_case_listing(cases))
        return 0

    # Reuse the production differential runner rather than maintaining a
    # second assembler/lifter/harness path.  Only the case corpus is replaced.
    from . import __main__ as runner

    runner.CASES = cases
    runner._WHY = {c["name"]: c["why"] for c in cases}
    runner._TOL = {c["name"]: c.get("tol", 0.0) for c in cases}

    runner_args = ["--only", "snippets"]
    if args.keep:
        runner_args.append("--keep")
    if args.verbose:
        runner_args.append("--verbose")
    rc = runner.main_with_args(runner_args)
    if rc:
        print(
            f"\nReproduce with: py -3 -m tools.conformance.fuzz --count {args.count} "
            f"--seed {seed_text} --keep -v",
            file=sys.stderr,
        )
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
