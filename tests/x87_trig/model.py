"""Vectors and error budget for tests/x87_trig: the x87's 66-bit-pi trig.

    uv run --with mpmath python tests/x87_trig/model.py           # C vectors
    uv run --with mpmath python tests/x87_trig/model.py --sweep   # + budget

Not part of the build or the tests; mpmath is only needed here. The x87
reduces FSIN/FCOS/FSINCOS/FPTAN operands modulo pi/4 with a 66-bit pi, so it
returns f(x * (1 + D)), D = (pi - pi66) / pi66, where libm returns f(x). This
evaluates that at 200 bits, rounds to double, and prints the table the ctest
compares against. --sweep checks the helper's formula (re-implemented here in
double, the arithmetic of recomp_x87_sincos_pi66 on libm's pair, as the
recomp_x87_sin/cos/sincos/tan helpers use it) against the model over
log-uniform operands, in units of the conformance harness's tolerance.
"""

import math
import random
import sys

from mpmath import cos, mp, mpf, pi, sin, tan

mp.prec = 200
PI66 = mpf(0xC90FDAA22168C234C) / mpf(2) ** 66
D = (pi - PI66) / PI66
D_DOUBLE = float(D)

VECTORS = [355.0, 1e5, 1e10, 1e15, 1e18, 1.1e13, 1.2e13, 2.0**62,
           2.0**63 - 2.0**10, math.pi]
VECTORS += [-x for x in VECTORS]


def hw(f, x):
    return f(mpf(x) * (1 + D))


def helper(x):
    """recomp_x87_sincos_pi66 on libm's sin and cos, in double, for the sweep."""
    if not math.isfinite(x) or abs(x) <= math.pi / 4:
        return math.sin(x), math.cos(x)
    e = x * D_DOUBLE
    s, c = math.sin(x), math.cos(x)
    if abs(e) < 2.0**-26:
        se, v = e, 0.0
    else:
        se = math.sin(e)
        h = math.sin(e * 0.5)
        v = 2.0 * h * h
    return s + (c * se - s * v), c - (s * se + c * v)


def crit(got, truth):
    """The harness's rule, |d| <= tol * max(|a|, |b|, 1), as a fraction of tol."""
    b = float(truth)
    return float(abs(mpf(got) - truth) / (1e-15 * max(abs(got), abs(b), 1.0)))


def table():
    print(f"/* D = (pi - pi66) / pi66 = {mp.nstr(D, 20)} = {D_DOUBLE.hex()} */")
    print("/* libm self-check: correctly rounded f(1e10) */")
    x = mpf(1e10)
    print(f"#define TRUE_SIN_1E10 {float(sin(x))!r}")
    print(f"#define TRUE_COS_1E10 {float(cos(x))!r}")
    print(f"#define TRUE_TAN_1E10 {float(tan(x))!r}")
    print("/* x, x87 sin, cos, tan of x: f(x * (1 + D)) at 200 bits */")
    for x in VECTORS:
        hc = hw(cos, x)
        assert abs(hc) > 1e-2, f"{x!r} sits in the tan pole band"
        print(f"    {{ {x.hex()}, {float(hw(sin, x))!r}, {float(hc)!r},"
              f" {float(hw(tan, x))!r} }}, /* {x!r} */")


def sweep(n=18000):
    random.seed(7)
    for branch, lo, hi in (("fast", -1, 45), ("general", 46, 62)):
        worst = {"sin": 0.0, "cos": 0.0, "tan": 0.0}
        for _ in range(n // 2):
            x = math.ldexp(1.0 + random.random(), random.randint(lo, hi))
            if random.random() < 0.5:
                x = -x
            if abs(x) >= 2.0**63:
                continue
            s, c = helper(x)
            hs, hc = hw(sin, x), hw(cos, x)
            worst["sin"] = max(worst["sin"], crit(s, hs))
            worst["cos"] = max(worst["cos"], crit(c, hc))
            if abs(hc) > 1e-2:
                worst["tan"] = max(worst["tan"], crit(s / c, hw(tan, x)))
        print(f"/* sweep {branch}: worst fraction of tol "
              + ", ".join(f"{k} {v:.2f}" for k, v in worst.items()) + " */")


if __name__ == "__main__":
    table()
    if "--sweep" in sys.argv:
        sweep()
