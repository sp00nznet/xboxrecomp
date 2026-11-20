"""Marking a seed observed invalidates the disassembly cache.

An observed seed passes the mid-instruction guard (test_observed_seeds), so the
same seed list with and without the mark gives different functions. The cache
keyed only on the list handed back the old result.
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.cache import AnalysisCache  # noqa: E402

SEEDS = [0x00010000, 0x001CF6AC]
OUTPUTS = ("summary.json", "functions.json", "xrefs.json",
           "strings.json", "labels.json")


class CacheObservedSeedsTest(unittest.TestCase):
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
                                     seed_functions=SEEDS,
                                     observed_seeds=saved)
        return AnalysisCache(self.out).is_valid(self.xbe, self.json, False,
                                                seed_functions=SEEDS,
                                                observed_seeds=asked)

    def test_marking_a_seed_observed_misses(self):
        self.assertFalse(self._valid(saved=set(), asked={0x001CF6AC}))

    def test_unmarking_a_seed_misses(self):
        self.assertFalse(self._valid(saved={0x001CF6AC}, asked=set()))

    def test_same_marks_hit(self):
        self.assertTrue(self._valid(saved={0x001CF6AC}, asked={0x001CF6AC}))

    def test_no_marks_keep_the_old_key(self):
        # Caches written before observed seeds were keyed stay valid.
        self.assertEqual(AnalysisCache._opts_key(False, None, SEEDS),
                         AnalysisCache._opts_key(False, None, SEEDS,
                                                 observed_seeds=set()))


if __name__ == "__main__":
    unittest.main()
