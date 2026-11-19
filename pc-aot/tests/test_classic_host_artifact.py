"""Exercise the public Classic host handoff without an Apple host or game."""
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock
import uuid
import zipfile

from test_ci import binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import stage_classic_host_artifact as artifact


class ClassicHostArtifact(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        self.source, self.out = root / "artifacts", root / "transfer"
        self.source.mkdir()
        linked = root / "build/SkyrimIOS.app/SkyrimIOS"
        linked.parent.mkdir(parents=True)
        linked.write_bytes(binary())
        runtime = root / "overlay/runtime"
        runtime.mkdir(parents=True)
        for name in artifact.ABI_HEADERS:
            if name.startswith("PcGuest"):
                original = artifact.ABI / name
            else:
                original = runtime / name
                original.write_bytes(b"/* synthetic public header */")
            shutil.copyfile(original, self.source / name)
        self.commit = "a" * 40
        self.info = {"CFBundleExecutable": "SkyrimIOS", "CFBundleDisplayName": "SkyrimIOS Classic Lab",
                     "CFBundleIdentifier": "org.skyrimios.pc2011.classic-lab", "PcGuestClassicSkseExperiment": True}
        self.archive()

    def archive(self, *, exe=None, extra=None):
        path = self.source / artifact.HOST
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("SkyrimIOS.app/SkyrimIOS", binary() if exe is None else exe)
            archive.writestr("SkyrimIOS.app/Info.plist", plistlib.dumps(self.info))
            archive.writestr("SkyrimIOS.app/ModArchiveLicenses.txt", "fixture")
            if extra:
                archive.writestr(extra, b"unexpected")
        self.report = dict(schema=1, source_commit=self.commit, artifact_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                           target="arm64-apple-ios", public_host_sdk_build=True, private_guest_included=False,
                           game_executed=False, physical_iphone_verified=False, signed_for_installation=False,
                           classic_skse_experiment=True)
        self.write_report()

    def write_report(self):
        (self.source / "result.json").write_text(json.dumps(self.report))

    def stage(self):
        artifact.stage(self.source, self.out, self.commit)

    def test_only_verified_six_files_leave_build_directory(self):
        (self.source / "build.log").write_text("local diagnostic")
        (self.source / "unrelated.zip").write_bytes(b"other")
        self.stage()
        names = {p.name for p in self.out.iterdir()}
        self.assertEqual(names, {artifact.HOST, "result.json", *artifact.ABI_HEADERS})
        for name in names:
            self.assertEqual((self.out / name).read_bytes(), (self.source / name).read_bytes())
        with self.assertRaises(FileExistsError):
            self.stage()

    def test_reject_incomplete_or_stale_result_and_changed_headers(self):
        for key, bad in (("schema", True), ("source_commit", "b" * 40), ("private_guest_included", True),
                         ("artifact_sha256", "0" * 64), ("unreviewed_field", "unexpected")):
            with self.subTest(key=key):
                self.archive()
                self.report[key] = bad
                self.write_report()
                with self.assertRaises(ValueError):
                    self.stage()
                self.assertFalse(self.out.exists())
        self.archive()
        (self.source / "x86.h").write_bytes(b"wrong header")
        with self.assertRaisesRegex(ValueError, "x86.h"):
            self.stage()

    def test_failed_copy_does_not_publish_partial_files_or_prevent_a_retry(self):
        with mock.patch.object(artifact.shutil, "copyfile", side_effect=[None, OSError("full")]):
            with self.assertRaises(OSError):
                self.stage()
        self.assertFalse(self.out.exists())
        self.stage()
        self.assertEqual(len(list(self.out.iterdir())), 6)

    def test_another_valid_unsigned_app_cannot_replace_the_symbol_checked_binary(self):
        self.archive(exe=binary(identifier=uuid.UUID(int=7)))
        with self.assertRaisesRegex(ValueError, "linked app"):
            self.stage()
        self.assertFalse(self.out.exists())

    def test_signed_simulator_ordinary_or_extra_payload_are_rejected(self):
        for options in ({"exe": binary(signed=True)}, {"exe": binary(platform=7)},
                        {"extra": "SkyrimIOS.app/embedded.mobileprovision"},
                        {"extra": "SkyrimIOS.app/Frameworks/SkyrimGuest.framework/SkyrimGuest"}):
            self.archive(**options)
            with self.assertRaises(ValueError):
                self.stage()
            self.assertFalse(self.out.exists())
        self.info["CFBundleIdentifier"] = "org.skyrimios.pc2011"
        self.archive()
        with self.assertRaisesRegex(ValueError, "identity"):
            self.stage()


if __name__ == "__main__":
    unittest.main()
