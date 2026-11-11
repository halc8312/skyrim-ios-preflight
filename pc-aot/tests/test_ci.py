import datetime as dt
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


record = module("record", "record-build.py")
safety = module("safety", "check-public-tree.py")
TEST_UUID = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")


def binary(*, cpu=0x0100000C, subtype=0, platform=2, minimum=18 << 16,
           dsym=False, identifier=TEST_UUID, signed=False):
    commands = struct.pack("<2I", 0x1B, 24) + identifier.bytes
    commands += struct.pack("<6I", 0x32, 24, platform, minimum, (26 << 16) | (5 << 8), 0)
    count = 2
    if signed:
        commands += struct.pack("<4I", 0x1D, 16, 32 + len(commands) + 16, 4)
        count += 1
    header = struct.pack("<8I", 0xFEEDFACF, cpu, subtype, 10 if dsym else 2, count, len(commands), 0, 0)
    return header + commands + (b"TEST" if signed else b"")


def profile():
    return {"TeamIdentifier": ["TESTTEAM01"], "UUID": str(TEST_UUID),
            "Entitlements": {"application-identifier": "TESTTEAM01.org.example.probe",
                             "com.apple.developer.team-identifier": "TESTTEAM01", "get-task-allow": False},
            "ProvisionedDevices": ["synthetic-device"], "DeveloperCertificates": [b"synthetic-cert"],
            "ExpirationDate": dt.datetime.now(dt.timezone.utc) + dt.timedelta(days=1)}


class MachOTests(unittest.TestCase):
    def test_accepts_device_arm64_and_reads_uuid_sdk(self):
        actual = record.macho(binary())
        self.assertEqual(actual["uuid"], str(TEST_UUID).upper())
        self.assertEqual(actual["sdk"], "26.5.0")
        self.assertFalse(actual["signed_load_command"])

    def test_rejects_wrong_architecture_platform_or_deployment(self):
        for options in ({"cpu": 0x01000007}, {"platform": 7}, {"platform": 1},
                        {"subtype": 2}, {"minimum": 17 << 16}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                record.macho(binary(**options))

    def test_detects_signature_and_malformed_commands(self):
        self.assertTrue(record.macho(binary(signed=True))["signed_load_command"])
        for malformed in (b"bad", binary()[:-1], binary()[:32], binary(signed=True)[:-2]):
            with self.assertRaises(ValueError):
                record.macho(malformed)


class ProfileTests(unittest.TestCase):
    def test_accepts_exact_registered_device_distribution_profile(self):
        self.assertTrue(record.validate_profile(profile(), "org.example.probe", "TESTTEAM01", "synthetic-device")
                        ["registered_device_checked"])

    def test_rejects_wrong_device_wildcard_development_enterprise_and_expiry(self):
        for change in ("device", "wildcard", "development", "enterprise", "expiry", "team"):
            data = profile()
            if change == "device":
                data["ProvisionedDevices"] = []
            elif change == "wildcard":
                data["Entitlements"]["application-identifier"] = "TESTTEAM01.*"
            elif change == "development":
                data["Entitlements"]["get-task-allow"] = True
            elif change == "enterprise":
                data["ProvisionsAllDevices"] = True
            elif change == "expiry":
                data["ExpirationDate"] = dt.datetime(2000, 1, 1)
            else:
                data["TeamIdentifier"] = ["OTHERTEAM1"]
            with self.subTest(change=change), self.assertRaises(ValueError):
                record.validate_profile(data, "org.example.probe", "TESTTEAM01", "synthetic-device")


class SafetyTests(unittest.TestCase):
    def test_game_renaming_and_generated_source_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, content in (("asset.txt", b"MZbinary"), ("input.dll", b"text"),
                                  ("port.c", b"void sub_00401000(void) {}"),
                                  ("generated/port.c", b"int clean;"),
                                  ("source.c", b"int own_source(void) { return 0; }")):
                path = root / name
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(content)
                self.assertEqual(bool(safety.check_file(name, path)), name != "source.c", name)

    def test_ignored_private_inputs_not_scanned_but_force_added_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("/local/\n", encoding="utf-8")
            (root / "local").mkdir()
            (root / "local/game.exe").write_bytes(b"MZprivate-fixture")
            self.assertEqual(safety.scan(root), [])
            subprocess.run(["git", "-C", str(root), "add", "-f", "local/game.exe"], check=True)
            self.assertTrue(any("private/generated" in error for error in safety.scan(root)))

    def test_scopes_paths_correctly_inside_existing_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            source = repo / "pc-aot"
            source.mkdir()
            (source / "source.c").write_text("int own_source;\n", encoding="utf-8")
            (repo / "unrelated.txt").write_bytes(b"MZoutside-scoped-source")
            self.assertEqual(safety.scan(source), [])
            (source / "bad.txt").write_bytes(b"MZinside-scoped-source")
            self.assertTrue(any("binary" in error for error in safety.scan(source)))


class EvidenceTests(unittest.TestCase):
    def make_unsigned_run(self, root):
        (root / "artifacts").mkdir()
        record.save_manifest(root, {"status": "started", "sdk_version": "26.5", "mode": "unsigned"})
        products = root / "DerivedData/Build/Products/Release-iphoneos"
        app = products / "SkyrimProbe.app"
        app.mkdir(parents=True)
        (app / "SkyrimProbe").write_bytes(binary())
        (app / "Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable": "SkyrimProbe",
            "CFBundleIdentifier": "org.skyrimios.probe", "CFBundleSupportedPlatforms": ["iPhoneOS"],
            "DTPlatformName": "iphoneos", "MinimumOSVersion": "18.0"}))
        dwarf = products / "SkyrimProbe.app.dSYM/Contents/Resources/DWARF/SkyrimProbe"
        dwarf.parent.mkdir(parents=True)
        dwarf.write_bytes(binary(dsym=True))
        return app, dwarf

    def test_unsigned_package_hash_manifest_and_noninstallability(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {}, clear=True):
            root = Path(temp)
            self.make_unsigned_run(root)
            record.verify(root, "unsigned")
            (root / "build.log").write_text("synthetic log\n", encoding="utf-8")
            record.finish(root, 0)
            manifest = json.loads((root / "artifacts/manifest.json").read_text())
            self.assertEqual(manifest["status"], "verified")
            self.assertFalse(manifest["installable"])
            self.assertEqual(manifest["dsym"]["uuid"], manifest["executable"]["uuid"])
            for artifact in manifest["artifacts"]:
                self.assertEqual(artifact["sha256"], record.sha256(root / "artifacts" / artifact["path"]))
            with zipfile.ZipFile(root / "artifacts/SkyrimProbe-UNSIGNED-NOT-INSTALLABLE.app.zip") as archive:
                self.assertIn("SkyrimProbe.app/SkyrimProbe", archive.namelist())

    def test_rejects_dsym_mismatch_and_foreign_app_payload(self):
        for error in ("uuid", "payload", "signed", "sdk"):
            with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {}, clear=True):
                root = Path(temp)
                app, dwarf = self.make_unsigned_run(root)
                if error == "uuid":
                    dwarf.write_bytes(binary(dsym=True, identifier=uuid.uuid4()))
                elif error == "payload":
                    (app / "TESV.exe").write_bytes(b"MZtest")
                elif error == "signed":
                    (app / "SkyrimProbe").write_bytes(binary(signed=True))
                else:
                    record.save_manifest(root, {"sdk_version": "26.6"})
                with self.subTest(error=error), self.assertRaises(ValueError):
                    record.verify(root, "unsigned")
                self.assertEqual(list((root / "artifacts").iterdir()), [])

    def test_failure_keeps_logs_and_cannot_claim_installable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "artifacts").mkdir()
            (root / "build.log").write_text("compile failed\n", encoding="utf-8")
            record.save_manifest(root, {"status": "started", "installable": True})
            record.finish(root, 65)
            manifest = json.loads((root / "artifacts/manifest.json").read_text())
            self.assertEqual(manifest["exit_code"], 65)
            self.assertEqual(manifest["status"], "failed")
            self.assertFalse(manifest["installable"])
            self.assertEqual((root / "build.log").read_text(), "compile failed\n")


class WorkflowTests(unittest.TestCase):
    def test_yaml_has_manual_only_workflows_and_narrow_artifacts(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("optional existing PyYAML unavailable; no dependency downloaded")
        config = yaml.safe_load((ROOT / "codemagic.yaml").read_text())
        self.assertEqual(set(config["workflows"]), {"probe-unsigned", "probe-ad-hoc"})
        for workflow in config["workflows"].values():
            self.assertNotIn("triggering", workflow)
            self.assertNotIn("publishing", workflow)
            self.assertNotIn("cache", workflow)
            self.assertEqual(workflow["environment"]["xcode"], "26.6")
            self.assertEqual(workflow["instance_type"], "mac_mini_m2")
            self.assertEqual(workflow["artifacts"], ["local/ci/*/artifacts/*"])
        self.assertNotIn("ios_signing", config["workflows"]["probe-unsigned"]["environment"])


if __name__ == "__main__":
    unittest.main()
