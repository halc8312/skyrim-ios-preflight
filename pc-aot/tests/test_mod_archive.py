"""Real MOD archive reader, data-root detection and launch view; authored files only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class ModArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler required; run on WSL/Mac")
        cls.tmp = tempfile.TemporaryDirectory(prefix="mod-archive-")
        cls.dir = Path(cls.tmp.name)
        cls.bin = cls.dir / "driver"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, "-I", str(PORT), str(PORT / "mod_archive.c"), str(PORT / "mod_deploy.c"),
                        str(PORT / "mod_plugins.c"), str(ROOT / "tests/native/test_mod_archive.c"),
                        "-lz", "-o", str(cls.bin)], check=True, timeout=120)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_driver(self, *args):
        out = subprocess.run([str(self.bin), *map(str, args)], check=True, text=True, capture_output=True, timeout=60)
        return out.stdout

    def make_zip(self, name, files, **kw):
        path = self.dir / name
        with zipfile.ZipFile(path, "w", compression=kw.get("compression", zipfile.ZIP_DEFLATED)) as z:
            for arc, data in files.items():
                if isinstance(data, zipfile.ZipInfo):
                    z.writestr(data, b"x")
                else:
                    z.writestr(arc, data)
        return path

    def test_extract_with_wrapper_folder_and_crc(self):
        big = os.urandom(300_000) + b"a" * 400_000
        z = self.make_zip("wrapped.zip", {
            "Cool Mod v1/Data/textures/Armor/A.dds": big,
            "Cool Mod v1/Data/CoolMod.esp": b"TES4",
            "Cool Mod v1/Data/CoolMod.bsa": b"",
            "Cool Mod v1/readme.txt": b"hi",
        })
        self.assertIn("root 1 [Cool Mod v1/Data/] installer 0", self.run_driver("root", z))
        dest = self.dir / "out1"; dest.mkdir()
        out = self.run_driver("extract", z, dest)
        self.assertIn("extract textures/Armor/A.dds ok", out)
        self.assertEqual((dest / "textures/Armor/A.dds").read_bytes(), big)
        self.assertFalse((dest / "readme.txt").exists())
        # Existing files are never replaced.
        self.assertIn("extract CoolMod.esp write", self.run_driver("extract", z, dest))

    def test_stored_zip64_and_installer_detection(self):
        path = self.dir / "z64.zip"
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
            with z.open("meshes/x.nif", "w", force_zip64=True) as f:
                f.write(b"nif" * 1000)
            z.writestr("fomod/ModuleConfig.xml", "<config/>")
        self.assertIn("root 1 [] installer 1", self.run_driver("root", path))
        dest = self.dir / "out2"; dest.mkdir()
        self.assertIn("extract meshes/x.nif ok", self.run_driver("extract", path, dest))

    def test_unsafe_names_are_refused(self):
        for i, name in enumerate(["../evil.esp", "/abs.esp", "C:/x.esp", "a/../../b.esp", "con?.esp"]):
            z = self.make_zip(f"bad{i}.zip", {name: b"x"})
            self.assertIn("status unsafe-path", self.run_driver("list", z), name)
        z = self.make_zip("dup.zip", {"Textures/a.dds": b"1", "textures/A.DDS": b"2"})
        self.assertIn("status duplicate", self.run_driver("list", z))
        info = zipfile.ZipInfo("link.esp"); info.create_system = 3; info.external_attr = (0o120777 << 16)
        z = self.make_zip("link.zip", {"link.esp": info})
        self.assertIn("status link", self.run_driver("list", z))
        z = self.dir / "bz.zip"
        with zipfile.ZipFile(z, "w", compression=zipfile.ZIP_BZIP2) as f:
            f.writestr("a.esp", b"x" * 100)
        self.assertIn("status unsupported-method", self.run_driver("list", z))
        (self.dir / "notzip.zip").write_bytes(b"hello world, not a zip file at all")
        self.assertIn("status not-zip", self.run_driver("list", self.dir / "notzip.zip"))

    def test_backslashes_and_non_utf8_names(self):
        path = self.dir / "names.zip"
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("Data\\Textures\\b.dds", b"1")
            info = zipfile.ZipInfo("x.txt"); z.writestr(info, b"2")
        raw = path.read_bytes().replace(b"x.txt", b"\x93\xfa.tx")  # Shift-JIS bytes, no UTF-8 flag
        path.write_bytes(raw)
        out = self.run_driver("list", path)
        self.assertIn("status ok", out)
        self.assertIn("skipped 1", out)
        self.assertIn("file Data/Textures/b.dds", out)

    def test_no_game_data(self):
        z = self.make_zip("docs.zip", {"readme.txt": b"x", "images/a.png": b"y"})
        self.assertIn("root 0", self.run_driver("root", z))

    def test_deploy_overlay_priority_protection_and_reuse(self):
        game = self.dir / "game"; (game / "Data/Textures").mkdir(parents=True)
        (game / "Data/Skyrim.esm").write_bytes(b"orig-esm")
        (game / "Data/Textures/Sky.dds").write_bytes(b"orig-sky")
        (game / "TESV_unpacked.exe").write_bytes(b"exe")
        a = self.dir / "pkgA"; (a / "textures").mkdir(parents=True)
        (a / "textures/sky.DDS").write_bytes(b"A-sky")
        (a / "textures/new.dds").write_bytes(b"A-new")
        (a / "skyrim.esm").write_bytes(b"evil")
        b = self.dir / "pkgB"; (b / "TEXTURES").mkdir(parents=True)
        (b / "TEXTURES/new.dds").write_bytes(b"B-new")
        (b / "B.esp").write_bytes(b"TES4")
        view = self.dir / "view"; conflicts = self.dir / "conflicts.tsv"
        out = self.run_driver("deploy", game, view, conflicts, "key-1", a, b)
        self.assertIn("deploy 0  matches 1", out)
        self.assertIn("signature-stable 1", out)
        self.assertEqual((view / "Data/Textures/Sky.dds").read_bytes(), b"A-sky")
        self.assertEqual((view / "Data/Textures/new.dds").read_bytes(), b"B-new")
        self.assertEqual((view / "Data/Skyrim.esm").read_bytes(), b"orig-esm")
        self.assertEqual((view / "Data/B.esp").read_bytes(), b"TES4")
        self.assertFalse((view / "Data/textures").exists() and (view / "Data/Textures").exists()
                         and not os.path.samefile(view / "Data/textures", view / "Data/Textures"))
        self.assertEqual((game / "Data/Textures/Sky.dds").read_bytes(), b"orig-sky")
        rows = conflicts.read_text().splitlines()
        self.assertIn("Textures/Sky.dds\t-1\t0", rows)
        self.assertIn("Textures/new.dds\t0\t1", rows)
        self.assertIn("Skyrim.esm\t-1\t0", rows)
        # Rebuild with fewer packages replaces the view atomically.
        out = self.run_driver("deploy", game, view, conflicts, "key-2")
        self.assertIn("deploy 0  matches 1", out)
        self.assertEqual((view / "Data/Textures/Sky.dds").read_bytes(), b"orig-sky")
        self.assertFalse((view / "Data/B.esp").exists())
        self.assertFalse(Path(str(view) + ".old").exists())


if __name__ == "__main__":
    unittest.main()
