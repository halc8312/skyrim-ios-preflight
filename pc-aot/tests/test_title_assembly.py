"""Game-free host/guest provenance checks for the self-build packaging route."""
import hashlib
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("title_assembly", ROOT / "scripts/assemble_title_ipa.py")
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)


class ArtifactBindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.host = self.root / "host"
        self.host.mkdir()
        self.stage = self.root / "private/stage"
        runtime = self.stage / "source/runtime"
        runtime.mkdir(parents=True)
        self.build = self.stage / "arm64-ios"
        self.build.mkdir()
        abi = self.root / "ios/private-abi/PcGuestABI.h"
        abi.parent.mkdir(parents=True)
        abi.write_bytes(b"synthetic binding\n")
        (runtime / "x86.h").write_bytes(b"synthetic cpu\r\n")
        (runtime / "rt_table.h").write_bytes(b"extern const FnModule g_modules[];\n")
        (self.host / "x86.h").write_bytes(b"synthetic cpu\n")
        (self.host / "rt_table.h").write_bytes(
            b"extern FnModule g_modules[]; /* assigned once before any guest thread starts */\n")
        (self.host / "PcGuestABI.h").write_bytes(abi.read_bytes())
        (self.host / "SkyrimIOS-public-host.zip").write_bytes(b"synthetic host archive")
        self.input_hash = hashlib.sha256(b"synthetic game input").hexdigest()
        self.result = self.build / "result.json"
        self.result.write_text(json.dumps({"candidate_sha256": self.input_hash}))
        (self.build / "compile-contract.json").write_text(json.dumps({
            "headers": {name: assembly.sha(runtime / name) for name in ("x86.h", "rt_table.h")},
            "abi_header_sha256": assembly.sha(abi)}))
        self.host_result = {"public_host_sdk_build": True, "private_guest_included": False,
                            "target": "arm64-apple-ios",
                            "artifact_sha256": assembly.sha(self.host / "SkyrimIOS-public-host.zip")}
        self.write_host_result()
        self.root_patch = patch.object(assembly, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def write_host_result(self):
        (self.host / "result.json").write_text(json.dumps(self.host_result))

    def check(self):
        return assembly.host_artifacts(self.host, self.result, self.input_hash)

    def test_accepts_exact_binding_and_only_known_declaration_adapter(self):
        path, digest = self.check()
        self.assertEqual(path, self.host / "SkyrimIOS-public-host.zip")
        self.assertEqual(digest, assembly.sha(path))

    def test_rejects_archive_change_after_host_result(self):
        (self.host / "SkyrimIOS-public-host.zip").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "verified public"):
            self.check()

    def test_rejects_guest_from_other_input(self):
        self.result.write_text(json.dumps({"candidate_sha256": "0" * 64}))
        with self.assertRaisesRegex(ValueError, "owned input"):
            self.check()

    def test_rejects_host_cpu_or_binding_layout_change(self):
        for name in ("x86.h", "rt_table.h", "PcGuestABI.h"):
            path = self.host / name
            original = path.read_bytes()
            path.write_bytes(original + b"changed ABI\n")
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.check()
            path.write_bytes(original)

    def test_rejects_guest_headers_changed_after_compile(self):
        (self.stage / "source/runtime/x86.h").write_bytes(b"modified CPU\n")
        with self.assertRaisesRegex(ValueError, "after compilation"):
            self.check()

    def test_classic_requires_matching_host_contract_and_reviewed_extra_header(self):
        key = "classic_skse_experiment"
        self.result.write_text(json.dumps({"candidate_sha256": self.input_hash, key: True}))
        with self.assertRaisesRegex(ValueError, "experiment mismatch"):
            self.check()
        self.host_result[key] = True
        self.write_host_result()
        with self.assertRaisesRegex(ValueError, "experiment mismatch"):
            self.check()
        path = self.build / "compile-contract.json"
        contract = json.loads(path.read_text())
        extension = self.root / "ios/private-abi/PcGuestSkseABI.h"
        extension.write_bytes(b"authored classic declaration\n")
        host_header = self.host / extension.name
        host_header.write_bytes(b"authored classic declaration\r\n")
        contract.update(classic_skse_experiment=True, classic_abi_header_sha256=assembly.sha(extension))
        path.write_text(json.dumps(contract))
        self.assertEqual(self.check()[0], self.host / "SkyrimIOS-public-host.zip")
        host_header.write_bytes(b"wrong declaration\n")
        with self.assertRaisesRegex(ValueError, "Classic binding header mismatch"):
            self.check()
        for wrong in (1, "true", None):
            self.host_result[key] = wrong
            self.write_host_result()
            with self.subTest(marker=wrong), self.assertRaisesRegex(ValueError, "Invalid Classic"):
                self.check()

    def test_rejects_non_host_or_non_device_artifact(self):
        for key, wrong in (("public_host_sdk_build", False), ("private_guest_included", True),
                           ("target", "arm64-apple-ios-simulator")):
            old = self.host_result[key]
            self.host_result[key] = wrong
            self.write_host_result()
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check()
            self.host_result[key] = old


class PackageIdentityTests(unittest.TestCase):
    def test_raw_archive_and_embedded_framework_must_match_classic_result(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            game = base / "authored-input"
            game.write_bytes(b"test")
            framework = base / "SkyrimGuest.framework"
            framework.mkdir()
            binary = framework / "SkyrimGuest"
            binary.write_bytes(b"fixture")
            archive, result, output = (base / name for name in ("host.zip", "result.json", "out.ipa"))
            for host, guest, manifest, separate_id, succeeds in (
                    (False, True, True, False, False), (True, False, False, True, False),
                    (True, False, True, True, False), (True, True, True, False, False),
                    (True, True, True, True, True)):
                with self.subTest(host=host, guest=guest, manifest=manifest):
                    result.write_text(json.dumps({"framework": str(framework), "strict_link_succeeded": True,
                        "signed": False, "sha256": assembly.sha(binary), "classic_skse_experiment": manifest}))
                    (framework / "Info.plist").write_bytes(plistlib.dumps(
                        {"CFBundleExecutable": "SkyrimGuest", "PcGuestClassicSkseExperiment": guest}))
                    info = plistlib.loads((ROOT / "ios/runtime-port/Info.plist.in").read_bytes())
                    info.update(CFBundleExecutable="SkyrimIOS", CFBundleIdentifier="org.skyrimios.pc2011",
                                PcGuestClassicSkseExperiment=host)
                    if separate_id:
                        info["CFBundleIdentifier"] += ".classic-lab"
                    with zipfile.ZipFile(archive, "w") as z:
                        z.writestr("SkyrimIOS.app/Info.plist", plistlib.dumps(info))
                        z.writestr("SkyrimIOS.app/SkyrimIOS", b"host")
                    argv = ["assemble_title_ipa.py", "--host-zip", str(archive), "--host-sha256",
                            assembly.sha(archive), "--guest-result", str(result), "--input", str(game),
                            "--input-sha256", assembly.sha(game), "--output", str(output)]
                    with patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                        if succeeds:
                            assembly.main()
                        else:
                            with self.assertRaisesRegex(ValueError, "experiment mismatch|identity mismatch"):
                                assembly.main()
                    self.assertEqual(output.exists(), succeeds)

    def test_packages_current_app_build_and_rejects_other_versions(self):
        info = plistlib.loads((ROOT / "ios/runtime-port/Info.plist.in").read_bytes())
        current = str(info["CFBundleVersion"])
        info.update(CFBundleExecutable="SkyrimIOS", CFBundleIdentifier="org.skyrimios.pc2011")
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            game = base / "synthetic-input"
            game.write_bytes(b"authored input fixture")
            framework = base / "SkyrimGuest.framework"
            framework.mkdir()
            (framework / "SkyrimGuest").write_bytes(b"authored guest fixture")
            (framework / "Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable": "SkyrimGuest"}))
            result = base / "guest-result.json"
            result.write_text(json.dumps({"framework": str(framework), "strict_link_succeeded": True,
                                          "signed": False, "sha256": assembly.sha(framework / "SkyrimGuest")}))
            for version in (current, str(int(current) - 1), str(int(current) + 1)):
                with self.subTest(version=version):
                    archive = base / (version + ".zip")
                    output = base / (version + ".ipa")
                    app_info = dict(info, CFBundleVersion=version)
                    with zipfile.ZipFile(archive, "w") as package:
                        package.writestr("SkyrimIOS.app/Info.plist", plistlib.dumps(app_info))
                        package.writestr("SkyrimIOS.app/SkyrimIOS", b"authored host fixture")
                    argv = ["assemble_title_ipa.py", "--host-zip", str(archive),
                            "--host-sha256", assembly.sha(archive), "--guest-result", str(result),
                            "--input", str(game), "--input-sha256", assembly.sha(game),
                            "--output", str(output)]
                    with patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                        if version != current:
                            with self.assertRaisesRegex(ValueError, "Public-host identity mismatch"):
                                assembly.main()
                            self.assertFalse(output.exists())
                            continue
                        assembly.main()
                    with zipfile.ZipFile(output) as package:
                        actual = plistlib.loads(package.read(assembly.APP + "Info.plist"))
                        self.assertEqual(actual["CFBundleVersion"], current)
                        self.assertEqual(actual["PcGuestExpectedInputSHA256"], assembly.sha(game))
                        self.assertEqual(package.read(assembly.APP + "Frameworks/SkyrimGuest.framework/SkyrimGuest"),
                                         (framework / "SkyrimGuest").read_bytes())


if __name__ == "__main__":
    unittest.main()
