"""A body that ends on a frame teardown with no terminator is reported.

Spartan's D3DXMatrixLookAtLH lost its `ret 0x10` to a section raw size one
byte short: the function ended at `leave`, and its translation returned
without popping the return address and arguments. `leave` and `pop ebp` only
ever precede a ret in compiler output, so ending on one is a cut body.
"""
import pytest

from . import config
from .translator import FunctionTranslator

BASE = 0x10000
FRAME = bytes.fromhex("558bec")        # push ebp; mov ebp, esp
LEAVE = bytes.fromhex("c9")
POP_EBP = bytes.fromhex("5d")
RET = bytes.fromhex("c3")
ADD_EAX_AL = bytes.fromhex("0000")     # what a data tail decodes as


def _warnings(capsys, image, placements):
    config._install(
        [config.Section(".text", BASE, 0x100, 0, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="teardown-test")
    db = {}
    for va, size in placements:
        db[va] = {"start": f"0x{va:08X}", "end": va + size,
                  "_addr": va, "size": size}
    capsys.readouterr()
    FunctionTranslator(image.ljust(0x100, b"\xcc"), db).translate_function(
        BASE, db[BASE])
    return [line for line in capsys.readouterr().err.splitlines()
            if "no terminator" in line]


@pytest.fixture(autouse=True)
def restore_layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", list(config._SECTIONS))


@pytest.mark.parametrize("teardown", [LEAVE, POP_EBP])
def test_teardown_without_ret_warns(capsys, teardown):
    body = FRAME + teardown
    warnings = _warnings(capsys, body, [(BASE, len(body))])
    assert len(warnings) == 1
    assert "sub_00010000" in warnings[0]
    assert f"0x{BASE + len(FRAME):08X}" in warnings[0]


def test_teardown_then_ret_is_silent(capsys):
    body = FRAME + LEAVE + RET
    assert _warnings(capsys, body, [(BASE, len(body))]) == []


def test_data_tail_is_silent(capsys):
    assert _warnings(capsys, ADD_EAX_AL, [(BASE, len(ADD_EAX_AL))]) == []


def test_teardown_falling_into_the_next_function_is_silent(capsys):
    body = FRAME + LEAVE
    image = body + RET
    placements = [(BASE, len(body)), (BASE + len(body), len(RET))]
    assert _warnings(capsys, image, placements) == []


def test_aliases_sharing_a_cut_end_warn_once(capsys):
    body = FRAME + LEAVE
    config._install(
        [config.Section(".text", BASE, 0x100, 0, len(body), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="teardown-test")
    db = {va: {"start": f"0x{va:08X}", "end": BASE + len(body), "_addr": va,
               "size": BASE + len(body) - va}
          for va in (BASE, BASE + 1)}
    db[BASE + 1]["detection_method"] = "tail_jump_alias"
    t = FunctionTranslator(body.ljust(0x100, b"\xcc"), db)
    capsys.readouterr()
    for va in db:
        t.translate_function(va, db[va])
    err = capsys.readouterr().err
    assert err.count("no terminator") == 1
