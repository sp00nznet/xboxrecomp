"""tools.seed_from_log's report: where an observed address sits among the
known functions. One at a function's start is that function, not a new one
(the old test was a strict start < va < end, which called it "new")."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.seed_from_log.__main__ import where_in  # noqa: E402

BOUNDS = [(0x1000, 0x1040, "sub_00001000"), (0x2000, 0x2010, "sub_00002000")]
STARTS = [b[0] for b in BOUNDS]


class WhereInTest(unittest.TestCase):
    def test_start_is_known(self):
        self.assertEqual(where_in(BOUNDS, STARTS, 0x1000), ("known", "sub_00001000"))
        self.assertEqual(where_in(BOUNDS, STARTS, 0x2000), ("known", "sub_00002000"))

    def test_inside_is_alias(self):
        self.assertEqual(where_in(BOUNDS, STARTS, 0x1001), ("alias", "sub_00001000"))
        self.assertEqual(where_in(BOUNDS, STARTS, 0x200F), ("alias", "sub_00002000"))

    def test_outside_is_new(self):
        self.assertEqual(where_in(BOUNDS, STARTS, 0x0FFF), ("new", None))
        self.assertEqual(where_in(BOUNDS, STARTS, 0x1040), ("new", None))
        self.assertEqual(where_in(BOUNDS, STARTS, 0x3000), ("new", None))
        self.assertEqual(where_in([], [], 0x1000), ("new", None))


if __name__ == "__main__":
    unittest.main()
