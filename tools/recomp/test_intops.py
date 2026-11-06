"""The emitted C must agree with the CPU at width/count boundaries."""
import pytest

from tools.conformance import __main__ as compiler
from .fpdiff import cases, run


@pytest.mark.parametrize("group", ("shift", "product", "save"))
def test_intops_native(tmp_path, group):
    if compiler._find_vcvars() is None:
        pytest.skip("32-bit MSVC required for the native differential check")
    corpus = [c for c in cases() if c["name"].startswith("intops_")]
    if group == "shift":
        corpus = [c for c in corpus if c["name"].endswith(("imm32", "cl33"))]
    elif group == "product":
        corpus = [c for c in corpus if c["name"].startswith(("intops_mul", "intops_imul"))]
    else:
        corpus = [c for c in corpus if c["name"].startswith("intops_fnsave")]
    result = run(tmp_path, corpus)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "UNSUPPORTED" not in result.stdout, result.stdout
    assert "C4293" not in (tmp_path / "compile.txt").read_text()
