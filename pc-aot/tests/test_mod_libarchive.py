"""7z (and other libarchive formats) import with the zip path rules; authored archives only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class ModLibarchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        seven = shutil.which("7z") or shutil.which("7zz")
        if not cc or not seven:
            raise unittest.SkipTest("C compiler and 7z required")
        cls.seven = seven
        cls.tmp = tempfile.TemporaryDirectory(prefix="mod-la-")
        cls.dir = Path(cls.tmp.name)
        cls.bin = cls.dir / "driver"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        extra = os.environ.get("LIBARCHIVE_FLAGS", "").split()
        try:
            subprocess.run([cc, *flags, "-I", str(PORT), *extra, str(PORT / "mod_libarchive.c"), str(PORT / "mod_archive.c"),
                            str(PORT / "mod_deploy.c"), str(PORT / "mod_plugins.c"), str(ROOT / "tests/native/test_mod_libarchive.c"),
                            "-larchive", "-lz", "-o", str(cls.bin)], check=True, timeout=120, capture_output=True)
        except subprocess.CalledProcessError as e:
            raise unittest.SkipTest("libarchive development files required: " + e.stderr.decode()[-300:])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def drive(self, *args):
        return subprocess.run([str(self.bin), *map(str, args)], check=True, text=True, capture_output=True, timeout=60).stdout

    def seven_zip(self, name, tree):
        src = self.dir / (name + ".src")
        for rel, data in tree.items():
            p = src / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        out = self.dir / name
        subprocess.run([self.seven, "a", "-t7z", "-mx=5", str(out), "."], cwd=src, check=True, capture_output=True)
        return out

    def test_7z_with_wrapper_folder(self):
        big = os.urandom(200_000) + b"z" * 300_000
        archive = self.seven_zip("m.7z", {"My Mod/Data/textures/a.dds": big, "My Mod/Data/My.esp": b"TES4",
                                          "My Mod/fomod/info.xml": b"<x/>"})
        out = self.drive("list", archive)
        self.assertIn("status ok count 3", out)
        self.assertIn("root 1 [My Mod/Data/] installer 0", out)
        dest = self.dir / "out"; dest.mkdir()
        self.assertIn("extract ok files 2", self.drive("extract", archive, dest))
        self.assertEqual((dest / "textures/a.dds").read_bytes(), big)

    def test_installer_and_duplicates(self):
        archive = self.seven_zip("f.7z", {"fomod/ModuleConfig.xml": b"<config/>", "00 Core/meshes/a.nif": b"x"})
        self.assertIn("installer 1", self.drive("list", archive))
        dup = self.dir / "dup.zip"
        import zipfile
        with zipfile.ZipFile(dup, "w") as z:
            z.writestr("Textures/a.dds", b"1"); z.writestr("textures/A.dds", b"2")
        self.assertIn("status duplicate", self.drive("list", dup))
        bad = self.dir / "bad.zip"
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("../x.esp", b"1")
        self.assertIn("status unsafe-path", self.drive("list", bad))


if __name__ == "__main__":
    unittest.main()
