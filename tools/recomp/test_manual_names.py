"""
Port names in recomp_manual.c (--names): XBR_FN(n), XBR_GEN(n) and
XBR_VA(n) resolve to the address form through the release's map before the
scan, so one manual file serves every release of a game; `#if XBR_HAVE(n)`
blocks drop out for a release without n; any other use of a name the map
lacks stops recomp by name instead of failing at link.

Run: python -m pytest tools/recomp/test_manual_names.py
"""

import json
import os
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import manual_scan  # noqa: E402
from tools.recomp.__main__ import _wrap_failure  # noqa: E402

NTSC = {"cull": 0x0009AD80, "flare": 0x00096BB0, "pad": 0x000A2DE0, "proj": 0x003AE6A0}

MANUAL = """\
#include "gen/recomp_names.h"

extern void XBR_GEN(cull)(void);

void XBR_FN(cull)(void)
{
    MEMF(XBR_VA(proj)) = 1.0f;
    RECOMP_ABI_CALL(XBR_VA(cull), XBR_GEN(cull));
}

#if XBR_HAVE(flare)
extern void XBR_GEN(flare)(void);
void XBR_FN(flare)(void)
{
    RECOMP_ABI_CALL(XBR_VA(flare), XBR_GEN(flare));
}
#else
void flare_absent(void) { }
#endif

/* XBR_FN(gone) in a comment is no use */
void XBR_FN(pad)(void) { sub_00011000(); }
"""


@pytest.fixture
def manual():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "recomp_manual.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(MANUAL)
        yield path
    manual_scan.NAMES = None
    manual_scan.HAVE = None


def test_the_forms_scan_as_the_address_form(manual):
    manual_scan.NAMES = dict(NTSC)
    skip, wrap, referenced = manual_scan.scan(manual)
    assert skip == {0x0009AD80, 0x00096BB0, 0x000A2DE0}
    assert wrap == {0x0009AD80, 0x00096BB0}
    assert {0x0009AD80, 0x00096BB0, 0x000A2DE0, 0x00011000} <= referenced
    # a sub_X_gen is a wrap, not a use: only the plain call counts
    assert manual_scan.code_uses(manual) == {0x00011000}
    assert manual_scan.definition_names(manual) == {
        "sub_0009AD80", "sub_00096BB0", "sub_000A2DE0"}


def test_another_release_resolves_to_its_addresses(manual):
    manual_scan.NAMES = {"cull": 0x00034F90, "flare": 0x00030B90,
                         "pad": 0x0009EBC0, "proj": 0x0039DE50}
    skip, wrap, _ = manual_scan.scan(manual)
    assert skip == {0x00034F90, 0x00030B90, 0x0009EBC0}
    assert wrap == {0x00034F90, 0x00030B90}


def test_a_missing_have_name_drops_its_block_and_keeps_the_else(manual):
    manual_scan.NAMES = {k: v for k, v in NTSC.items() if k != "flare"}
    skip, wrap, _ = manual_scan.scan(manual)
    assert skip == {0x0009AD80, 0x000A2DE0}
    assert wrap == {0x0009AD80}
    text = manual_scan._live(manual)
    assert "flare_absent" in text and "XBR_HAVE" not in text


def test_a_present_have_name_drops_the_else(manual):
    manual_scan.NAMES = dict(NTSC)
    assert "flare_absent" not in manual_scan._live(manual)


def test_a_missing_name_used_in_code_stops_by_name(manual):
    manual_scan.NAMES = {k: v for k, v in NTSC.items() if k != "proj"}
    with pytest.raises(SystemExit) as e:
        manual_scan.scan(manual)
    assert "lacks: proj" in str(e.value)
    assert "XBR_HAVE(name)" in str(e.value)


def test_forms_without_names_stop():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "m.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(MANUAL)
        manual_scan.NAMES = None
        with pytest.raises(SystemExit) as e:
            manual_scan.scan(path)
        assert "--names" in str(e.value)


def test_the_address_form_is_unchanged_with_names():
    src = "extern void sub_00044000_gen(void);\nvoid sub_00044000(void) { sub_00044000_gen(); }\n"
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "m.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        manual_scan.NAMES = None
        before = manual_scan.scan(path)
        manual_scan.NAMES = dict(NTSC)
        try:
            assert manual_scan.scan(path) == before
        finally:
            manual_scan.NAMES = None


def test_nested_conditionals_inside_a_have_block():
    src = ("#if XBR_HAVE(a)\n#ifdef X\nint x;\n#else\nint y;\n#endif\nint z;\n#else\n"
           "int w;\n#endif\n")
    kept = manual_scan.strip_missing(src, {"a": 1})
    assert "#ifdef X" in kept and "int y;" in kept and "int z;" in kept
    assert "int w;" not in kept
    gone = manual_scan.strip_missing(src, {})
    assert gone.strip() == "int w;"


def test_elif_on_a_have_block_is_refused():
    with pytest.raises(SystemExit):
        manual_scan.strip_missing("#if XBR_HAVE(a)\n#elif 1\n#endif\n", {})


def test_load_names_takes_ints_and_hex():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "names.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"a": 0x10, "b": "0x0009AD80"}, f)
        assert manual_scan.load_names(path) == {"a": 0x10, "b": 0x9AD80}


def test_the_have_form_counts_value_names():
    """A value (a struct offset) has no address, but the release has it:
    its XBR_HAVE block stays, as the header's XBR_HAVE_<name> 1 keeps it
    for the compiler."""
    src = ("#if XBR_HAVE(fence_put)\n"
           "void XBR_FN(cull)(void) { }\n"
           "#else\n"
           "void no_fence(void) { }\n"
           "#endif\n")
    with tempfile.TemporaryDirectory() as d:
        names = os.path.join(d, "names.json")
        with open(names, "w", encoding="utf-8") as f:
            json.dump({"addresses": {"cull": "0x0009AD80"},
                       "have": ["cull", "fence_put"]}, f)
        manual_scan.NAMES, manual_scan.HAVE = manual_scan.load_names_have(names)
        assert manual_scan.HAVE == {"cull", "fence_put"}
        path = os.path.join(d, "recomp_manual.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        try:
            assert manual_scan.definition_names(path) == {"sub_0009AD80"}
            # the release without the value: the #else side
            manual_scan.HAVE = {"cull"}
            assert manual_scan.definition_names(path) == set()
            assert "no_fence" in manual_scan._live(path)
        finally:
            manual_scan.NAMES = manual_scan.HAVE = None


def test_the_plain_form_has_its_address_names():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "names.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"a": 0x10}, f)
        assert manual_scan.load_names_have(path) == ({"a": 0x10}, {"a"})
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"addresses": [], "have": []}, f)
        with pytest.raises(SystemExit):
            manual_scan.load_names_have(path)


def test_the_wrap_failure_names_the_port_name():
    manual_scan.NAMES = {"cull": 0x00034F90}
    try:
        text = _wrap_failure("src/recomp_manual.c",
                             [(0x00034F90, "not a function in this XBE's analysis")])
    finally:
        manual_scan.NAMES = None
    assert "sub_00034F90_gen (XBR_GEN(cull)): 0x00034F90 is not a function" in text
