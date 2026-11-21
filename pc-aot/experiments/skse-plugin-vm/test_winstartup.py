#!/usr/bin/env python3
"""Check the guest model and the native PE32 detached-GUI startup reference."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class GuestStartupTests(unittest.TestCase):
    def test_guest_startup(self):
        windows = sys.platform == "win32"
        llvm = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin")) / "clang.exe"
        cc = shutil.which(os.environ.get("CC", "cc"))
        if windows and llvm.is_file():
            cc = str(llvm)
        if not cc:
            self.skipTest("A C compiler is required.")
        flags = [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2"]
        if windows:
            flags.append("-fuse-ld=lld")
        sanitizer = []
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            sanitizer.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                sanitizer.extend(["-fno-pie", "-no-pie"])
        sources = [str(HERE / name) for name in ("x86_winthread.c", "test_winstartup.c")]
        with tempfile.TemporaryDirectory(prefix="skse-startup-") as scratch:
            exe = Path(scratch) / ("startup.exe" if windows else "startup")
            subprocess.run([*flags, *sanitizer, *sources, "-o", str(exe)], check=True, timeout=60)
            subprocess.run([str(exe)], check=True, timeout=30)
            if windows:
                reference = Path(scratch) / "startup-reference.exe"
                subprocess.run([*flags, "--target=i686-pc-windows-msvc", "-DSKSE_WIN_STARTUP_NATIVE_CHILD",
                                "-Wl,/subsystem:windows", *sources, "-o", str(reference)],
                               check=True, timeout=60)
                empty_file = Path(scratch) / "authored-console-file.bin"
                empty_file.touch()
                environment = os.environ.copy()
                environment["SKYRIM_WINCONSOLE_TEST_FILE"] = str(empty_file)
                subprocess.run([str(reference)], check=True, timeout=30, close_fds=True,
                               creationflags=subprocess.DETACHED_PROCESS, env=environment)
                print("Detached Windows i386 GUI startup and console references matched.")


if __name__ == "__main__":
    unittest.main()
