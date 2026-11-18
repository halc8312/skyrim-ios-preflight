"""Synthetic packaging controls; no Apple compilation or device execution."""
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_phone_builder", ROOT / "scripts/build-metal-probe-ios.py")
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


def macho(*, platform=2, minimum=26 << 16, sdk=26 << 16, cpu=0x0100000C, subtype=0, signed=False):
    commands = struct.pack("<6I", 0x32, 24, platform, minimum, sdk, 0)
    commands += struct.pack("<2I", 0x1B, 24) + bytes(range(16))
    if signed:
        commands += struct.pack("<4I", 0x1D, 16, 96, 1)
    return struct.pack("<8I", 0xFEEDFACF, cpu, subtype, 2, 3 if signed else 2, len(commands), 0, 0) + commands + b"\0" * 32


class MetalPhoneBuildControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        for name in build.INPUTS:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// synthetic input\n", encoding="utf-8")
        (self.root / "experiments/metal-compile/ios/Info.plist").write_bytes(
            (ROOT / "experiments/metal-compile/ios/Info.plist").read_bytes())
        (self.root / "sdk").mkdir()
        self.calls = []

    def invoke(self, simulator=False, fail=None, mutate=False):
        def command(argv, label, output, report, env, timeout, *, deadline):
            self.calls.append((argv, label, dict(env), timeout, deadline))
            self.assertLessEqual(timeout, 180)
            self.assertNotIn("CFLAGS", env)
            self.assertNotIn("SKYRIM_PROFILE_PATH", env)
            if label.startswith("compile-"):
                self.assertIn("-DSKYRIM_METAL_RUNTIME_NO_MAIN", argv)
                self.assertIn("-fblocks", argv)
                self.assertIn("-fexceptions", argv)
                self.assertIn("-fobjc-exceptions", argv)
                if label == "compile-contention":
                    self.assertIn("-fno-objc-arc", argv)
                    self.assertNotIn("-fobjc-arc", argv)
                else:
                    self.assertIn("-fobjc-arc", argv)
                    self.assertNotIn("-fno-objc-arc", argv)
                self.assertIn("-Werror", argv)
                Path(argv[-1]).write_bytes(b"synthetic object")
            if label == "link-app":
                Path(argv[-1]).write_bytes(macho(platform=7 if simulator else 2))
                if mutate:
                    name = "fixture.metal" if mutate is True else mutate
                    (output / "inputs/experiments/metal-compile" / name).write_text("changed\n")
            stdout = {"source-revision": "a" * 40, "xcode-version": "Xcode 26.6",
                      "sdk-path": str(self.root / "sdk"), "sdk-version": "26.5",
                      "compiler-path": str(self.root / "clang++"), "compiler-version": "Apple clang 21"}.get(label, "")
            event = {"label": label, "argv": argv, "returncode": 1 if label == fail else 0, "timed_out": False}
            report["commands"].append(event)
            (output / (label + ".log")).write_text(stdout or "synthetic build log\n")
            return event, stdout, ""
        with patch.object(build.platform, "system", return_value="Darwin"), patch.dict(
                build.os.environ, {"CFLAGS": "injected", "SKYRIM_PROFILE_PATH": "private"}):
            return build.execute(self.root, simulator=simulator, command=command)

    def test_device_bundle_and_proof_flags(self):
        run = self.invoke()
        report = json.loads((run / "build-result.json").read_text())
        self.assertEqual(report["status"], "unsigned_iphone_build_passed")
        self.assertEqual(report["macho"]["platform"], 2)
        self.assertTrue(all(report[key] is False for key in build.FALSE_EVIDENCE))
        artifact = run / "artifacts/MetalProbe-unsigned.ipa"
        with zipfile.ZipFile(artifact) as archive:
            self.assertEqual(set(archive.namelist()), {"Payload/MetalProbe.app/" + name for name in build.APP_FILES})
            info = plistlib.loads(archive.read("Payload/MetalProbe.app/Info.plist"))
            self.assertEqual(info["CFBundleIdentifier"], build.DEFAULT_BUNDLE)
            self.assertEqual(info["CFBundleVersion"], "5")
            self.assertEqual(info["MinimumOSVersion"], "26.0")
            provenance = json.loads(archive.read("Payload/MetalProbe.app/BuildInfo.json"))
            self.assertEqual(len(provenance["inputs"]), 14)
            self.assertEqual(set(provenance["inputs"]), set(build.INPUTS))
            self.assertEqual(len(archive.infolist()), 5)
            self.assertEqual(archive.getinfo("Payload/MetalProbe.app/MetalProbe").external_attr >> 16 & 0o777, 0o755)
        with zipfile.ZipFile(run / "artifacts/metal-probe-build-evidence.zip") as archive:
            self.assertIn("build-result.json", archive.namelist())
            self.assertTrue(any(name.startswith("inputs/") for name in archive.namelist()))
            self.assertFalse(any(name.endswith(".ipa") or "Payload/" in name for name in archive.namelist()))

    def test_simulator_is_explicit_separate_artifact(self):
        run = self.invoke(simulator=True)
        report = json.loads((run / "build-result.json").read_text())
        self.assertEqual(report["status"], "unsigned_simulator_build_passed")
        self.assertEqual(report["macho"]["platform"], 7)
        self.assertFalse(list((run / "artifacts").glob("*.ipa")))
        with zipfile.ZipFile(run / "artifacts/MetalProbe-simulator.app.zip") as archive:
            self.assertIn("MetalProbe.app/MetalProbe", archive.namelist())
            self.assertEqual(plistlib.loads(archive.read("MetalProbe.app/Info.plist"))["CFBundleVersion"], "5")
        compile_calls = [args for args, label, *_ in self.calls if label.startswith("compile-")]
        self.assertTrue(all("arm64-apple-ios26.0-simulator" in args for args in compile_calls))

    def test_compile_failure_preserves_evidence_without_ipa(self):
        with self.assertRaisesRegex(ValueError, "compile-host failed"):
            self.invoke(fail="compile-host")
        run, = (self.root / "local/metal-probe-ios").iterdir()
        report = json.loads((run / "build-result.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertFalse(list((run / "artifacts").glob("*.ipa")))
        self.assertTrue((run / "artifacts/metal-probe-build-evidence.zip").is_file())
        self.assertNotIn("link-app", [call[1] for call in self.calls])

    def test_changed_source_snapshot_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "resource changed"):
            self.invoke(mutate=True)
        run, = (self.root / "local/metal-probe-ios").iterdir()
        self.assertFalse(list((run / "artifacts").glob("*.ipa")))

    def test_changed_phone_source_snapshot_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "resource changed: phone_fixture.metal"):
            self.invoke(mutate="phone_fixture.metal")
        run, = (self.root / "local/metal-probe-ios").iterdir()
        self.assertFalse(list((run / "artifacts").glob("*.ipa")))

    def test_changed_workload_header_snapshot_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "source snapshot changed"):
            self.invoke(mutate="phone_workloads.h")
        run, = (self.root / "local/metal-probe-ios").iterdir()
        self.assertFalse(list((run / "artifacts").glob("*.ipa")))

    def test_contention_uses_separate_mrc_object_and_stays_in_evidence(self):
        run = self.invoke()
        compiles = [(argv, label) for argv, label, *_ in self.calls if label.startswith("compile-")]
        self.assertEqual([label for _, label in compiles],
                         ["compile-runtime", "compile-host", "compile-store", "compile-contention"])
        contention, = [argv for argv, label in compiles if label == "compile-contention"]
        self.assertTrue(any(arg.endswith("/phone_contention.mm") or arg.endswith("\\phone_contention.mm")
                            for arg in contention))
        link, = [argv for argv, label, *_ in self.calls if label == "link-app"]
        self.assertIn(str(run / "objects/contention.o"), link)
        with zipfile.ZipFile(run / "artifacts/metal-probe-build-evidence.zip") as archive:
            for name in ("phone_contention.h", "phone_contention.mm"):
                self.assertIn("inputs/experiments/metal-compile/" + name, archive.namelist())

    def test_changed_contention_source_snapshot_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "source snapshot changed"):
            self.invoke(mutate="phone_contention.mm")
        run, = (self.root / "local/metal-probe-ios").iterdir()
        self.assertFalse(list((run / "artifacts").glob("*.ipa")))

    def test_old_or_forged_bundle_version_is_refused(self):
        template = self.root / "experiments/metal-compile/ios/Info.plist"
        value = plistlib.loads(template.read_bytes())
        for version in ("4", 5, "05", "5.0", "6"):
            with self.subTest(version=version):
                value["CFBundleVersion"] = version
                template.write_bytes(plistlib.dumps(value))
                with self.assertRaisesRegex(ValueError, "metadata mismatch: CFBundleVersion"):
                    self.invoke()
        for run in (self.root / "local/metal-probe-ios").iterdir():
            self.assertFalse(list((run / "artifacts").glob("*.ipa")))

    def test_source_provenance_cannot_omit_inject_or_change_fixture_hashes(self):
        run = self.invoke()
        app = run / "Payload/MetalProbe.app"
        path = app / "BuildInfo.json"
        original = json.loads(path.read_text())
        fixture_hashes = {name: original["inputs"]["experiments/metal-compile/" + name] for name in build.FIXTURES}
        base = original["inputs"]
        missing = dict(base)
        del missing["experiments/metal-compile/phone_workloads.h"]
        invalid = dict(base, **{"experiments/metal-compile/runtime.mm": "not a SHA256"})
        wrong_fixture = dict(base, **{"experiments/metal-compile/phone_fixture.metal": "0" * 64})
        for inputs in (None, [], missing, invalid, wrong_fixture, dict(base, **{"private.bin": "0" * 64})):
            path.write_text(json.dumps(dict(original, inputs=inputs)))
            with self.subTest(inputs_type=type(inputs).__name__), self.assertRaisesRegex(ValueError, "provenance"):
                build.validate_app(app, build.DEFAULT_BUNDLE, False, fixture_hashes)

    def test_two_builds_preserve_first_artifact(self):
        first = self.invoke()
        original = (first / "artifacts/MetalProbe-unsigned.ipa").read_bytes()
        second = self.invoke()
        self.assertNotEqual(first, second)
        self.assertEqual((first / "artifacts/MetalProbe-unsigned.ipa").read_bytes(), original)

    def test_unsupported_host_does_not_start_tools(self):
        with patch.object(build.platform, "system", return_value="Windows"):
            with self.assertRaisesRegex(ValueError, "requires macOS"):
                build.execute(self.root)
        self.assertFalse((self.root / "local").exists())

    def test_game_bundle_is_rejected(self):
        for bundle in ("org.skyrimios.title", "com.example.skyrim", "../metalprobe", "org.skyrimios.probe.extra"):
            with self.subTest(bundle=bundle), self.assertRaises(ValueError):
                build.validate_bundle(bundle)
        self.assertEqual(build.validate_bundle("org.skyrimios.probe"), "org.skyrimios.probe")

    def test_wrong_platform_arch_minimum_or_signature_rejected(self):
        for data in (b"short", macho(platform=7), macho(platform=1), macho(minimum=18 << 16),
                     macho(sdk=18 << 16), macho(cpu=0x01000007), macho(subtype=2), macho(signed=True)):
            with self.subTest(data=data[:24]), self.assertRaises(ValueError):
                build.validate_macho(data)
        self.assertEqual(build.validate_macho(macho())["platform"], 2)
        self.assertEqual(build.validate_macho(macho(platform=7), True)["platform"], 7)

    def test_malformed_load_commands_rejected(self):
        data = bytearray(macho())
        struct.pack_into("<I", data, 36, 9)
        with self.assertRaises(ValueError):
            build.validate_macho(data)
        data = bytearray(macho())
        struct.pack_into("<I", data, 52, 10)
        with self.assertRaises(ValueError):
            build.validate_macho(data)

    def test_linked_output_parent_rejected(self):
        (self.root / "outside").mkdir()
        try:
            (self.root / "local").symlink_to(self.root / "outside", target_is_directory=True)
        except OSError:
            self.skipTest("symlink creation unavailable on this host")
        with self.assertRaisesRegex(ValueError, "symlink"):
            build.fresh_run(self.root)
        self.assertEqual(list((self.root / "outside").iterdir()), [])

    def test_private_extra_member_prevents_packaging(self):
        run = self.invoke()
        app = run / "Payload/MetalProbe.app"
        (app / "embedded.mobileprovision").write_bytes(b"synthetic prohibited member")
        with self.assertRaisesRegex(ValueError, "exactly"):
            build.package_app(app, run / "extra.ipa", False)
        self.assertFalse((run / "extra.ipa").exists())


if __name__ == "__main__":
    unittest.main()
