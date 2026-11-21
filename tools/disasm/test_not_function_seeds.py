"""A seed entry can say an address is never a function start.

Some false starts pass every check: a "call" decoded from data whose target
lands mid-instruction in a real function, cutting it in two, or a coincidental
match against a table of addresses. No heuristic rules them out without
breaking real functions elsewhere, so a seed file can name them:
{"start": "0x...", "not_function": true}. Nothing proposes them as a start,
and the mark is part of the disassembly cache key.
"""
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.__main__ import _load_seed_functions  # noqa: E402
from tools.disasm.cache import AnalysisCache  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.test_returning_body_switch import BASE, _engine  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402

OUTPUTS = ("summary.json", "functions.json", "xrefs.json",
           "strings.json", "labels.json")


def _caller_and_callee():
    # +0:  call CALLEE ; ret                  e8 <rel32> c3
    # +6:  int3 padding
    # +16: CALLEE: push ebp ; mov ebp, esp ; pop ebp ; ret
    callee = BASE + 16
    data = b"\xe8" + struct.pack("<i", callee - (BASE + 5)) + b"\xc3"
    data += b"\xcc" * (16 - len(data))
    data += b"\x55\x8b\xec\x5d\xc3" + b"\xcc" * 16
    return data, callee


def _detect(not_functions=()):
    data, callee = _caller_and_callee()
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.not_functions = set(not_functions)
    det.detect_all([image.section])
    return det, callee


class NotFunctionTest(unittest.TestCase):
    def test_a_call_target_is_a_function(self):
        det, callee = _detect()
        self.assertIn(callee, det.functions)

    def test_a_marked_call_target_is_not(self):
        det, callee = _detect(not_functions={BASE + 16})
        self.assertNotIn(callee, det.functions)
        self.assertIn(BASE, det.functions)

    def test_seed_file_entries(self):
        entries = [{"start": "0x00010000"},
                   {"start": "0x00010010", "not_function": True}]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "seeds.json")
            with open(path, "w") as f:
                json.dump(entries, f)
            marked = set()
            got = _load_seed_functions(path, set(), marked)
        self.assertEqual(got, [0x00010000])
        self.assertEqual(marked, {0x00010010})


class NotFunctionCacheTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        d = self._tmp.name
        self.xbe = os.path.join(d, "t.xbe")
        self.json = os.path.join(d, "t.json")
        for path, data in ((self.xbe, b"XBEH"), (self.json, b"{}")):
            with open(path, "wb") as f:
                f.write(data)
        self.out = os.path.join(d, "out")
        os.makedirs(self.out)
        for name in OUTPUTS:
            open(os.path.join(self.out, name), "w").close()

    def tearDown(self):
        self._tmp.cleanup()

    def _valid(self, saved, asked):
        AnalysisCache(self.out).save(self.xbe, self.json, False, 1.0,
                                     seed_functions=[BASE],
                                     not_functions=saved)
        return AnalysisCache(self.out).is_valid(self.xbe, self.json, False,
                                                seed_functions=[BASE],
                                                not_functions=asked)

    def test_marking_an_address_misses(self):
        self.assertFalse(self._valid(saved=set(), asked={BASE + 16}))

    def test_same_marks_hit(self):
        self.assertTrue(self._valid(saved={BASE + 16}, asked={BASE + 16}))

    def test_no_marks_keep_the_old_key(self):
        self.assertEqual(AnalysisCache._opts_key(False, None, [BASE]),
                         AnalysisCache._opts_key(False, None, [BASE],
                                                 not_functions=set()))


if __name__ == "__main__":
    unittest.main()
