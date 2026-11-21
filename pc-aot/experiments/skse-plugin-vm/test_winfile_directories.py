#!/usr/bin/env python3
"""Run the virtual-directory tests and native Windows API comparisons."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class GuestVirtualDirectoryTests(unittest.TestCase):
    def test_virtual_directories(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if sys.platform == "win32" and not cc:
            llvm = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin"))
            if (llvm / "clang.exe").is_file():
                cc = str(llvm / "clang.exe")
        if not cc:
            self.skipTest("A C compiler is required.")
        args = [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2"]
        if sys.platform == "win32":
            args.append("-fuse-ld=lld")
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                args.extend(["-fno-pie", "-no-pie"])
        with tempfile.TemporaryDirectory(prefix="skse-winfile-") as scratch:
            executable = Path(scratch) / ("winfile.exe" if sys.platform == "win32" else "winfile")
            subprocess.run([*args, str(HERE / "x86_winfile.c"), str(HERE / "test_winfile_directories.c"),
                            "-o", str(executable)], check=True, timeout=60)
            environment = os.environ.copy()
            if sys.platform == "win32":
                native_root = Path(scratch) / "reference"
                native_root.mkdir()
                environment["SKYRIM_WINFILE_TEST_ROOT"] = str(native_root)
            subprocess.run([str(executable)], env=environment, check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
