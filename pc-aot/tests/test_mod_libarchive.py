"""7z/RAR/RAR5 import with the zip path rules; authored archives only."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

from mod_archive_fixtures import rar4_stored, rar5_stored, seven_zip

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class ModLibarchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        prefix = os.environ.get("SKYRIM_MOD_ARCHIVE_TEST_PREFIX")
        if not cc:
            if prefix:
                raise RuntimeError("The pinned archive validation requires a C compiler")
            raise unittest.SkipTest("C compiler required")
        cls.tmp = tempfile.TemporaryDirectory(prefix="mod-la-")
        cls.dir = Path(cls.tmp.name)
        cls.bin = cls.dir / "driver"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        extra = shlex.split(os.environ.get("LIBARCHIVE_FLAGS", ""))
        libraries = ["-larchive"]
        if prefix:
            prefix = Path(prefix)
            extra = ["-I", str(prefix / "include")]
            libraries = [str(prefix / "lib/libarchive.a"), str(prefix / "lib/liblzma.a")]
        if sys.platform == "darwin":
            libraries.append("-liconv")
        try:
            subprocess.run([cc, *flags, "-I", str(PORT), *extra, str(PORT / "mod_libarchive.c"), str(PORT / "mod_archive.c"),
                            str(PORT / "mod_deploy.c"), str(PORT / "mod_plugins.c"), str(ROOT / "tests/native/test_mod_libarchive.c"),
                            *libraries, "-lz", "-o", str(cls.bin)], check=True, timeout=120, capture_output=True)
        except subprocess.CalledProcessError as e:
            if prefix:
                raise RuntimeError(e.stderr.decode(errors="replace")) from e
            raise unittest.SkipTest("libarchive development files required: " + e.stderr.decode()[-300:])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def drive(self, *args):
        return subprocess.run([str(self.bin), *map(str, args)], check=True, text=True, capture_output=True, timeout=60).stdout

    def seven_zip(self, name, tree):
        out = self.dir / name
        out.write_bytes(seven_zip(tree))
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
        dup = self.seven_zip("dup.7z", {"Textures/a.dds": b"1", "textures/A.dds": b"2"})
        self.assertIn("status duplicate", self.drive("list", dup))
        bad = self.seven_zip("bad.7z", {"../x.esp": b"1"})
        self.assertIn("status unsafe-path", self.drive("list", bad))

    def test_stored_rar_and_rar5_extract_exact_bytes(self):
        data = bytes(range(251)) * 17
        for label, make in (("rar4", rar4_stored), ("rar5", rar5_stored)):
            with self.subTest(label=label):
                archive = self.dir / (label + ".rar")
                archive.write_bytes(make("Data/textures/sample.dds", data))
                dest = self.dir / label
                dest.mkdir()
                self.assertIn("root 1 [Data/] installer 0", self.drive("list", archive))
                self.assertIn("extract ok files 1", self.drive("extract", archive, dest))
                self.assertEqual((dest / "textures/sample.dds").read_bytes(), data)

    def test_libarchive_reader_refuses_zip(self):
        archive = self.dir / "zip-disguised-as.7z"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("Data/textures/sample.dds", b"authored")
        self.assertIn("status not-zip", self.drive("list", archive))


if __name__ == "__main__":
    unittest.main()
