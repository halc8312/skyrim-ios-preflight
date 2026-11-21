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
        template = Path("ios/runtime-port/Info.plist.in")
        (self.root / template).parent.mkdir()
        (self.root / template).write_bytes((ROOT / template).read_bytes())
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

    def test_classic_guest_needs_both_metadata_and_the_fourth_header(self):
        key = "classic_skse_experiment"
        self.result.write_text(json.dumps({"candidate_sha256": self.input_hash, key: True}))
        with self.assertRaisesRegex(ValueError, "Classic ABI mismatch"):
            self.check()
        self.host_result[key] = True
        self.write_host_result()
        with self.assertRaisesRegex(ValueError, "Classic ABI mismatch"):
            self.check()
        path = self.build / "compile-contract.json"
        contract = json.loads(path.read_text())
        extension = self.root / "ios/private-abi/PcGuestSkseABI.h"
        extension.write_bytes(b"authored classic binding\n")
        host_header = self.host / extension.name
        host_header.write_bytes(b"authored classic binding\r\n")
        contract.update(classic_skse_experiment=True, classic_abi_header_sha256=assembly.sha(extension))
        path.write_text(json.dumps(contract))
        self.check()
        host_header.write_bytes(b"wrong host declaration")
        with self.assertRaisesRegex(ValueError, "Classic binding header mismatch"):
            self.check()
        host_header.write_bytes(extension.read_bytes())
        contract["classic_abi_header_sha256"] = "f" * 64
        path.write_text(json.dumps(contract))
        with self.assertRaisesRegex(ValueError, "Classic binding header mismatch"):
            self.check()
        for invalid in (1, "true", None):
            self.host_result[key] = invalid
            self.write_host_result()
            with self.subTest(marker=invalid), self.assertRaisesRegex(ValueError, "Invalid Classic"):
                self.check()

    def classic_contract(self):
        path = self.build / "compile-contract.json"
        contract = json.loads(path.read_text())
        extension = self.root / "ios/private-abi/PcGuestSkseABI.h"
        extension.write_bytes(b"authored classic binding\n")
        (self.host / extension.name).write_bytes(extension.read_bytes())
        contract.update(classic_skse_experiment=True, classic_abi_header_sha256=assembly.sha(extension))
        return path, contract

    def test_mod_hook_guest_needs_host_abi_groups_and_all_five_headers(self):
        path, contract = self.classic_contract()
        hashes = {}
        for name, folder in assembly.MOD_HOOK_HEADERS.items():
            source = self.root / folder / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"authored " + name.encode() + b"\n")
            (self.host / name).write_bytes(source.read_bytes().replace(b"\n", b"\r\n"))
            hashes[name] = assembly.sha(source)
        groups = 3 | 64 | 128 | 256 | 512  # Include the fresh-save witness.
        contract.update(mod_hook_groups=groups, mod_hooks_header_sha256=hashes)
        path.write_text(json.dumps(contract))
        guest = {"candidate_sha256": self.input_hash, "classic_skse_experiment": True, "mod_hook_groups": groups}
        self.result.write_text(json.dumps(guest))
        self.host_result["classic_skse_experiment"] = True
        self.write_host_result()
        with self.assertRaisesRegex(ValueError, "MOD hook ABI"):
            self.check()
        self.host_result["mod_hooks_abi"] = 1
        self.write_host_result()
        self.check()
        # A hook-capable host still accepts a Classic guest without hooks.
        self.result.write_text(json.dumps(dict(guest, mod_hook_groups=0)))
        with self.assertRaisesRegex(ValueError, "compile contract"):
            self.check()
        del contract["mod_hook_groups"]
        path.write_text(json.dumps(contract))
        self.check()
        contract["mod_hook_groups"] = groups
        path.write_text(json.dumps(contract))
        self.result.write_text(json.dumps(guest))
        for name in assembly.MOD_HOOK_HEADERS:
            host_copy = self.host / name
            original = host_copy.read_bytes()
            host_copy.write_bytes(original + b"changed\n")
            with self.subTest(header=name), self.assertRaisesRegex(ValueError, "MOD hook header mismatch"):
                self.check()
            host_copy.write_bytes(original)
        recorded = dict(hashes)
        recorded.pop("voice_fallback.h")
        contract["mod_hooks_header_sha256"] = recorded
        path.write_text(json.dumps(contract))
        with self.assertRaisesRegex(ValueError, "MOD hook header mismatch"):
            self.check()
        contract["mod_hooks_header_sha256"] = hashes
        path.write_text(json.dumps(contract))
        self.assertEqual(assembly.mod_hook_groups({"mod_hook_groups": 4164}), 4164)
        self.assertEqual(assembly.mod_hook_groups({"mod_hook_groups": 18432}), 18432)
        self.assertEqual(assembly.mod_hook_groups({"mod_hook_groups": 32772}), 32772)
        self.assertEqual(assembly.mod_hook_groups({"mod_hook_groups": 65536}), 65536)
        for invalid in (True, "3", 512, 4096, 16384, 32768, 131072, 2, -1):
            self.result.write_text(json.dumps(dict(guest, mod_hook_groups=invalid)))
            with self.subTest(groups=invalid), self.assertRaisesRegex(ValueError, "Invalid MOD hook groups"):
                self.check()
        self.result.write_text(json.dumps(guest))
        for invalid in (True, 2, "1"):
            self.host_result["mod_hooks_abi"] = invalid
            self.write_host_result()
            with self.subTest(abi=invalid), self.assertRaisesRegex(ValueError, "Invalid MOD hook ABI"):
                self.check()
        self.host_result["mod_hooks_abi"] = 1
        self.write_host_result()
        # Hooks without the Classic guest are refused.
        self.result.write_text(json.dumps(dict(guest, classic_skse_experiment=False)))
        self.host_result["classic_skse_experiment"] = False
        self.write_host_result()
        contract["classic_skse_experiment"] = False
        path.write_text(json.dumps(contract))
        with self.assertRaisesRegex(ValueError, "Classic host"):
            self.check()

    def test_packages_mod_hook_guest_only_with_matching_plists(self):
        path, contract = self.classic_contract()
        hashes = {}
        for name, folder in assembly.MOD_HOOK_HEADERS.items():
            source = self.root / folder / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"authored " + name.encode() + b"\n")
            (self.host / name).write_bytes(source.read_bytes())
            hashes[name] = assembly.sha(source)
        contract.update(mod_hook_groups=31, mod_hooks_header_sha256=hashes)
        path.write_text(json.dumps(contract))
        framework = self.build / "SkyrimGuest.framework"
        framework.mkdir()
        guest = framework / "SkyrimGuest"
        guest.write_bytes(b"synthetic guest")
        game = self.stage / "owned-input"
        game.write_bytes(b"synthetic game input")
        self.result.write_text(json.dumps({"candidate_sha256": self.input_hash, "sha256": assembly.sha(guest),
            "strict_link_succeeded": True, "signed": False, "framework": str(framework),
            "classic_skse_experiment": True, "mod_hook_groups": 31}))
        self.host_result.update(classic_skse_experiment=True, mod_hooks_abi=1)
        archive = self.host / "SkyrimIOS-public-host.zip"
        current = plistlib.loads((ROOT / "ios/runtime-port/Info.plist.in").read_bytes())["CFBundleVersion"]
        for index, (host_abi, guest_groups, error) in enumerate((
                (1, 31, None), (None, 31, "lacks the MOD hook ABI"), (1, 15, "framework MOD hook groups"),
                (1, None, "framework MOD hook groups"), (True, 31, "Invalid MOD hook ABI"))):
            with self.subTest(host=host_abi, guest=guest_groups):
                info = dict(CFBundleIdentifier="org.skyrimios.pc2011", CFBundleVersion=current,
                            CFBundleExecutable="SkyrimIOS", PcGuestClassicSkseExperiment=True)
                if host_abi is not None:
                    info["PcGuestModHooksABI"] = host_abi
                with zipfile.ZipFile(archive, "w") as package:
                    package.writestr("SkyrimIOS.app/Info.plist", plistlib.dumps(info))
                    package.writestr("SkyrimIOS.app/SkyrimIOS", b"synthetic host")
                framework_info = {"PcGuestClassicSkseExperiment": True}
                if guest_groups is not None:
                    framework_info["PcGuestModHooks"] = guest_groups
                (framework / "Info.plist").write_bytes(plistlib.dumps(framework_info))
                self.host_result["artifact_sha256"] = assembly.sha(archive)
                self.write_host_result()
                out = self.root / f"hooks-{index}.ipa"
                argv = ["assemble_title_ipa.py", "--host-artifacts", str(self.host), "--guest-result",
                        str(self.result), "--input", str(game), "--input-sha256", self.input_hash,
                        "--output", str(out)]
                with patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                    if error:
                        with self.assertRaisesRegex(ValueError, error):
                            assembly.main()
                        self.assertFalse(out.exists())
                    else:
                        assembly.main()
                        with zipfile.ZipFile(out) as package:
                            actual = plistlib.loads(package.read(assembly.APP + "Info.plist"))
                            self.assertEqual(actual["PcGuestModHooksABI"], 1)

    def test_packages_existing_classic_capability_for_only_the_standard_product(self):
        key = "PcGuestClassicSkseExperiment"
        framework = self.build / "SkyrimGuest.framework"
        framework.mkdir()
        guest = framework / "SkyrimGuest"
        guest.write_bytes(b"synthetic guest")
        game = self.stage / "owned-input"
        game.write_bytes(b"synthetic game input")
        source = self.root / "ios/private-abi/PcGuestSkseABI.h"
        source.write_bytes(b"authored classic ABI")
        (self.host / source.name).write_bytes(source.read_bytes())
        path = self.build / "compile-contract.json"
        contract = json.loads(path.read_text())
        contract.update(classic_skse_experiment=True, classic_abi_header_sha256=assembly.sha(source))
        path.write_text(json.dumps(contract))
        self.result.write_text(json.dumps({"candidate_sha256": self.input_hash, "sha256": assembly.sha(guest),
            "strict_link_succeeded": True, "signed": False, "framework": str(framework),
            "classic_skse_experiment": True}))
        self.host_result["classic_skse_experiment"] = True
        archive = self.host / "SkyrimIOS-public-host.zip"
        normal_id = "org.skyrimios.pc2011"
        current = plistlib.loads((ROOT / "ios/runtime-port/Info.plist.in").read_bytes())["CFBundleVersion"]
        cases = [
            (normal_id, True, True, False, None),
            (normal_id, True, True, True, "--host-artifacts"),
            (normal_id, True, False, False, "framework Classic ABI mismatch"),
            (normal_id, False, True, False, "Host/guest Classic ABI mismatch"),
            (normal_id, "1", True, False, "Invalid Classic"),
            (normal_id+".classic-lab", True, True, False, "identity mismatch"),
        ]
        for index, (bundle_id, host_marker, guest_marker, raw, error) in enumerate(cases):
            with self.subTest(bundle=bundle_id, host_marker=host_marker, guest_marker=guest_marker, raw=raw):
                info = dict(CFBundleIdentifier=bundle_id, CFBundleVersion=current, CFBundleExecutable="SkyrimIOS")
                info[key] = host_marker
                with zipfile.ZipFile(archive, "w") as package:
                    package.writestr("SkyrimIOS.app/Info.plist", plistlib.dumps(info))
                    package.writestr("SkyrimIOS.app/SkyrimIOS", b"synthetic host")
                (framework / "Info.plist").write_bytes(plistlib.dumps({key:guest_marker}))
                self.host_result["artifact_sha256"] = assembly.sha(archive)
                self.write_host_result()
                out = self.root / f"output-{index}.ipa"
                selector = ["--host-zip", str(archive), "--host-sha256", assembly.sha(archive)] if raw else [
                    "--host-artifacts", str(self.host)]
                argv = ["assemble_title_ipa.py", *selector, "--guest-result", str(self.result),
                        "--input", str(game), "--input-sha256", self.input_hash, "--output", str(out)]
                with patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                    if error:
                        with self.assertRaisesRegex(ValueError, error):
                            assembly.main()
                        self.assertFalse(out.exists())
                    else:
                        assembly.main()
                        with zipfile.ZipFile(out) as package:
                            actual = plistlib.loads(package.read(assembly.APP + "Info.plist"))
                            self.assertEqual(actual["CFBundleIdentifier"], normal_id)
                            self.assertIs(actual[key], True)
                            self.assertEqual(package.read(assembly.APP + "Frameworks/SkyrimGuest.framework/SkyrimGuest"),
                                             guest.read_bytes())

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
