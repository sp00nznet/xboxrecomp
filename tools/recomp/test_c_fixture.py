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

def test_library_flags_follow_sources():
    """GNU ld drops -l libraries that appear before the objects that need them."""
    cmd = c_fixture.command('gcc', ['a.c'], 'a.exe',
                            gnu_flags=['-std=c11', '-Werror', '-lm'])
    src = cmd.index('a.c')
    assert cmd.index('-lm') > src
    assert cmd.index('-std=c11') < src
    assert cmd.index('-Werror') < src
    # compile-only does not pass the library through: there is no link step.
    compile_only = c_fixture.command('gcc', ['a.c'], 'a.o', compile_only=True,
                                     gnu_flags=['-Werror', '-lm'])
    assert '-lm' not in compile_only
    # The MSVC path still ignores GNU library flags, as before.
    msvc = c_fixture.command('cl.exe', ['a.c'], 'a.exe',
                             gnu_flags=['-Werror', '-lm'])
    assert '-lm' not in msvc


def test_libm_links_when_floor_is_not_folded(tmp_path):
    """A non-constant floor() must link on GNU with -lm in gnu_flags."""
    cc = c_fixture.compiler()
    if not cc:
        pytest.skip('C compiler unavailable')
    if c_fixture.is_msvc(cc):
        pytest.skip('MSVC does not take -lm')
    c, exe = tmp_path / 't.c', tmp_path / 't.exe'
    c.write_text(
        '#include <math.h>\n'
        'int floor_of(double x) { return (int)floor(x); }\n'
        'int main(int argc, char **argv) {\n'
        '    (void)argv;\n'
        '    return floor_of(argc + 0.5);\n'
        '}\n')
    built = subprocess.run(
        c_fixture.command(cc, [c], exe, gnu_flags=['-lm']),
        cwd=tmp_path, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    # argc == 1 => floor(1.5) == 1
    assert subprocess.run([str(exe)], timeout=5).returncode == 1
