"""Game-free host/guest provenance checks for the self-build packaging route."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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

    def test_rejects_non_host_or_non_device_artifact(self):
        for key, wrong in (("public_host_sdk_build", False), ("private_guest_included", True),
                           ("target", "arm64-apple-ios-simulator")):
            old = self.host_result[key]
            self.host_result[key] = wrong
            self.write_host_result()
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check()
            self.host_result[key] = old


if __name__ == "__main__":
    unittest.main()
