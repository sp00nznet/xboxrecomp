"""RECOMP_STUB_LOG: an unresolved stub names itself the first time it runs.

A stub skips whatever code really lives at its address, so reaching one is a
likely cause of a stall or a wrong result far away. The emitted stubs log the
stub address, the return address and esp once per stub when RECOMP_STUB_LOG
is set, and stay silent otherwise. The run half compiles the generated stub
file against a minimal recomp_types.h and skips itself without a C compiler.
"""

import os
import shutil
import subprocess
import tempfile

import pytest

from tools.recomp.lifter import Lifter
from tools.recomp.translator import BatchTranslator


CALLER = 0x00120000
MISSING = 0x00130000


class _FakeTranslator:
    def __init__(self):
        self.owned_function_starts = set()
        self.lifter = Lifter()
        self.lifter.referenced_calls = {MISSING: f"sub_{MISSING:08X}"}

    def translate_function(self, addr, func_info):
        return f"void {func_info['name']}(void) {{}}"

    def _stub_ret_bytes(self, addr):
        return 8


def _emit(output_dir):
    batch = BatchTranslator.__new__(BatchTranslator)
    batch.translator = _FakeTranslator()
    batch.translate_batch_split(
        [(CALLER, {"name": f"sub_{CALLER:08X}"})], output_dir)
    with open(os.path.join(output_dir, "recomp_stubs_unresolved.c")) as f:
        return f.read()


def test_stub_calls_the_one_shot_log_and_still_pops():
    with tempfile.TemporaryDirectory() as out:
        stubs = _emit(out)
    assert 'getenv("RECOMP_STUB_LOG")' in stubs
    assert (f"void sub_{MISSING:08X}(void) {{ RECOMP_STUB_HIT(0x{MISSING:08X}u); "
            "g_esp += 12;") in stubs


_TYPES_H = r"""
#include <stdint.h>
static uint8_t g_stack[64];
static uint32_t g_esp = 16;
#define MEM32(a) (*(uint32_t *)(g_stack + (a)))
"""

_MAIN_C = r"""
#include "recomp_stubs_unresolved.c"
int main(void)
{
    MEM32(16) = 0x00120005u;
    sub_00130000();
    g_esp = 16;
    sub_00130000();
    return g_esp == 28 ? 0 : 1;
}
"""


def _cc():
    for cc in ("clang", "gcc", "cc"):
        if shutil.which(cc):
            return shutil.which(cc)
    return None


@pytest.mark.skipif(_cc() is None, reason="no C compiler on PATH")
@pytest.mark.parametrize("value, lines", [(None, 0), ("0", 0), ("1", 1)])
def test_stub_logs_once_only_when_asked(value, lines):
    with tempfile.TemporaryDirectory() as out:
        _emit(out)
        with open(os.path.join(out, "recomp_types.h"), "w") as f:
            f.write(_TYPES_H)
        with open(os.path.join(out, "main.c"), "w") as f:
            f.write(_MAIN_C)
        exe = os.path.join(out, "stub_log")
        subprocess.run([_cc(), "-o", exe, os.path.join(out, "main.c")],
                       check=True, cwd=out)
        env = dict(os.environ)
        env.pop("RECOMP_STUB_LOG", None)
        if value is not None:
            env["RECOMP_STUB_LOG"] = value
        run = subprocess.run([exe], env=env, capture_output=True, text=True)
    assert run.returncode == 0
    logged = [l for l in run.stderr.splitlines() if l.startswith("[STUB]")]
    assert len(logged) == lines
    if lines:
        assert logged[0] == ("[STUB] unresolved 0x00130000 reached, "
                             "return 0x00120005 esp 0x00000010")
