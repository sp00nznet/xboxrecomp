"""A body that runs off its end with no function there dispatches by address.

BLiNX 2's sub_001CFCA0 was cut mid-instruction by a phantom call target at
0x001CFCE8. Its lifted body ended after two stores and returned, skipping
the rest of the code and the ret, so every caller came back with esp four
bytes low and the second Tom Tom Gang rank exam hung. x86 keeps executing at
`end`; the C body now does the same through RECOMP_FALLOFF_TAIL, which
reaches an alias there or logs the miss once and pops the return address.
"""
import pytest

from . import config
from .translator import FunctionTranslator

BASE = 0x10000
# mov dword ptr [0x00EC5340], 2: no control transfer
STORE = bytes.fromhex("c705" "4053ec00" "02000000")
RET = bytes.fromhex("c3")


@pytest.fixture(autouse=True)
def restore_layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", list(config._SECTIONS))


def _translate(image, placements, code_size=None):
    size = len(image) if code_size is None else code_size
    config._install(
        [config.Section(".text", BASE, size, 0, size, True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="falloff-test")
    db = {va: {"start": f"0x{va:08X}", "end": va + n, "_addr": va, "size": n}
          for va, n in placements}
    t = FunctionTranslator(image, db)
    return t.translate_function(BASE, db[BASE]), t


def test_running_off_into_unclaimed_code_tail_dispatches():
    image = STORE + STORE + RET
    code, t = _translate(image, [(BASE, len(STORE))])
    assert f"RECOMP_FALLOFF_TAIL(0x{BASE + len(STORE):08X}u)" in code
    assert t.falloffs == [(BASE, BASE + len(STORE))]


def test_running_into_the_next_function_is_a_plain_fallthrough():
    image = STORE + RET
    code, t = _translate(image, [(BASE, len(STORE)), (BASE + len(STORE), 1)])
    assert "RECOMP_FALLOFF_TAIL" not in code
    assert t.falloffs == []


def test_a_body_that_returns_is_untouched():
    image = STORE + RET
    code, t = _translate(image, [(BASE, len(image))])
    assert "RECOMP_FALLOFF_TAIL" not in code
    assert t.falloffs == []


def test_running_off_the_end_of_the_section_emits_nothing():
    # Past the last code byte there is nothing to execute: no dispatch.
    image = STORE
    code, t = _translate(image, [(BASE, len(STORE))])
    assert "RECOMP_FALLOFF_TAIL" not in code
    assert t.falloffs == []
