"""RenderWare ID parsing must not depend on one reference build's version."""
import unittest
from .rw_identifier import identify_rw_functions

class RenderWareVersionTest(unittest.TestCase):
    def classify(self, tag, namespace="RenderWare"):
        text = "@@(#)$Id: //" + namespace + "/" + tag + "/rwsdk/src/plcore/core.c#3 $"
        return identify_rw_functions(
            [{"address": "0x2000", "string": text}], {0x2000: [0x1000]},
            [{"start": "0x1000"}])

    def test_numeric_version_tags_preserve_classification(self):
        for tag in ("RW36Active", "RW37Active"):
            with self.subTest(tag=tag):
                results, modules = self.classify(tag)
                self.assertEqual(results[0x1000]["category"], "rw_plcore")
                self.assertEqual(results[0x1000]["method"], "rw_string_ref")
                self.assertEqual(modules["core.c"]["functions"], [0x1000])

    def test_unrelated_or_malformed_tags_are_not_seeds(self):
        for tag, namespace in (("RWfooActive", "RenderWare"),
                               ("RW37ActiveExtra", "RenderWare"),
                               ("RW37Active", "Other")):
            with self.subTest(tag=tag, namespace=namespace):
                self.assertEqual(self.classify(tag, namespace), ({}, {}))

if __name__ == "__main__":
    unittest.main()
