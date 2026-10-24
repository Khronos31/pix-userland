"""Run packaging checks from the same source copy as portable/Windows legacy tests."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LegacySourceCopyTest(unittest.TestCase):
    def test_packaging_from_build_script_source_copy(self):
        with tempfile.TemporaryDirectory(prefix="pix-legacy-copy-") as temporary:
            subprocess.run(["sh", str(ROOT / "tools/copy-legacy-test-source.sh"),
                            str(ROOT), temporary], check=True)
            subprocess.run([sys.executable, "tests/test_dt295_packaging.py"],
                           cwd=temporary, check=True)


if __name__ == "__main__":
    unittest.main()
