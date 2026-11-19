"""Game-free signing contracts; mocked tools never establish Apple signing success."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("sign_title", ROOT / "scripts/sign_title_ipa.py")
SIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SIGN)
CERTIFICATE = b"synthetic certificate bytes; not a real identity"
IDENTITY = hashlib.sha1(CERTIFICATE).hexdigest().upper()
BUNDLE = "org.example.owned-skyrim"
NOW = datetime(2026, 11, 16, tzinfo=timezone.utc)


def profile():
    return {
        "TeamIdentifier": ["TESTTEAM01"], "ApplicationIdentifierPrefix": ["PREFIX0001"],
        "Platform": ["iOS"], "ProvisionedDevices": ["00000000-SYNTHETIC00000000"],
        "ExpirationDate": datetime.now(timezone.utc).replace(tzinfo=None) + timedelta(days=30),
        "CreationDate": datetime(2026, 1, 1), "DeveloperCertificates": [CERTIFICATE],
        "Entitlements": {"application-identifier": "PREFIX0001." + BUNDLE,
                         "com.apple.developer.team-identifier": "TESTTEAM01", "get-task-allow": True,
                         **{key: True for key in SIGN.MEMORY_KEYS}, SIGN.DEBUG_MEMORY: True},
    }


def app_files():
    info = {"CFBundleExecutable": "SkyrimIOS", "CFBundleIdentifier": "org.skyrimios.pc2011",
            "CFBundleVersion": "12", "MinimumOSVersion": "18.0",
            "CFBundleSupportedPlatforms": ["iPhoneOS"], "PcGuestExpectedInputSHA256": "a" * 64}
    header = lambda kind: struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, kind, 0, 0, 0, 0)
    return {
        SIGN.APP + "/Info.plist": plistlib.dumps(info),
        SIGN.APP + "/SkyrimIOS": header(2),
        SIGN.APP + "/" + SIGN.FRAMEWORK + "/Info.plist": plistlib.dumps({"CFBundleExecutable": "SkyrimGuest"}),
        SIGN.APP + "/" + SIGN.FRAMEWORK + "/SkyrimGuest": header(6),
        SIGN.APP + "/Assets/example.txt": b"public fixture resource\n",
    }


def write_archive(path, entries=None, extra=None):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in (entries or app_files()).items():
            member = zipfile.ZipInfo(name)
            member.create_system = 3
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(member, data)
        for member, data in extra or []:
            if isinstance(member, str):
                # ZipInfo's Windows constructor normalizes backslashes. Preserve
                # malicious wire names explicitly so this exercises the reader.
                name = member
                member = zipfile.ZipInfo("placeholder")
                member.filename = name
                member.orig_filename = name
            archive.writestr(member, data)


class ProfileTests(unittest.TestCase):
    def test_exact_authorization_accepts_legacy_app_prefix_and_minimal_entitlements(self):
        p = profile()
        p["Entitlements"]["aps-environment"] = "development"
        result = SIGN.validate_profile(p, IDENTITY.lower(), BUNDLE)
        self.assertEqual(result["application-identifier"], "PREFIX0001." + BUNDLE)
        self.assertEqual(result["com.apple.developer.team-identifier"], "TESTTEAM01")
        self.assertNotIn("aps-environment", result)
        self.assertTrue(result[SIGN.DEBUG_MEMORY])
        p["Entitlements"]["get-task-allow"] = False
        del p["Entitlements"][SIGN.DEBUG_MEMORY]
        result = SIGN.validate_profile(p, IDENTITY, BUNDLE)
        self.assertFalse(result["get-task-allow"])
        self.assertNotIn(SIGN.DEBUG_MEMORY, result)

    def test_rejects_wrong_app_certificate_expiry_devices_and_ungranted_memory(self):
        mutations = [
            lambda p: p["Entitlements"].update({"application-identifier": "PREFIX0001.*"}),
            lambda p: p["Entitlements"].update({"com.apple.developer.team-identifier": "OTHERTEAM1"}),
            lambda p: p.update(DeveloperCertificates=[b"different certificate"]),
            lambda p: p.update(ExpirationDate=datetime(2020, 1, 1)),
            lambda p: p.update(CreationDate=datetime(2090, 1, 1)),
            lambda p: p.update(ProvisionedDevices=[]),
            lambda p: p.update(ProvisionsAllDevices=True),
            lambda p: p.update(Platform=["OSX"]),
            lambda p: p["Entitlements"].update({SIGN.MEMORY_KEYS[0]: False}),
            lambda p: p["Entitlements"].update({SIGN.MEMORY_KEYS[1]: 1}),
            lambda p: p["Entitlements"].pop("get-task-allow"),
        ]
        for i, mutate in enumerate(mutations):
            with self.subTest(i=i):
                p = profile(); mutate(p)
                with self.assertRaises(ValueError):
                    SIGN.validate_profile(p, IDENTITY, BUNDLE)

    def test_only_full_valid_keychain_fingerprint_is_accepted(self):
        found = ('  1) ' + IDENTITY + ' "Apple Development: synthetic fixture"\n    1 valid identities found\n').encode()
        self.assertTrue(SIGN.keychain_identity_available(found, IDENTITY))
        self.assertFalse(SIGN.keychain_identity_available(found, "0" * 40))
        self.assertFalse(SIGN.keychain_identity_available(b"0 valid identities found", IDENTITY))
        with self.assertRaises(ValueError):
            SIGN.validate_profile(profile(), "Apple Development", BUNDLE)


class ArchiveTests(unittest.TestCase):
    def test_unsigned_owned_package_and_thin_device_binaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "unsigned.ipa"; write_archive(path)
            with zipfile.ZipFile(path) as archive:
                files = SIGN.unsigned_members(archive)
                SIGN.validate_app_info(archive, files)
                self.assertEqual(len(files), 5)

    def test_rejects_escape_duplicates_case_collisions_links_and_signatures(self):
        invalid = ["../outside", SIGN.APP + "/../outside", SIGN.APP + "//bad", SIGN.APP + "/./bad",
                   SIGN.APP + "/bad\\path", SIGN.APP + "/bad:stream", SIGN.APP + "/Info.plist",
                   SIGN.APP + "/info.plist", SIGN.APP + "/embedded.mobileprovision",
                   SIGN.APP + "/_CodeSignature/CodeResources", SIGN.APP + "/PlugIns/Other.appex/Other",
                   SIGN.APP + "/Frameworks/Other.framework/Other", "Other.app/Info.plist"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "unsigned.ipa"
            for name in invalid:
                with self.subTest(name=name), warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    write_archive(path, extra=[(name, b"bad")])
                    with zipfile.ZipFile(path) as archive, self.assertRaises(ValueError):
                        SIGN.unsigned_members(archive)
            member = zipfile.ZipInfo(SIGN.APP + "/link")
            member.create_system = 3; member.external_attr = (stat.S_IFLNK | 0o777) << 16
            write_archive(path, extra=[(member, b"/outside")])
            with zipfile.ZipFile(path) as archive, self.assertRaises(ValueError):
                SIGN.unsigned_members(archive)

    def test_rejects_file_parent_collision_and_archive_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "unsigned.ipa"
            write_archive(path, extra=[(SIGN.APP + "/Assets", b"file")])
            with zipfile.ZipFile(path) as archive, self.assertRaises(ValueError):
                SIGN.unsigned_members(archive)
            write_archive(path)
            with zipfile.ZipFile(path) as archive, mock.patch.object(SIGN, "MAX_ARCHIVE", 10), self.assertRaises(ValueError):
                SIGN.unsigned_members(archive)

    def test_rejects_private_pairing_missing_guest_and_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "unsigned.ipa"
            for change in ("pairing", "missing_guest", "wrong_arch", "unbound"):
                entries = app_files()
                if change in ("pairing", "unbound"):
                    info = plistlib.loads(entries[SIGN.APP + "/Info.plist"])
                    if change == "pairing": info["PcDiagnostics"] = {"token": "synthetic token"}
                    else: info.pop("PcGuestExpectedInputSHA256")
                    entries[SIGN.APP + "/Info.plist"] = plistlib.dumps(info)
                elif change == "missing_guest": entries.pop(SIGN.APP + "/" + SIGN.FRAMEWORK + "/SkyrimGuest")
                else: entries[SIGN.APP + "/SkyrimIOS"] = b"MZ" + b"0" * 30
                write_archive(path, entries)
                with self.subTest(change=change), zipfile.ZipFile(path) as archive, self.assertRaises(ValueError):
                    SIGN.validate_app_info(archive, SIGN.unsigned_members(archive))


class FakeAppleTools:
    """Simulates commands for orchestration/error tests, not cryptographic evidence."""
    def __init__(self, p, fail_at=None, wrong_entitlements=False, wrong_certificate=False, mutate_resource=False):
        self.profile = p
        self.calls = []
        self.fail_at = fail_at
        self.wrong_entitlements = wrong_entitlements
        self.wrong_certificate = wrong_certificate
        self.mutate_resource = mutate_resource
        self.entitlements = None

    def __call__(self, command, log):
        command = [str(x) for x in command]
        self.calls.append(command)
        log.write_text("Synthetic command fixture; no Apple code executed\n")
        if len(self.calls) == self.fail_at:
            raise RuntimeError("injected command failure")
        if command[1:3] == ["cms", "-D"]:
            return plistlib.dumps(self.profile)
        if command[1] == "find-identity":
            return ('  1) ' + IDENTITY + ' "Synthetic identity"\n').encode()
        if "--sign" in command:
            target = Path(command[-1])
            (target / "_CodeSignature").mkdir()
            (target / "_CodeSignature/CodeResources").write_bytes(b"synthetic signature")
            if "--entitlements" in command:
                self.entitlements = plistlib.loads(Path(command[command.index("--entitlements") + 1]).read_bytes())
                if self.mutate_resource: (target / "Assets/example.txt").write_bytes(b"unexpected change")
            return b""
        if "--verify" in command:
            return b""
        if "--extract-certificates" in command:
            prefix = command[command.index("--extract-certificates") + 1]
            Path(prefix + "0").write_bytes(b"wrong certificate" if self.wrong_certificate else CERTIFICATE)
            return b""
        if "--entitlements" in command:
            return plistlib.dumps({} if self.wrong_entitlements else self.entitlements)
        raise AssertionError("Unexpected command")


class SigningFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.unsigned = self.root / "unsigned.ipa"; write_archive(self.unsigned)
        self.profile = self.root / "test.mobileprovision"; self.profile.write_bytes(b"synthetic CMS input")
        self.output = self.root / "private/delivery/test.ipa"

    def sign(self, runner, output=None):
        return SIGN.sign_package(self.unsigned, self.profile, IDENTITY, BUNDLE,
                                 output or self.output, root=self.root, runner=runner)

    def test_classic_lab_can_only_sign_as_a_separate_application(self):
        entries = app_files()
        for name in (SIGN.APP + "/Info.plist", SIGN.APP + "/" + SIGN.FRAMEWORK + "/Info.plist"):
            info = plistlib.loads(entries[name])
            info["PcGuestClassicSkseExperiment"] = True
            if name == SIGN.APP + "/Info.plist":
                info["CFBundleIdentifier"] += ".classic-lab"
            entries[name] = plistlib.dumps(info)
        write_archive(self.unsigned, entries)
        with self.assertRaisesRegex(ValueError, "dedicated Classic Lab"):
            self.sign(FakeAppleTools(profile()))
        self.assertFalse(self.output.exists())
        lab_id = BUNDLE + ".classic-lab"
        lab_profile = profile()
        lab_profile["Entitlements"]["application-identifier"] = "PREFIX0001." + lab_id
        result = SIGN.sign_package(self.unsigned, self.profile, IDENTITY, lab_id, self.output,
                                   root=self.root, runner=FakeAppleTools(lab_profile))
        self.assertTrue(result["signed"])
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(plistlib.loads(archive.read(SIGN.APP + "/Info.plist"))["CFBundleIdentifier"], lab_id)

    def test_ordered_signing_profile_subset_and_atomic_no_replace(self):
        unsigned_hash = SIGN.sha(self.unsigned)
        runner = FakeAppleTools(profile())
        result = self.sign(runner)
        self.assertTrue(result["signed"])
        self.assertTrue(result["codesign_verified"])
        self.assertFalse(result["profile_apple_chain_independently_verified"])
        self.assertFalse(result["installed"])
        self.assertEqual(result["sha256"], SIGN.sha(self.output))
        self.assertEqual(unsigned_hash, SIGN.sha(self.unsigned))
        self.assertIn("SkyrimGuest.framework", runner.calls[2][-1])
        self.assertIn("--entitlements", runner.calls[3])
        self.assertNotIn("--deep", runner.calls[2] + runner.calls[3])
        self.assertIn("--deep", runner.calls[5])
        with zipfile.ZipFile(self.output) as archive:
            info = plistlib.loads(archive.read(SIGN.APP + "/Info.plist"))
            self.assertEqual(info["CFBundleIdentifier"], BUNDLE)
            self.assertNotIn("PcDiagnostics", info)
            self.assertEqual(archive.read(SIGN.APP + "/embedded.mobileprovision"), self.profile.read_bytes())
            self.assertEqual(archive.read(SIGN.APP + "/Assets/example.txt"), b"public fixture resource\n")
        calls = len(runner.calls)
        with self.assertRaises(ValueError): self.sign(runner)
        self.assertEqual(calls, len(runner.calls))
        self.assertEqual(result["sha256"], SIGN.sha(self.output))

    def test_tool_failure_preserves_evidence_without_publishing(self):
        with self.assertRaises(RuntimeError): self.sign(FakeAppleTools(profile(), fail_at=4))
        self.assertFalse(self.output.exists())
        work = next(self.output.parent.glob("sign-title-*"))
        self.assertTrue((work / "unsigned.ipa").exists())
        self.assertTrue((work / "logs/sign-app.log").exists())
        result = json.loads((work / "result.json").read_text())
        self.assertTrue(result["failure_evidence"])
        self.assertFalse(result["codesign_verified"])

    def test_publish_race_never_replaces_an_existing_output(self):
        runner = FakeAppleTools(profile())
        def competing_writer(command, log):
            result = runner(command, log)
            if "--extract-certificates" in command and str(command[-1]).endswith("SkyrimIOS.app"):
                self.output.write_bytes(b"another writer's existing output")
            return result
        with self.assertRaises(FileExistsError): self.sign(competing_writer)
        self.assertEqual(self.output.read_bytes(), b"another writer's existing output")
        work = next(self.output.parent.glob("sign-title-*"))
        result = json.loads((work / "result.json").read_text())
        self.assertTrue(result["codesign_verified"])
        self.assertFalse(result["published"])
        self.assertTrue((work / "signed.ipa").is_file())

    def test_signed_entitlements_certificate_and_resource_must_match(self):
        for field in ("wrong_entitlements", "wrong_certificate", "mutate_resource"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.sign(FakeAppleTools(profile(), **{field: True}))
            self.assertFalse(self.output.exists())

    def test_profile_rejection_happens_before_any_signing_command(self):
        p = profile(); p["Entitlements"].pop(SIGN.MEMORY_KEYS[0])
        runner = FakeAppleTools(p)
        with self.assertRaises(ValueError): self.sign(runner)
        self.assertEqual(len(runner.calls), 1)
        self.assertFalse(self.output.exists())

    def test_output_must_stay_private_and_symlink_must_not_escape(self):
        runner = FakeAppleTools(profile())
        with self.assertRaises(ValueError): self.sign(runner, self.root / "public.ipa")
        self.assertFalse(runner.calls)
        private = self.root / "private"; private.mkdir()
        outside = self.root / "outside"; outside.mkdir()
        try:
            (private / "escape").symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("Host does not allow creating directory symlinks")
        with self.assertRaises(ValueError): self.sign(runner, private / "escape/test.ipa")
        self.assertFalse(runner.calls)


@unittest.skipUnless(sys.platform == "darwin", "Requires real Apple SDK and codesign tools")
class AppleSigningSyntaxTests(unittest.TestCase):
    def test_real_apple_ad_hoc_fixture_and_entitlement_readback(self):
        """Real local signature syntax only: '-' is not a device-installable identity."""
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp).resolve()
            app = base / "SkyrimIOS.app"
            framework = app / SIGN.FRAMEWORK
            framework.mkdir(parents=True)
            info = {"CFBundleExecutable": "SkyrimIOS", "CFBundleIdentifier": BUNDLE,
                    "CFBundleName": "SkyrimIOS", "CFBundleVersion": "1",
                    "CFBundlePackageType": "APPL", "MinimumOSVersion": "18.0",
                    "CFBundleSupportedPlatforms": ["iPhoneOS"]}
            (app / "Info.plist").write_bytes(plistlib.dumps(info))
            (framework / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleExecutable": "SkyrimGuest", "CFBundleIdentifier": "org.example.fixtureguest",
                "CFBundleName": "SkyrimGuest", "CFBundleVersion": "1", "CFBundlePackageType": "FMWK"}))
            source = base / "fixture.c"
            source.write_text("int main(void) { return 0; }\n")
            sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
            common = ["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios18.0",
                      "-isysroot", sdk, str(source)]
            for output, flags in ((app / "SkyrimIOS", []),
                                  (framework / "SkyrimGuest", ["-dynamiclib", "-Wl,-install_name,@rpath/SkyrimGuest.framework/SkyrimGuest"])):
                subprocess.run(common + flags + ["-o", str(output)], check=True, capture_output=True)
            ent = SIGN.validate_profile(profile(), IDENTITY, BUNDLE)
            ent_path = base / "entitlements.plist"; ent_path.write_bytes(plistlib.dumps(ent))
            for bundle, flags in ((framework, []), (app, ["--entitlements", str(ent_path)])):
                subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none",
                                "--generate-entitlement-der", *flags, str(bundle)], check=True, capture_output=True)
            subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)],
                           check=True, capture_output=True)
            readback = subprocess.check_output(["/usr/bin/codesign", "-d", "--entitlements", ":-", str(app)],
                                               stderr=subprocess.PIPE)
            self.assertEqual(plistlib.loads(readback), ent)


if __name__ == "__main__":
    unittest.main()
