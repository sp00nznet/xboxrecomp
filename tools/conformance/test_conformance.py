"""Runs the differential conformance suite as part of `pytest tools/`.

Skipped where it cannot run rather than failed, so the rest of the suite stays
meaningful everywhere. Two things can satisfy it:

  - a 32-bit MSVC (vcvars32.bat), which is the native Windows path; or
  - XBOXRECOMP_PYTEST_DOCKER=1, which lets it use the linux/386 containers.

The container backend is opt-in rather than automatic because `pytest tools/`
should stay fast and hermetic -- building and running containers is far too
heavy a side effect for a unit-test pass.
"""

import os
import unittest

from . import __main__ as conformance

_ALLOW_DOCKER = os.environ.get("XBOXRECOMP_PYTEST_DOCKER") == "1"


class ConformanceTest(unittest.TestCase):
    def test_lifted_code_matches_the_cpu(self):
        if conformance._find_vcvars() is None:
            if not _ALLOW_DOCKER:
                self.skipTest(
                    "needs a 32-bit MSVC (vcvars32.bat) to assemble and build "
                    "the harness, or XBOXRECOMP_PYTEST_DOCKER=1 to use the "
                    "containers instead")
            if not conformance._docker_image_present():
                # Opting in without the image is a setup problem, not a lifter
                # bug -- failing here would point the reader at the wrong thing.
                self.skipTest(
                    "XBOXRECOMP_PYTEST_DOCKER=1 but the linux/386 image is not "
                    "built; run `bash tools/macos/setup.sh --test`")

        rc = conformance.main_with_args([], allow_container=_ALLOW_DOCKER)
        self.assertEqual(rc, 0, "the lifted C disagreed with the CPU; run "
                                "`py -3 -m tools.conformance` for the detail")


if __name__ == "__main__":
    unittest.main()
