"""Run the authored host scenarios for the Classic high-heels height model."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class ClassicHighHeelHeightTest(unittest.TestCase):
    def test_height_transitions(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler unavailable")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "highheel_height"
            subprocess.run(
                [cc, *flags, HERE / "highheel_height.c", HERE / "test_highheel_height.c", "-o", binary],
                check=True, timeout=60,
            )
            result = subprocess.check_output([binary], text=True, timeout=10)
            self.assertEqual(result.strip(), "6 Classic HighHeels host scenarios passed")


if __name__ == "__main__":
    unittest.main()
