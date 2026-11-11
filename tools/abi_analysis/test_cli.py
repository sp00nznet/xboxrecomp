"""ABI output paths work with synthetic caller-owned inputs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from tools.conformance import mkxbe

class AbiCliTest(unittest.TestCase):
    def test_output_basename_and_nested_directory(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            (work / "input.xbe").write_bytes(mkxbe.build())
            functions = []
            va = mkxbe.TEXT_VA
            for code in mkxbe.FUNCS:
                functions.append({"start": hex(va), "size": len(code), "has_prologue": True})
                va += (len(code) + 15) & ~15
            (work / "functions.json").write_text(json.dumps(functions), encoding="utf-8")
            (work / "identified.json").write_text("[]", encoding="utf-8")
            for output in ("abi.json", "nested/abi.json"):
                with self.subTest(output=output):
                    run = subprocess.run([
                        sys.executable, "-m", "tools.abi_analysis", "input.xbe",
                        "--functions", "functions.json", "--identified", "identified.json",
                        "--output", output], cwd=work, capture_output=True, text=True,
                        env={**os.environ, "PYTHONPATH": str(root)})
                    self.assertEqual(run.returncode, 0, run.stderr)
                    result = json.loads((work / output).read_text())
                    self.assertEqual([row["address"] for row in result],
                                     [f"{int(f['start'], 16):#010x}" for f in functions])

if __name__ == "__main__":
    unittest.main()
