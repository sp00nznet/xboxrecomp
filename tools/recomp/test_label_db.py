"""String-reference labels never name code (see translator.load_label_db)."""
import json
import os
import tempfile
import unittest

from tools.recomp.translator import load_label_db


class LabelDbTest(unittest.TestCase):
    def test_string_refs_are_not_names(self):
        labels = [
            {"address": "0x00264DC4", "name": "str_MAIN_L", "type": "string_ref"},
            {"address": "0x00264FD8", "name": "str_MAIN_L", "type": "string_ref"},
            {"address": "0x00010400", "name": "KeTickCount", "type": "kernel_import"},
        ]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "labels.json")
            with open(path, "w") as f:
                json.dump(labels, f)
            db = load_label_db(path)
        self.assertEqual(db, {0x00010400: "KeTickCount"})

    def test_missing_file_is_empty(self):
        self.assertEqual(load_label_db(None), {})
        self.assertEqual(load_label_db("no/such/labels.json"), {})


if __name__ == "__main__":
    unittest.main()
