"""Check SkyUI's SKSE script requirements across ordered Data roots with authored files."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
VM = ROOT / "experiments/skse-plugin-vm"
sys.path.insert(0, str(VM))
from test_classic_pex import STRINGS, script  # noqa: E402

OK, NO_SKYUI, BAD_ARCHIVE, NO_SCRIPT, BAD_SCRIPT, UNREADABLE = range(6)


def skse_pex(release=True):
    names = list(STRINGS)
    names[1] = "SKSE"
    names[3] = "GetVersionRelease" if release else "Other"
    return script(strings=tuple(names))


def bsa(version=104):
    return b"BSA\0" + version.to_bytes(4, "little") + bytes(16)


@unittest.skipUnless(os.name == "posix", "Requires POSIX paths")
class RequirementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "requirements"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT, "-I", VM]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_requirements.c", PORT / "mod_requirements.c",
                        PORT / "mod_plugins.c", VM / "classic_pex.c", "-o", cls.binary], check=True, timeout=60)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = Path(tmp.name)

    def root(self, name, files):
        base = self.top / name
        base.mkdir()
        for path, content in files.items():
            target = base / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        return base

    def check(self, *roots):
        out = subprocess.check_output([self.binary, *roots], text=True, timeout=10).split()
        return [int(v) for v in out]

    def skyui(self, **extra):
        return {"SkyUI.esp": b"esp", "SkyUI.bsa": bsa(), **extra}

    def test_complete_set_and_support_script_count(self):
        support = {f"Scripts/{n}.pex": b"x" for n in ("UI", "UICallback", "Input")}
        skyui = self.root("skyui", self.skyui())
        skse = self.root("skse", {"Scripts/SKSE.pex": skse_pex(), **support})
        self.assertEqual(self.check(skyui, skse), [OK, 3, 4])

    def test_absent_skyui_missing_or_invalid_scripts(self):
        game = self.root("game", {})
        self.assertEqual(self.check(game)[0], NO_SKYUI)
        self.assertEqual(self.check()[0], NO_SKYUI)
        skyui = self.root("skyui", self.skyui())
        self.assertEqual(self.check(game, skyui)[0], NO_SCRIPT)
        bad = self.root("bad", {"Scripts/SKSE.pex": b"not a script"})
        self.assertEqual(self.check(game, skyui, bad)[0], BAD_SCRIPT)
        other = self.root("other", {"Scripts/SKSE.pex": skse_pex(release=False)})
        self.assertEqual(self.check(game, skyui, other)[0], BAD_SCRIPT)
        big = self.root("big", {"Scripts/SKSE.pex": skse_pex() + bytes(8 << 20)})
        self.assertEqual(self.check(game, skyui, big)[0], BAD_SCRIPT)

    def test_later_root_wins_and_case_is_ignored(self):
        skyui = self.root("skyui", {"skyui.ESP": b"e", "SKYUI.BSA": bsa()})
        good = self.root("good", {"scripts/skse.PEX": skse_pex()})
        broken = self.root("broken", {"Scripts/SKSE.pex": b"bad"})
        self.assertEqual(self.check(skyui, broken, good)[0], OK)
        self.assertEqual(self.check(skyui, good, broken)[0], BAD_SCRIPT)

    def test_archive_must_be_classic(self):
        for number, content in enumerate((bsa(105), bsa(103), b"BSA", b"XXXX" + bytes(12))):
            skyui = self.root(f"s{number}", self.skyui(**{"SkyUI.bsa": content}))
            skse = self.root("k" + skyui.name, {"Scripts/SKSE.pex": skse_pex()})
            self.assertEqual(self.check(skyui, skse)[0], BAD_ARCHIVE)
        only_esp = self.root("esp", {"SkyUI.esp": b"e"})
        self.assertEqual(self.check(only_esp)[0], BAD_ARCHIVE)

    def test_links_and_ambiguous_aliases_are_refused(self):
        skyui = self.root("skyui", self.skyui())
        real = self.root("real", {"x.pex": skse_pex()})
        linked = self.root("linked", {})
        (linked / "Scripts").mkdir()
        (linked / "Scripts/SKSE.pex").symlink_to(real / "x.pex")
        self.assertEqual(self.check(skyui, linked)[0], UNREADABLE)
        alias = self.root("alias", {"Scripts/SKSE.pex": skse_pex()})
        alternate = alias / "scripts/skse.pex"
        if alternate.exists():
            self.assertEqual(self.check(skyui, alias)[0], OK)  # One entry on a case-insensitive volume.
        else:
            alternate.parent.mkdir()
            alternate.write_bytes(skse_pex())
            self.assertEqual(self.check(skyui, alias)[0], UNREADABLE)
        linked_dir = self.root("linked_dir", {})
        (linked_dir / "Scripts").symlink_to(real)
        self.assertEqual(self.check(skyui, linked_dir)[0], UNREADABLE)

    def test_xpmse_reports_dependencies_in_profile_order(self):
        fnis = self.root("fnis", {
            "meshes/actors/character/Characters/defaultmale.hkx": b"graph",
            "meshes/actors/character/characters female/defaultfemale.hkx": b"graph",
            "meshes/actors/character/behaviors/FNIS_FNISBase_Behavior.HKX": b"graph",
        })
        skse = self.root("skse", {"Scripts/SKSE.pex": skse_pex()})
        nioverride = self.root("nioverride", {"SKSE/Plugins/NiOverride.dll": b"authored marker"})
        self.assertEqual(self.check("--xpmse", fnis, skse, nioverride), [3, 1, 1, 0])
        self.assertEqual(self.check("--xpmse", skse), [0, 1, 0, 0])

        invalid = self.root("invalid", {"scripts/skse.pex": skse_pex(release=False)})
        self.assertEqual(self.check("--xpmse", skse, invalid), [0, 0, 0, 0])
        absent = self.root("absent", {})
        (absent / "Meshes").symlink_to(fnis / "meshes")
        self.assertEqual(self.check("--xpmse", fnis, absent, skse), [0, 1, 0, 3])

    def test_missing_roots_do_not_fail(self):
        skyui = self.root("skyui", self.skyui())
        self.assertEqual(self.check(skyui, str(self.top / "nope"))[0], NO_SCRIPT)


if __name__ == "__main__":
    unittest.main()
