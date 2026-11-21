"""Detect first-wave MOD features by marker names in ordered Data roots (authored files)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


@unittest.skipUnless(os.name == "posix", "Requires POSIX paths")
class FeatureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "features"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        vm = ROOT / "experiments/skse-plugin-vm"
        subprocess.run([cc, *flags, "-I", vm, ROOT / "tests/native/test_mod_features.c", PORT / "mod_features.c",
                        PORT / "mod_plugins.c", vm / "pe_image.c", "-o", cls.binary], check=True, timeout=60)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = Path(tmp.name)

    def root(self, name, files):
        base = self.top / name
        base.mkdir()
        for path in files:
            target = base / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"x")
        return base

    def scan(self, *roots):
        out = subprocess.check_output([self.binary, *roots], text=True, timeout=10).split()
        return int(out[0]), {item.split(":")[0]: tuple(map(int, item.split(":")[1:])) for item in out[1:]}

    def test_duplicate_nioverride_reports_each_pe_version(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("pe_fixture", ROOT / "experiments/skse-plugin-vm/test_pe_image.py")
        pe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pe)
        racemenu = self.root("racemenu", ["SKSE/Plugins/chargen.dll"])
        (racemenu / "SKSE/Plugins/nioverride.dll").write_bytes(pe.add_version(pe.make_image(), (3, 4, 5, 0)))
        standalone = self.root("nioverride", [])
        (standalone / "SKSE/Plugins").mkdir(parents=True)
        (standalone / "SKSE/Plugins/NIOverride.dll").write_bytes(pe.add_version(pe.make_image(), (3, 4, 4, 0)))
        plain = self.root("plain", ["SKSE/Plugins/nioverride.dll"])       # no version resource
        none = self.root("none", ["Meshes/x.nif"])
        twice = self.root("twice", ["SKSE/Plugins/nioverride.dll", "SKSE/Plugins/nioverride_old.dll"])
        env = dict(os.environ, FEATURE_VERSION="nioverride")
        out = subprocess.check_output([self.binary, racemenu, standalone, plain, none, twice], text=True,
                                      timeout=10, env=env).splitlines()
        self.assertIn("nioverride:4:4", out[0])   # four packages provide it; the last one wins
        import errno
        self.assertEqual(out[1:], ["0:3.4.5.0", "0:3.4.4.0", f"{errno.EIO}:0.0.0.0",
                                   f"{errno.ENOENT}:0.0.0.0", f"{errno.EMLINK}:0.0.0.0"])

    def test_detection_by_marker_names_ignores_case_and_spacing(self):
        a = self.root("a", ["SKSE/Plugins/Fuz Ro D-oh.DLL", "SKSE/Plugins/StorageUtil.dll",
                            "SKSE/Plugins/ShowRaceMenuPrecacheKiller.dll", "SKSE/Plugins/hdtPhysicsExtensions.dll",
                            "SKSE/Plugins/hdtHighHeels.dll", "SKSE/Plugins/SKSE_EnhancedCamera.dll",
                            "SKSE/Plugins/SafetyLoad.dll", "SKSE/Plugins/chargen.dll", "SKSE/Plugins/nioverride.dll",
                            "enhancedcharacteredit.ESP", "SKSE/Plugins/unrelated.dll"])
        unreadable, found = self.scan(a)
        self.assertEqual(unreadable, 0)
        self.assertEqual(found, {key: (1, 0) for key in (
            "fuz", "papyrusutil", "precache", "racemenu", "nioverride", "ece", "enhancedcamera",
            "safetyload", "hdtphysics", "hdthighheels")})

    def test_non_dll_files_and_wrong_folders_do_not_match(self):
        a = self.root("a", ["SKSE/Plugins/fuz.ini", "SKSE/Plugins/fuz.dll.txt", "Plugins/fuz.dll",
                            "SKSE/fuz.dll", "SKSE/Plugins/sub/fuz.dll", "ece.esp"])
        self.assertEqual(self.scan(a), (0, {}))
        self.assertEqual(self.scan(), (0, {}))
        self.assertEqual(self.scan(str(self.top / "missing")), (0, {}))

    def test_duplicate_providers_report_the_later_root(self):
        a = self.root("a", ["SKSE/Plugins/nioverride.dll"])
        b = self.root("b", ["SKSE/Plugins/fuz.dll"])
        c = self.root("c", ["SKSE/Plugins/NiOverride.dll"])
        self.assertEqual(self.scan(a, b, c), (0, {"nioverride": (2, 2), "fuz": (1, 1)}))

    def test_links_and_ambiguous_aliases_are_skipped(self):
        real = self.root("real", ["SKSE/Plugins/fuz.dll"])
        linked = self.root("linked", [])
        (linked / "SKSE").symlink_to(real / "SKSE")
        alias = self.root("alias", ["SKSE/Plugins/fuz.dll", "skse/plugins/fuz.dll"])
        dll_link = self.root("dll_link", [])
        (dll_link / "SKSE/Plugins").mkdir(parents=True)
        (dll_link / "SKSE/Plugins/fuz.dll").symlink_to(real / "SKSE/Plugins/fuz.dll")
        unreadable, found = self.scan(linked, alias, dll_link)
        self.assertEqual((unreadable, found), (2, {}))


if __name__ == "__main__":
    unittest.main()
