"""Check cold guest environment state and compare with a private Windows block."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_pe_image import find_tool

HERE = Path(__file__).resolve().parent


class GuestEnvironmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = find_tool(os.environ.get("CC", "cc"), "clang", "clang-18")
        if not cc:
            raise unittest.SkipTest("A C compiler is required.")
        args = [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2"]
        if sys.platform == "win32":
            args.append("-fuse-ld=lld")
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                args.extend(["-fno-pie", "-no-pie"])
        scratch = tempfile.TemporaryDirectory(prefix="skse-env-")
        cls.addClassCleanup(scratch.cleanup)
        cls.binary = Path(scratch.name) / ("winenv.exe" if sys.platform == "win32" else "winenv")
        sources = ("x86_winenv.c", "x86_wintext.c", "test_winenv.c")
        subprocess.run([*args, *(str(HERE / name) for name in sources), "-o", str(cls.binary)],
                       check=True, timeout=60)

    def run_case(self, case):
        subprocess.run([self.binary, case], check=True, timeout=10)

    def test_utf16_snapshot_lifetime_case_order_and_deletion(self):
        self.run_case("snapshot")

    def test_bounded_input_count_size_and_undersized_destination(self):
        self.run_case("limits")

    @unittest.skipUnless(sys.platform == "win32", "Native private Windows environment reference")
    def test_native_windows_private_block(self):
        self.run_case("windows")


if __name__ == "__main__":
    unittest.main()
