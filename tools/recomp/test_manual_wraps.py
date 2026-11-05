"""
A wrap recomp_manual.c declares (`extern void sub_X_gen(void);`) must get
its generated body, or recomp stops and says which and why: the link error
it replaces (`undefined symbol: sub_X_gen`) named neither.

Run: python -m pytest tools/recomp/test_manual_wraps.py
"""

import os
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.__main__ import (  # noqa: E402
    _wrap_failure,
    _wraps_without_body,
)
from tools.recomp.manual_scan import code_uses, code_references, scan  # noqa: E402

GOOD, MISSING, INNER, OWNER = 0x0009AD80, 0x00096BB0, 0x000A2DE0, 0x000A2D00


def _batch(owned=(), recovered=None):
    func_db = {
        GOOD: {"name": f"sub_{GOOD:08X}"},
        INNER: {"name": f"sub_{INNER:08X}"},
        OWNER: {"name": "FlareOwner"},
    }
    inner = SimpleNamespace(
        owned_function_starts=set(owned),
        _recovered_cfg=recovered or {},
        func_db=func_db,
    )
    return SimpleNamespace(func_db=func_db, translator=inner)


def test_every_wrap_with_a_body_passes():
    assert _wraps_without_body(_batch(), {GOOD, INNER}) == []


def test_missing_and_owned_wraps_are_named():
    batch = _batch(owned={INNER}, recovered={OWNER: {"end": INNER + 0x40}})
    missing = _wraps_without_body(batch, {GOOD, MISSING, INNER})
    assert missing == [
        (MISSING, "not a function in this XBE's analysis"),
        (INNER, "inside FlareOwner (0x000A2D00), not a function start of its own"),
    ]
    text = _wrap_failure("src/recomp_manual.c", missing)
    assert text.startswith(
        "ERROR: Your game's XBE is probably another release (another region "
        "or version)")
    # The XBE's path is the game's (B3's is not game_files/default.xbe):
    # the CLI's hint names it, the toolkit does not guess.
    assert "default.xbe" not in text
    assert "game's fingerprint command names both releases" in text
    assert ("src/recomp_manual.c needs 2 functions this XBE's analysis does "
            "not give it, so the link would fail:\n") in text
    assert "  sub_00096BB0_gen: 0x00096BB0 is not a function in this XBE's analysis\n" in text
    assert "  sub_000A2DE0_gen: 0x000A2DE0 is inside FlareOwner (0x000A2D00)" in text
    with_call = _wrap_failure("m.c", missing, [0x00055555])
    assert "needs 3 functions" in with_call
    assert with_call.endswith(
        "  sub_00055555: used, but 0x00055555 is not a function in this "
        "XBE's analysis")


def test_an_owner_that_is_not_recorded_is_still_described():
    missing = _wraps_without_body(_batch(owned={INNER}), {INNER})
    assert missing == [(INNER, "inside another function, not a function start of its own")]
    assert "m.c needs 1 function this" in _wrap_failure("m.c", missing)


SAMPLE = """\
/* sub_00011111 in a comment is no reference */
// nor sub_00022222 here
static const char *s = "sub_00033333";
extern void sub_00044444(void);
static uint32_t *sub_00077777(int a);
static int g(void) { return sub_00088888(1) + (int)sub_00099999(); }
static void h(void) { RECOMP_ABI_CALL(0x000AAAAAu, sub_000AAAAA); }
extern void __attribute__((weak)) sub_000BBBB1(void);
void sub_000BBBB2(void), sub_000BBBB3(void);
#define CALLIT() sub_000BBBB4()
#define CALLTWO() \\
    sub_000BBBB5()
extern void sub_0009AD80_gen(void);
void sub_0009AD80(void) { sub_0009AD80_gen(); sub_00055555(); }
#if 0
void sub_00066666(void) { }
#endif
"""


def test_code_references_skip_comments_strings_and_disabled_blocks():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "recomp_manual.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(SAMPLE)
        refs = code_references(path)
        calls = code_uses(path)
        _, wrap, referenced = scan(path)
    assert {0x00044444, 0x00077777, 0x00088888, 0x00099999,
            0x000AAAAA, 0x0009AD80, 0x00055555} <= refs
    # Uses only: the declarations (after a type or a '*') and the
    # definition are not; the calls after return, a cast and '{', and the
    # name passed as a value, are.
    assert calls == {0x00088888, 0x00099999, 0x000AAAAA, 0x00055555}
    # Each of these would link: a weak declaration, a second declarator,
    # unused macro bodies (one continued). Only uses in a body count.
    assert {0x000BBBB1, 0x000BBBB2, 0x000BBBB3, 0x000BBBB4, 0x000BBBB5} <= refs
    # The scan's own set keeps comments and strings: they pin sub_ names.
    assert {0x00011111, 0x00022222, 0x00033333} <= referenced
    assert wrap == {0x0009AD80}
    assert code_references(os.path.join(d, "gone.c")) == set()
    assert code_uses(os.path.join(d, "gone.c")) == set()
