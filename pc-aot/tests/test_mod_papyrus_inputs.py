"""Exercise the named-profile PapyrusUtil input acquisition with authored bytes."""
import errno
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
PROBE = ROOT / "experiments/skse-plugin-vm"
PROFILE = "12345678-abcd-1f23-80ab-6f72340a0ac0"
OTHER = "45678901-4321-ABCD-0123-45678901ABCD"
PAPYRUS = "2ed3e5a1-ff41-43e0-8fc1-e122dcddcde0"
OVERLAY = "396cc75c-1b11-428c-8017-8d32981c8ba3"
DLL = b"authored\x00dll"
PEX = b"authored pex"


@unittest.skipUnless(os.name == "posix", "Requires POSIX descriptor-relative operations")
class PapyrusInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "mod-papyrus-inputs"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT, "-I", PROBE]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_papyrus_inputs.c",
                        PORT / "mod_papyrus_inputs.c", PROBE / "probe_asset_file.c",
                        "-o", cls.binary], check=True, timeout=30)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = Path(tmp.name)
        self.support = self.top / "support"
        self.view = self.support / "Views" / PROFILE
        self.papyrus = self.support / "Mods" / PAPYRUS
        self.overlay = self.support / "Mods" / OVERLAY
        self.settings = self.support / "PapyrusSettings"
        self.write(self.view, "data/skSe/Plugins/STORAGEUTIL.DLL", DLL)
        self.write(self.view, "data/SCRIPTS/JsonUtil.pex", PEX)
        self.write(self.papyrus, "Data/SKSE/PLUGINS/StorageUtil.dll", DLL)
        self.write(self.papyrus, "Data/scripts/JSONUTIL.PEX", PEX)
        self.write(self.overlay, "Data/Textures/Elsewhere.dds", b"untouched")

    @staticmethod
    def write(root, path, content):
        file = root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(content)
        return file

    def acquire(self, *packages, profile=PROFILE):
        result = subprocess.check_output([self.binary, self.support, profile, *packages],
                                         text=True, timeout=8).split()
        return int(result[0]), result[1:]

    def check_success(self, *packages, profile=PROFILE):
        error, data = self.acquire(*packages, profile=profile)
        self.assertEqual(error, 0)
        self.assertEqual(data, [profile.replace("-", "").lower(), DLL.hex(), PEX.hex()])

    def check_refused(self, errors, *packages, profile=PROFILE):
        error, data = self.acquire(*packages, profile=profile)
        self.assertIn(error, errors)
        self.assertEqual(data, [])

    def test_named_profiles_grant_only_authored_pair_and_separate_private_root(self):
        self.check_success(PAPYRUS, OVERLAY)
        self.assertTrue(self.settings.is_dir())
        self.assertEqual(list(self.settings.iterdir()), [])
        original = self.write(self.top / "Documents/Skyrim", "Data/Scripts/JsonUtil.pex", b"original")
        second = self.support / "Views" / OTHER
        shutil.copytree(self.view, second)
        self.check_success(PAPYRUS, profile=OTHER)
        self.assertEqual(original.read_bytes(), b"original")
        self.assertEqual((self.papyrus / "Data/scripts/JSONUTIL.PEX").read_bytes(), PEX)

    def test_default_off_and_unrelated_selection_leave_no_grant(self):
        self.check_refused({errno.EINVAL}, PAPYRUS, profile="default")
        self.check_refused({errno.EINVAL}, PAPYRUS, profile="../outside")
        self.check_refused({errno.EINVAL}, "../outside")
        self.check_refused({errno.EINVAL})
        self.check_refused({errno.ENOENT}, OVERLAY)
        self.assertFalse(self.settings.exists())

    def test_later_partial_provider_or_mismatched_view_cannot_mix_inputs(self):
        self.write(self.overlay, "Data/Scripts/JsonUtil.pex", PEX)
        self.check_refused({errno.ESTALE}, PAPYRUS, OVERLAY)
        self.check_success(OVERLAY, PAPYRUS)
        self.settings.rmdir()
        self.write(self.view, "data/skSe/Plugins/STORAGEUTIL.DLL", b"projected original")
        self.check_refused({errno.ESTALE}, PAPYRUS)
        self.assertFalse(self.settings.exists())

    def test_rejects_case_alias_links_and_nonprivate_settings_folders(self):
        real = self.papyrus / "Data/SKSE/PLUGINS/StorageUtil.dll"
        alternate = real.with_name("STORAGEUTIL.DLL")
        if not alternate.exists():
            alternate.write_bytes(DLL)
            self.check_refused({errno.EACCES}, PAPYRUS)
            alternate.unlink()
        outside = self.top / "external.dll"
        outside.write_bytes(DLL)
        real.unlink()
        real.symlink_to(outside)
        self.check_refused({errno.EACCES}, PAPYRUS)
        self.assertFalse(self.settings.exists())
        real.unlink()
        real.write_bytes(DLL)
        directory = self.top / "other-private-data"
        directory.mkdir()
        self.settings.symlink_to(directory, target_is_directory=True)
        self.check_refused({errno.ELOOP, errno.ENOTDIR}, PAPYRUS)
        self.assertEqual(list(directory.iterdir()), [])
        self.settings.unlink()
        self.settings.mkdir(mode=0o755)
        self.settings.chmod(0o755)
        self.check_refused({errno.EACCES}, PAPYRUS)
        self.assertEqual(self.settings.stat().st_mode & 0o777, 0o755)
        self.assertEqual(outside.read_bytes(), DLL)

    def test_missing_and_oversize_active_view_are_not_snapshotted(self):
        script = self.view / "data/SCRIPTS/JsonUtil.pex"
        script.unlink()
        self.check_refused({errno.ENOENT}, PAPYRUS)
        with script.open("wb") as file:
            file.truncate(64 * 1024 + 1)
        self.check_refused({errno.EFBIG}, PAPYRUS)
        self.assertFalse(self.settings.exists())


if __name__ == "__main__":
    unittest.main()
