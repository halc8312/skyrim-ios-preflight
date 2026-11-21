"""Check SSE2 format conversions against native instructions with default MXCSR."""

import os
from pathlib import Path
import platform
import shutil
import sys
import tempfile
import unittest

from test_skse_plugin_vm import HERE, LLVM, compile_interpreter, run


class SseConversionTests(unittest.TestCase):
    def test_sse_conversion_and_bounds(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if sys.platform == "win32" and not compiler and (LLVM / "clang.exe").is_file():
            compiler = LLVM / "clang.exe"
        if not compiler:
            self.skipTest("C compiler required")
        flags = []
        if sys.platform == "win32":
            flags += ["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]

        with tempfile.TemporaryDirectory(prefix="skse-sse-") as scratch:
            suffix = ".exe" if sys.platform == "win32" else ""
            guest = Path(scratch) / f"guest{suffix}"
            source = "test_x86_sse_conversion.c"
            compile_interpreter(compiler, guest, source, flags)
            actual = run(guest, capture_output=True, text=True).stdout.splitlines()
            for instruction in ("CVTSS2SD", "CVTSD2SS", "CVTPD2PS"):
                for form in ("reg", "mem"):
                    self.assertTrue(any(row.startswith(f"{instruction} {form} ") for row in actual))
            if sys.platform == "win32":
                native_flags = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld"]
            elif platform.machine().lower() in {"x86_64", "amd64"}:
                native_flags = []
            else:
                # The guest still verifies memory access widths and exact known results here.
                return
            reference = Path(scratch) / f"reference{suffix}"
            run(compiler, *native_flags, "-std=c11", "-msse2", "-O2", "-DSKSE_NATIVE_SSE_REFERENCE",
                HERE / source, "-o", reference)
            expected = run(reference, capture_output=True, text=True).stdout.splitlines()
            self.assertEqual(len(actual), len(expected))
            for native, interpreted in zip(expected, actual):
                self.assertEqual(interpreted, native)


if __name__ == "__main__":
    unittest.main()
