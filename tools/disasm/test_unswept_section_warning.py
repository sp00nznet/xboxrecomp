"""A direct call into an executable section the sweep skipped is reported.

With --text-only and no --extra-sections, the XDK library sections (D3D,
DSOUND, XGRPH, XPP) are not swept. _pass_call_targets still makes a function
at each target .text calls there, decoded only to its first terminator, but
the calls inside those bodies are never collected, so every callee becomes a
stub that returns without running. Spartan: Total Warrior lost XGAssembleShader
that way and crashed at boot on the out-buffer it never wrote. disasm now
prints one warning per such section; which candidates it adds is unchanged.

Run: python3 tools/disasm/test_unswept_section_warning.py
"""

import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.loader import SectionInfo  # noqa: E402

TEXT, LIB, RDATA = 0x00010000, 0x00020000, 0x00030000


def _rel(at, target):
    return (target - (at + 5)).to_bytes(4, "little", signed=True)


#  .text  0x10000: call 0x20000      into XGRPH, unswept code
#         0x10005: call 0x30000      into .rdata, flagged executable but data
#         0x1000A: ret
#  XGRPH  0x20000: call 0x20010; ret; int3 padding; 0x20010: xor eax,eax; ret
#  .rdata 0x30000: xor eax,eax; ret
TEXT_BYTES = (b"\xe8" + _rel(TEXT, LIB) + b"\xe8" + _rel(TEXT + 5, RDATA) + b"\xc3")
LIB_BYTES = (b"\xe8" + _rel(LIB, LIB + 0x10) + b"\xc3"
             + b"\xcc" * 10 + b"\x33\xc0\xc3")
RDATA_BYTES = b"\x33\xc0\xc3"


def _section(name, va, data):
    return SectionInfo(name=name, virtual_addr=va, virtual_size=len(data),
                       raw_addr=0, raw_size=len(data), writable=False,
                       executable=True, flags="")


class _Image:
    """Minimal duck-typed BinaryImage: every section executable, as Xbox
    linkers mark them."""

    def __init__(self):
        self.data = {}
        self.sections = []
        for name, va, data in ((".text", TEXT, TEXT_BYTES),
                               ("XGRPH", LIB, LIB_BYTES),
                               (".rdata", RDATA, RDATA_BYTES)):
            sec = _section(name, va, data)
            self.sections.append(sec)
            self.data[name] = data
        self.base_address = TEXT
        self.image_size = RDATA + len(RDATA_BYTES) - TEXT

    def section(self, name):
        return next(s for s in self.sections if s.name == name)

    def get_section_at_va(self, va):
        for s in self.sections:
            if s.virtual_addr <= va < s.virtual_addr + s.virtual_size:
                return s
        return None

    def get_section_data(self, section):
        return self.data[section.name]


def _run(swept_names):
    """Sweep the named sections and run the call-target pass on them; return
    (stdout, candidate addresses added)."""
    img = _Image()
    eng = DisasmEngine(img)
    swept = [img.section(n) for n in swept_names]
    for sec in swept:
        eng.linear_sweep(sec)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine, det.image = eng, img
    added = []
    det._add_candidate = lambda addr, conf, why: added.append(addr)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        det._pass_call_targets(swept)
    return out.getvalue(), added


class UnsweptSectionWarningTest(unittest.TestCase):
    def test_a_call_into_an_unswept_code_section_warns(self):
        out, added = _run([".text"])
        self.assertIn(
            "Warning: 1 call target(s) into unswept code section XGRPH"
            " (e.g. 0x00020000); their callees will be stubs."
            " Add it with --extra-sections XGRPH.", out)
        # A data section is not code even when flagged executable.
        self.assertNotIn(".rdata", out)
        # The warning only reports: the target is still realigned and kept,
        # and the call inside its body is still missed.
        self.assertIn(LIB, added)
        self.assertNotIn(LIB + 0x10, added)

    def test_the_section_swept_does_not_warn(self):
        # Negative control: the same image with XGRPH swept.
        out, added = _run([".text", "XGRPH"])
        self.assertNotIn("Warning", out)
        self.assertIn(LIB, added)
        self.assertIn(LIB + 0x10, added)


if __name__ == "__main__":
    unittest.main()
