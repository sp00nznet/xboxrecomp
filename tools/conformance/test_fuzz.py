import pytest

from tools.conformance.fuzz import _format_seed, generate_cases


def test_fuzz_is_deterministic():
    a = generate_cases(32, 0x1234)
    b = generate_cases(32, 0x1234)
    assert a == b


def test_fuzz_seed_changes_corpus():
    assert generate_cases(16, 1) != generate_cases(16, 2)


def test_fuzz_cases_are_runner_compatible():
    for case in generate_cases(100, 0xC0FFEE):
        assert set(("name", "why", "asm", "inputs", "kind", "tol")) <= set(case)
        assert case["kind"] == "gpr"
        assert case["asm"]
        assert case["inputs"]
        assert all(len(pair) == 2 for pair in case["inputs"])
        assert all(0 <= value <= 0xFFFFFFFF for pair in case["inputs"] for value in pair)


def test_shift_and_rotate_fuzz_compares_results_not_stale_flags():
    cases = generate_cases(2000, 0x51F7)
    shift_cases = [c for c in cases if any(
        insn.split()[0] in {"shl", "shr", "sar", "rol", "ror"}
        for insn in c["asm"]
    )]
    assert shift_cases
    for case in shift_cases:
        assert not any(insn.startswith("set") for insn in case["asm"])


def test_arithmetic_setcc_avoids_unrequested_carry_tracking():
    carry_conditions = {"setb", "setae", "setbe", "seta"}
    cases = generate_cases(2000, 0xCA77)
    for case in cases:
        first = case["asm"][0].split()[0]
        if first in {"add", "sub", "and", "or", "xor"}:
            assert not any(insn.split()[0] in carry_conditions for insn in case["asm"])


def test_seed_format_round_trips_negative_and_positive_values():
    assert _format_seed(0xC0FFEE) == "0xC0FFEE"
    assert _format_seed(-7) == "-7"
    assert int(_format_seed(0xC0FFEE), 0) == 0xC0FFEE
    assert int(_format_seed(-7), 0) == -7


def test_fuzz_count_must_be_positive():
    with pytest.raises(ValueError):
        generate_cases(0, 1)


def test_runner_accepts_flag_preserving_nop_noise(monkeypatch):
    from tools.conformance import __main__ as runner

    if runner._find_vcvars() is None:
        pytest.skip("needs 32-bit MSVC")
    cases = generate_cases(1, 7)
    assert "lea edi, [edi]" in cases[0]["asm"]
    monkeypatch.setattr(runner, "CASES", cases)
    monkeypatch.setattr(runner, "_WHY", {c["name"]: c["why"] for c in cases})
    monkeypatch.setattr(runner, "_TOL", {})
    assert runner.main_with_args(["--only", "snippets"]) == 0
