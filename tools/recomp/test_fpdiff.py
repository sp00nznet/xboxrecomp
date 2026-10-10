"""The lifted C must agree with the CPU on the x87/SSE/MMX forms below.

Runs tools.recomp.fpdiff on DOA3's hot forms and on every form a fix in the
lifter made agree. Skipped without a 32-bit MSVC, like tools.conformance.
"""

import tempfile
import unittest

from tools.conformance import __main__ as runner
from . import fpdiff

# Case-name prefixes; fpdiff.cases() holds the snippets.
CASES = (
    # DOA3's dominant forms: m32 arithmetic and fcomp/fnstsw/test ah/jcc.
    "fadd_m32", "fmul_m32", "fsub_m32", "fdivr_m32", "fld_st", "fstp_st",
    "fxch_st", "fcom_m32_t", "fcomp_st1_t", "seq_fs", "seq_fl", "seq_mov",
    "seq_and", "seq_two", "seq_sahf", "seq_wait",
    "mulps_", "addps_", "shufps_", "paddw_", "psraw_", "pextrw_",
    # EFLAGS from sahf, fcomi and comiss, read by every jcc/setcc/cmovcc.
    "fcomp_m32_t", "fcomi", "fucomi", "comiss", "ucomiss", "fcom_st1_sahf",
    "fcomp_m32_sahf", "fcompp_sahf", "fucompp_sahf", "ftst_sahf",
    # fld/fstp tbyte, as the CRT loads its constants.
    "fld_m80",
    # ficom/ficomp compare st0 with an integer and ficomp pops.
    "ficom",
    # Trig range, remainder, scale and exp/log edge cases; fisttp chops.
    "fsin", "fcos", "fptan", "f2xm1", "fyl2xp1", "fprem", "fscale", "fisttp",
    "seq_fptan",
    # SSE conversions under MXCSR, rcp/rsqrt estimates, SSE2 scalar doubles.
    "cvts", "cvtt", "cvtps", "cvtpi", "rcp", "rsqrt", "addsd", "subsd",
    "mulsd", "divsd", "minsd", "maxsd", "sqrtsd", "comisd", "ucomisd",
)


class FpdiffTest(unittest.TestCase):
    def test_lifted_fp_matches_the_cpu(self):
        if runner._find_vcvars() is None:
            self.skipTest("needs a 32-bit MSVC (vcvars32.bat)")
        corpus = [c for c in fpdiff.cases() if c["name"].startswith(CASES)]
        with tempfile.TemporaryDirectory() as out:
            result = fpdiff.run(out, corpus)
        failed = [l for l in result.stdout.splitlines()
                  if l.startswith("RESULT")
                  and (l.split()[2] != "0" or l.endswith("UNSUPPORTED"))]
        self.assertEqual(result.returncode, 0, "\n".join(failed[:40]))


if __name__ == "__main__":
    unittest.main()
