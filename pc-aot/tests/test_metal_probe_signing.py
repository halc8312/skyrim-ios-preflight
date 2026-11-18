"""Synthetic signing-admission/packaging controls; no real identity or signing."""
import copy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import stat
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_probe_signer", ROOT / "scripts/sign-metal-probe-ipa.py")
signer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(signer)
NOW = datetime(2026, 11, 18, tzinfo=timezone.utc)
TEAM, DEVICE, CERT = "TEST000001", "SYNTHETIC-DEVICE-001", b"synthetic-certificate-not-an-identity"


def image(*, platform=2, minimum=26 << 16, cpu=0x0100000C, signed=False, dependency=None):
    commands = [struct.pack("<6I", 0x32, 24, platform, minimum, 26 << 16, 0),
                struct.pack("<2I", 0x1B, 24) + bytes(range(16))]
    if signed:
        commands.append(struct.pack("<4I", 0x1D, 16, 4096, 128))
    if dependency is not None:
        name = dependency.encode() + b"\0"
        name += bytes((-len(name)) % 8)
        commands.append(struct.pack("<6I", 0xC, 24 + len(name), 24, 0, 0, 0) + name)
    raw = b"".join(commands)
    return struct.pack("<8I", 0xFEEDFACF, cpu, 0, 2, len(commands), len(raw), 0, 0) + raw + bytes(64)


def members():
    info = plistlib.loads((ROOT / "experiments/metal-compile/ios/Info.plist").read_bytes())
    info["CFBundleSupportedPlatforms"] = ["iPhoneOS"]
    proof = {key: False for key in ("signed_for_device", "installed", "physical_iphone_verified",
             "gpu_execution_verified", "metal4_runtime_verified", "gameplay_improvement_verified")}
    proof.update(bundle_id=signer.BUNDLE, simulator=False,
                 inputs={"experiments/metal-compile/fixture.metal": signer.FIXTURE_SHA256,
                         "experiments/metal-compile/phone_fixture.metal": signer.PHONE_FIXTURE_SHA256,
                         "experiments/metal-compile/phone_workloads.h": signer.digest(
                             (ROOT / "experiments/metal-compile/phone_workloads.h").read_bytes())})
    return {"MetalProbe": image(), "Info.plist": plistlib.dumps(info),
            "BuildInfo.json": json.dumps(proof).encode(),
            "fixture.metal": (ROOT / "experiments/metal-compile/fixture.metal").read_bytes(),
            "phone_fixture.metal": (ROOT / "experiments/metal-compile/phone_fixture.metal").read_bytes()}


def profile():
    return {"TeamIdentifier": [TEAM], "ApplicationIdentifierPrefix": [TEAM],
            "ProvisionedDevices": [DEVICE], "Platform": ["iOS"], "DeveloperCertificates": [CERT],
            "CreationDate": NOW - timedelta(days=1), "ExpirationDate": NOW + timedelta(days=1),
            "Entitlements": {"application-identifier": TEAM + "." + signer.BUNDLE,
                "com.apple.developer.team-identifier": TEAM, "get-task-allow": True,
                "keychain-access-groups": [TEAM + ".*", "com.apple.token"]}}


def write_ipa(path, files=None, extra=()):
    with zipfile.ZipFile(path, "w") as archive:
        for name, raw in (files or members()).items():
            item = zipfile.ZipInfo(signer.APP + "/" + name)
            item.create_system = 3
            item.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(item, raw)
        for item, raw in extra:
            archive.writestr(item, raw)
    return path


class ProbeSigningAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="metal-probe-signing-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def test_only_authored_five_file_device_app_is_accepted(self):
        self.assertEqual(signer.read_unsigned(write_ipa(self.root / "probe.ipa").read_bytes()), members())

    def test_phone_fixture_mutation_is_refused_even_with_matching_metadata(self):
        files = members()
        files["phone_fixture.metal"] += b"\n// changed phone workload\n"
        for forged_provenance in (False, True):
            if forged_provenance:
                provenance = json.loads(files["BuildInfo.json"])
                provenance["inputs"]["experiments/metal-compile/phone_fixture.metal"] = signer.digest(
                    files["phone_fixture.metal"])
                files["BuildInfo.json"] = json.dumps(provenance).encode()
            with self.subTest(forged_provenance=forged_provenance), \
                    self.assertRaisesRegex(signer.Refusal, "authored_phone_fixture_mismatch"):
                signer.bundle_contract(files)

    def test_missing_phone_fixture_and_unexpected_workload_resource_are_refused(self):
        files = members()
        del files["phone_fixture.metal"]
        with self.assertRaisesRegex(signer.Refusal, "fixed_unsigned_file_set"):
            signer.read_unsigned(write_ipa(self.root / "old-four-file-app.ipa", files).read_bytes())
        files = members()
        files["phone_workloads.h"] = b"unexpected source resource"
        with self.assertRaisesRegex(signer.Refusal, "unexpected_zip_member"):
            signer.read_unsigned(write_ipa(self.root / "extra-source-app.ipa", files).read_bytes())

    def test_phone_source_provenance_is_required_and_hash_validated(self):
        missing = object()
        for source, invalid_values in (("phone_fixture.metal", (missing, "0" * 64, None)),
                                      ("phone_workloads.h", (missing, "", "g" * 64, "a" * 63, None, 7))):
            for invalid in invalid_values:
                files = members()
                provenance = json.loads(files["BuildInfo.json"])
                key = "experiments/metal-compile/" + source
                if invalid is missing:
                    del provenance["inputs"][key]
                else:
                    provenance["inputs"][key] = invalid
                files["BuildInfo.json"] = json.dumps(provenance).encode()
                with self.subTest(source=source, invalid=invalid), \
                        self.assertRaisesRegex(signer.Refusal, "phone_.*provenance"):
                    signer.bundle_contract(files)

    def test_foreign_app_simulator_forged_success_and_fixture_are_refused(self):
        for field, value in (("CFBundleIdentifier", "org.skyrimios.title"),
                             ("CFBundleIdentifier", "org.skyrimios.probe"),
                             ("CFBundleExecutable", "SkyrimIOS"),
                             ("CFBundleSupportedPlatforms", ["iPhoneSimulator"]),
                             ("MinimumOSVersion", "18.0")):
            files = members()
            info = plistlib.loads(files["Info.plist"])
            info[field] = value
            files["Info.plist"] = plistlib.dumps(info)
            with self.subTest(field=field, value=value), self.assertRaises(signer.Refusal):
                signer.bundle_contract(files)
        for field, value in (("simulator", True), ("bundle_id", "other.app"),
                             ("gpu_execution_verified", True), ("inputs", {})):
            files = members()
            provenance = json.loads(files["BuildInfo.json"])
            provenance[field] = value
            files["BuildInfo.json"] = json.dumps(provenance).encode()
            with self.subTest(field=field), self.assertRaises(signer.Refusal):
                signer.bundle_contract(files)
        files = members()
        files["fixture.metal"] += b"// changed"
        with self.assertRaisesRegex(signer.Refusal, "fixture_mismatch"):
            signer.bundle_contract(files)

    def test_zip_alias_extra_nested_image_duplicate_and_link_are_refused(self):
        paths = ["../outside", signer.APP + "/../other", signer.APP + "/Data/game.esm",
                 signer.APP + "/Frameworks/Guest.framework/Guest", signer.APP + "/embedded.mobileprovision",
                 signer.APP + "/Info.plist", "Payload/"]
        for index, name in enumerate(paths):
            with self.subTest(name=name), warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                path = write_ipa(self.root / f"bad-{index}.ipa", extra=[(name, b"extra")])
                with self.assertRaises(signer.Refusal):
                    signer.read_unsigned(path.read_bytes())
        linked = zipfile.ZipInfo(signer.APP + "/MetalProbe")
        linked.create_system = 3
        linked.external_attr = (stat.S_IFLNK | 0o777) << 16
        path = write_ipa(self.root / "link.ipa", {k: v for k, v in members().items() if k != "MetalProbe"},
                         [(linked, b"outside")])
        with self.assertRaisesRegex(signer.Refusal, "zip_special"):
            signer.read_unsigned(path.read_bytes())

    def test_device_image_gate_rejects_simulator_old_target_signature_and_guest_dependency(self):
        for raw in (image(platform=7), image(minimum=18 << 16), image(minimum=(26 << 16) + 1),
                    image(cpu=0x01000007), image(signed=True), b"truncated",
                    image(dependency="@rpath/PrivateGuest.framework/PrivateGuest"),
                    image(dependency="/usr/lib/../../private/guest.dylib")):
            with self.subTest(raw_length=len(raw)), self.assertRaises(signer.Refusal):
                signer.device_image_contract(raw)
        signer.device_image_contract(image(dependency="/System/Library/Frameworks/Metal.framework/Metal"))
        unterminated = bytearray(image(dependency="/usr/lib/libc++.1.dylib"))
        command_offset = 32 + 24 + 24
        command_size = struct.unpack_from("<I", unterminated, command_offset + 4)[0]
        for offset in range(command_offset + 24, command_offset + command_size):
            if unterminated[offset] == 0:
                unterminated[offset] = ord("X")
        with self.assertRaisesRegex(signer.Refusal, "unterminated"):
            signer.device_image_contract(unterminated)

    def test_profile_accepts_only_matching_identity_device_and_minimal_claims(self):
        claims = signer.profile_contract(profile(), CERT, DEVICE, TEAM, NOW)
        self.assertEqual(set(claims), {"application-identifier", "com.apple.developer.team-identifier", "get-task-allow"})
        self.assertNotIn("keychain-access-groups", claims)
        self.assertEqual(claims["application-identifier"], TEAM + "." + signer.BUNDLE)

    def test_expired_future_foreign_and_enterprise_profiles_are_refused(self):
        changes = [{"ExpirationDate": NOW}, {"CreationDate": NOW + timedelta(seconds=1)},
                   {"ExpirationDate": None}, {"TeamIdentifier": ["OTHER00001"]},
                   {"ApplicationIdentifierPrefix": ["OTHER00001"]}, {"DeveloperCertificates": [b"other"]},
                   {"ProvisionedDevices": ["OTHER-DEVICE"]}, {"ProvisionedDevices": [DEVICE, DEVICE]},
                   {"ProvisionsAllDevices": True}, {"Platform": ["OSX"]}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(signer.Refusal):
                signer.profile_contract(dict(profile(), **change), CERT, DEVICE, TEAM, NOW)

    def test_wildcard_foreign_and_unneeded_entitlements_are_refused(self):
        changes = [{"application-identifier": TEAM + ".*"},
                   {"application-identifier": TEAM + ".org.skyrimios.title"},
                   {"com.apple.developer.team-identifier": "OTHER00001"}, {"get-task-allow": 1},
                   {"com.apple.developer.kernel.extended-virtual-addressing": True},
                   {"com.apple.developer.kernel.increased-memory-limit": True},
                   {"com.apple.security.application-groups": [TEAM + ".group"]},
                   {"keychain-access-groups": ["OTHER00001.*"]}]
        for change in changes:
            value = profile()
            value["Entitlements"].update(change)
            with self.subTest(change=change), self.assertRaises(signer.Refusal):
                signer.profile_contract(value, CERT, DEVICE, TEAM, NOW)

    def test_utc_naive_profile_dates_and_expiration_boundary(self):
        value = profile()
        value["CreationDate"] = value["CreationDate"].replace(tzinfo=None)
        value["ExpirationDate"] = value["ExpirationDate"].replace(tzinfo=None)
        signer.profile_contract(value, CERT, DEVICE, TEAM, NOW)
        with self.assertRaises(signer.Refusal):
            signer.profile_contract(value, CERT, DEVICE, TEAM, NOW + timedelta(days=1))

    def test_input_expected_hash_and_change_are_detected(self):
        path = self.root / "synthetic.input"
        path.write_bytes(b"first")
        with self.assertRaisesRegex(signer.Refusal, "hash_mismatch"):
            signer.Input(path, "0" * 64, 100)
        pinned = signer.Input(path, signer.digest(b"first"), 100)
        self.addCleanup(pinned.close)
        pinned.recheck()
        path.write_bytes(b"other")
        with self.assertRaisesRegex(signer.Refusal, "changed_during_signing"):
            pinned.recheck()

    def test_changed_helper_is_rejected_before_any_module_execution(self):
        path = self.root / "helper.py"
        path.write_text("raise RuntimeError('must not execute')\n")
        with self.assertRaisesRegex(signer.Refusal, "helper_pin_changed"):
            signer.load_pinned(path, "0" * 64, "never_loaded")
        path.write_text("VALUE = 7\n")
        self.assertEqual(signer.load_pinned(path, signer.digest(path.read_bytes()), "synthetic_helper").VALUE, 7)

    def test_outputs_are_private_separate_and_never_overwritten(self):
        private = self.root / "private/metal-probe-signing"
        private.mkdir(parents=True)
        output, work = private / "probe.ipa", private / "work"
        with patch.object(signer, "ROOT", self.root):
            self.assertEqual(signer.output_paths(output, work), (output, work))
            for bad_output, bad_work in ((self.root / "public.ipa", work), (output, self.root / "public-work"),
                                         (work / "probe.ipa", work), (output, output), (private / "probe.zip", work)):
                with self.subTest(output=bad_output.name), self.assertRaises(signer.Refusal):
                    signer.output_paths(bad_output, bad_work)
            output.write_bytes(b"previous artifact")
            with self.assertRaises(signer.Refusal):
                signer.output_paths(output, work)
            self.assertEqual(output.read_bytes(), b"previous artifact")

    def test_linked_input_and_output_parent_are_refused(self):
        target = self.root / "target"
        target.mkdir()
        linked = self.root / "linked"
        try:
            linked.symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("host does not permit synthetic symlinks")
        with self.assertRaisesRegex(signer.Refusal, "linked_path"):
            signer.plain(linked / "output.ipa")


class ProbeVerifiedPackagingTests(unittest.TestCase):
    """Hash-bound packaging is tested here; this does not assert a valid signature."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="metal-probe-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.app = self.root / "MetalProbe.app"
        self.files = dict(members(), **{"embedded.mobileprovision": b"synthetic profile",
                           "_CodeSignature/CodeResources": b"synthetic seal"})
        for name, raw in self.files.items():
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        self.expected = {name: signer.digest(raw) for name, raw in self.files.items()}

    def test_verified_member_hashes_and_executable_mode_survive_packaging(self):
        output = self.root / "synthetic.ipa"
        result = signer.publish_verified(self.app, output, self.expected)
        self.assertEqual(result, signer.digest(output.read_bytes()))
        with zipfile.ZipFile(output) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(set(archive.namelist()), {signer.APP + "/" + name for name in self.files})
            for name, raw in self.files.items():
                item = archive.getinfo(signer.APP + "/" + name)
                self.assertEqual(archive.read(item), raw)
                self.assertEqual(item.external_attr >> 16 & 0o777, 0o755 if name == "MetalProbe" else 0o644)

    def test_post_verification_tamper_never_publishes_output(self):
        (self.app / "fixture.metal").write_bytes(b"tampered after verification")
        output = self.root / "rejected.ipa"
        with self.assertRaisesRegex(signer.Refusal, "resource_changed"):
            signer.publish_verified(self.app, output, self.expected)
        self.assertFalse(output.exists())
        self.assertTrue(list(self.root.glob(".rejected.ipa.pending-*")), "preserve failed evidence")

    def test_existing_ipa_survives_publish_race(self):
        output = self.root / "existing.ipa"
        output.write_bytes(b"preserve previous IPA")
        with self.assertRaises(FileExistsError):
            signer.publish_verified(self.app, output, self.expected)
        self.assertEqual(output.read_bytes(), b"preserve previous IPA")

    def test_extra_or_missing_verified_file_is_refused(self):
        for expected in ({**self.expected, "game.esm": "0" * 64},
                         {k: v for k, v in self.expected.items() if k != "embedded.mobileprovision"}):
            with self.assertRaisesRegex(signer.Refusal, "verified_file_set"):
                signer.publish_verified(self.app, self.root / "rejected.ipa", expected)


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux-only controlled signing failure path")
class ProbeSigningFailureBoundary(unittest.TestCase):
    def test_ipa_path_substitution_and_restore_cannot_change_pinned_contents(self):
        with tempfile.TemporaryDirectory(prefix="metal-probe-input-substitution-") as directory:
            root = Path(directory).resolve()
            path = write_ipa(root / "input.ipa")
            saved = root / "original.ipa"
            original = members()
            foreign = members()
            provenance = json.loads(foreign["BuildInfo.json"])
            provenance["source_revision"] = "b" * 40
            foreign["BuildInfo.json"] = json.dumps(provenance).encode()
            replacement = write_ipa(root / "replacement.ipa", foreign)
            pin = signer.Input(path, signer.digest(path.read_bytes()), signer.MAX_IPA)
            try:
                path.rename(saved)
                replacement.rename(path)
                self.assertEqual(signer.read_unsigned(path.read_bytes()), foreign)
                self.assertEqual(signer.read_pinned_unsigned(pin), original)
                path.rename(replacement)
                saved.rename(path)
                # Restoring the pathname passes the end check; only reading the
                # pinned descriptor prevents the intervening substitution.
                pin.recheck()
            finally:
                pin.close()

    def test_ipa_descriptor_content_change_is_rejected_before_parsing(self):
        with tempfile.TemporaryDirectory(prefix="metal-probe-input-mutation-") as directory:
            path = write_ipa(Path(directory).resolve() / "input.ipa")
            pin = signer.Input(path, signer.digest(path.read_bytes()), signer.MAX_IPA)
            try:
                path.write_bytes(b"changed after initial verification")
                with patch.object(signer, "read_unsigned") as parse:
                    with self.assertRaisesRegex(signer.Refusal, "input_changed_during_signing"):
                        signer.read_pinned_unsigned(pin)
                    parse.assert_not_called()
            finally:
                pin.close()

    def test_crypto_failure_cannot_publish_or_claim_verified_ipa(self):
        with tempfile.TemporaryDirectory(prefix="metal-probe-failure-test-") as directory:
            root = Path(directory).resolve()
            private = root / "private/metal-probe-signing"
            private.mkdir(parents=True)
            files = {"input_ipa": write_ipa(root / "input.ipa"), "profile": root / "profile",
                     "certificate": root / "certificate", "key": root / "synthetic-key",
                     "device_record": root / "device.json"}
            for name, path in files.items():
                if name != "input_ipa":
                    path.write_bytes(json.dumps({"unique_device_id": DEVICE}).encode() if name == "device_record" else b"synthetic")
            tool = root / "synthetic-tool"
            tool.write_bytes(b"not executable")
            args = SimpleNamespace(**files, signer_helper=root / "helper.py", identity_verifier_dir=root,
                output=private / "output.ipa", work_dir=private / "work", timeout_seconds=30,
                expected_input_sha256=signer.digest(files["input_ipa"].read_bytes()),
                expected_profile_sha256=signer.digest(files["profile"].read_bytes()),
                expected_certificate_sha256=signer.digest(files["certificate"].read_bytes()),
                expected_device_record_sha256=signer.digest(files["device_record"].read_bytes()))
            core = SimpleNamespace(no_core=lambda: None, SIGNER=tool, NODUMP=tool,
                SIGNER_SHA=signer.digest(tool.read_bytes()), NODUMP_SHA=signer.digest(tool.read_bytes()),
                image=lambda *_: {}, Runner=lambda *_: SimpleNamespace(run=lambda *_: CERT),
                canonicalize=lambda *_: 1, verify_image=Mock(side_effect=ValueError("synthetic crypto rejection")))
            with patch.object(signer, "ROOT", root), patch.object(signer, "load_pinned", return_value=core), \
                    patch.object(signer, "verify_authority", return_value={}), patch.object(signer, "publish_verified") as publish:
                with self.assertRaisesRegex(ValueError, "synthetic crypto rejection"):
                    signer.sign(args)
            publish.assert_not_called()
            self.assertFalse(args.output.exists())
            report = json.loads((args.work_dir / "signing-result.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertFalse(report["crypto_verified"])
            self.assertFalse(report["installed"])


if __name__ == "__main__":
    unittest.main()
