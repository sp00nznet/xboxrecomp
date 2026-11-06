"""_find_vcvars finds a Visual Studio whatever its folder is called.

Visual Studio 2022 and earlier live in a folder named for the year; newer
releases are named by major version ("18"). A lookup of "2022" alone made
`py -3 -m tools.conformance` print "no 32-bit toolchain" on a machine that had
one, and `pytest tools/` then skipped the compiled tests without saying why.
"""

import os

import pytest

from . import __main__ as conformance


def _make(root, *parts):
    d = os.path.join(root, "Microsoft Visual Studio", *parts, "VC", "Auxiliary", "Build")
    os.makedirs(d)
    p = os.path.join(d, "vcvars32.bat")
    open(p, "w").close()
    return p


@pytest.fixture
def pf(tmp_path, monkeypatch):
    """An empty Program Files; the x86 one points at an empty directory too."""
    root = str(tmp_path / "pf")
    other = str(tmp_path / "pf86")
    os.makedirs(root)
    os.makedirs(other)
    monkeypatch.setenv("ProgramFiles", root)
    monkeypatch.setenv("ProgramFiles(x86)", other)
    return root


def test_no_visual_studio(pf):
    assert conformance._find_vcvars() is None


def test_year_folder(pf):
    want = _make(pf, "2019", "Community")
    assert conformance._find_vcvars() == want


def test_major_version_folder(pf):
    # Visual Studio releases after 2022 are named by major version
    want = _make(pf, "18", "Community")
    assert conformance._find_vcvars() == want


def test_2022_wins_when_present(pf):
    _make(pf, "18", "Community")
    _make(pf, "2019", "Professional")
    want = _make(pf, "2022", "BuildTools")
    assert conformance._find_vcvars() == want


def test_newest_other_install_wins(pf):
    _make(pf, "2019", "Community")
    want = _make(pf, "18", "Enterprise")
    assert conformance._find_vcvars() == want


def test_a_folder_without_vcvars_is_skipped(pf):
    os.makedirs(os.path.join(pf, "Microsoft Visual Studio", "18", "Community"))
    want = _make(pf, "2019", "Community")
    assert conformance._find_vcvars() == want


def test_non_version_folders_are_ignored(pf):
    os.makedirs(os.path.join(pf, "Microsoft Visual Studio", "Installer"))
    os.makedirs(os.path.join(pf, "Microsoft Visual Studio", "Shared"))
    assert conformance._find_vcvars() is None
