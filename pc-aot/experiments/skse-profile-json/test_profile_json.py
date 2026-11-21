"""Exercise the private profile JSON store using authored scratch files."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class ProfileJsonTests(unittest.TestCase):
    @unittest.skipIf(sys.platform == "win32", "The iOS backend uses POSIX relative descriptors.")
    def test_profile_grants_and_atomic_publication(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("A C compiler is required.")
        args = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread"]
        if sys.platform.startswith("linux"):
            args.extend([
                "-DSKSE_PROFILE_TEST_FAULTS=1",
                "-Wl,--wrap=write,--wrap=fsync,--wrap=renameat",
            ])
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                args.extend(["-fno-pie", "-no-pie"])
        with tempfile.TemporaryDirectory(prefix="skse-profile-json-") as scratch:
            directory = Path(scratch)
            binary = directory / "test-profile-json"
            subprocess.run([
                *args, *map(str, (HERE / "test_profile_json.c", HERE / "profile_json.c", HERE / "profile_overlay.c",
                                 HERE.parent / "skse-plugin-vm" / "x86_winfile.c")), "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary), str(directory)], check=True, timeout=60)


    @unittest.skipUnless(sys.platform == "win32", "Calls the native Win32 APIs for comparison.")
    def test_virtual_overlay_matches_windows(self):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        llvm = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin")) / "clang.exe"
        if not compiler and llvm.is_file():
            compiler = str(llvm)
        if not compiler:
            self.skipTest("Windows Clang is required.")
        with tempfile.TemporaryDirectory(prefix="skse-profile-win32-") as scratch:
            binary = Path(scratch) / "test-profile-vfs-win.exe"
            subprocess.run([
                compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fuse-ld=lld",
                str(HERE / "test_profile_vfs_windows.c"), str(HERE.parent / "skse-plugin-vm" / "x86_winfile.c"),
                "-o", str(binary),
            ], check=True, timeout=60)
            test_env = dict(os.environ, SKYRIM_PROFILE_WIN_TEST_ROOT=scratch)
            subprocess.run([str(binary)], env=test_env, check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
