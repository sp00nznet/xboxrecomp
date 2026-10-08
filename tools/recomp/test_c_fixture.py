"""Compiler-backed fixture plumbing must execute or skip explicitly."""
import subprocess
import pytest
from . import c_fixture


def test_actual_compile_and_compile_failure(tmp_path):
    cc=c_fixture.compiler()
    if not cc: pytest.skip('C compiler unavailable')
    c,exe=tmp_path/'t.c',tmp_path/'t.exe'
    c.write_text('int main(void){return 7;}')
    built=subprocess.run(c_fixture.command(cc,[c],exe),cwd=tmp_path,capture_output=True)
    assert built.returncode==0,built.stdout+built.stderr
    assert subprocess.run([str(exe)],timeout=5).returncode==7
    c.write_text('#error fixture must reject this source\n')
    assert subprocess.run(c_fixture.command(cc,[c],exe),cwd=tmp_path,capture_output=True).returncode!=0


def test_missing_compiler_is_a_real_skip(monkeypatch):
    from . import test_dispatch_flat, test_fpu_branch
    monkeypatch.setattr(c_fixture,'compiler',lambda:None)
    with pytest.raises(pytest.skip.Exception):
        test_dispatch_flat.test_generated_dispatch_flat_matches_binary_search()
    with pytest.raises(pytest.skip.Exception):
        test_fpu_branch.test_idiom_semantics_compiled()


def test_unsupported_msvc_sanitizer_is_not_silently_dropped():
    with pytest.raises(ValueError,match='sanitizer'):
        c_fixture.command('cl.exe',['source.c'],'out.exe',gnu_flags=['-fsanitize=signed-integer-overflow'])
