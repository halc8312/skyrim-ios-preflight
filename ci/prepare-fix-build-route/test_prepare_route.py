"""Authored preparation/refusal checks. No build, service or game execution."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location("route", Path(__file__).with_name("prepare_route.py"))
route = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(route)
COMMIT = "1" * 40


class PreparationTests(unittest.TestCase):
    def fixture(self, parent):
        repo = parent / "authored repo"
        (repo / "ci").mkdir(parents=True)
        setup = "#!/bin/bash\nset -euo pipefail\n" + route.HARNESS + "\n" + route.STAGE_CALL + "\n"
        prefix = "#!/bin/bash\nset -euo pipefail\n" + route.HARNESS + "\noriginal_unit_gates\noriginal_wgpu_builder\n"
        body = '  original_compile_and_package "$variant"\ndone\n'
        stages = prefix + route.LOOP + body + route.AFTER + "\nforbidden_simulator_execution\n"
        values = {"ci/run-ios-owned-host-preflight.sh": setup.encode(),
                  "ci/run-ios-owned-host-tests.sh": stages.encode()}
        for name, data in values.items():
            (repo / name).write_bytes(data)
        pins = {"reviewed_reference_commit": COMMIT,
                "files": {name: route.digest(data) for name, data in values.items()}}
        return repo, values, pins, prefix, body

    @staticmethod
    def git(_repo, *args):
        return {("rev-parse", "HEAD"): COMMIT,
                ("remote", "get-url", "origin"): route.ORIGIN,
                ("status", "--porcelain=v1", "--untracked-files=normal"): ""}[args]

    def test_reuses_original_prefix_and_device_block_only(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(route, "git", self.git):
            root = Path(temp)
            repo, values, pins, prefix, body = self.fixture(root)
            output = root / "fresh scripts"
            result = route.prepare(repo, COMMIT, output, pins)
            expected = (prefix + "for variant in device; do\n" + body).replace(
                route.HARNESS, "harness=" + route.shlex.quote(str(repo.resolve())))
            self.assertEqual((output / "device-stage.sh").read_bytes(), expected.encode())
            self.assertNotIn("forbidden_simulator_execution", expected)
            for name, data in values.items():
                self.assertEqual((repo / name).read_bytes(), data)
            self.assertFalse(result["simulator_xctest_executed"])
            self.assertFalse(result["audio_sdk_executed"])
            self.assertFalse(result["source_commit_signature_authenticated"])
            self.assertEqual(result["source_commit"], COMMIT)
            self.assertIn(route.shlex.quote(str(output.resolve() / "device-stage.sh")),
                          (output / "preflight.sh").read_text())

    def test_changed_pin_refuses_before_output(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(route, "git", self.git):
            root = Path(temp)
            repo, values, pins, _, _ = self.fixture(root)
            (repo / next(iter(values))).write_bytes(b"authored changed source")
            with self.assertRaisesRegex(ValueError, "reference_pin_mismatch"):
                route.prepare(repo, COMMIT, root / "not-created", pins)
            self.assertFalse((root / "not-created").exists())

    def test_exact_commit_and_clean_checkout_are_required(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo, _, pins, _, _ = self.fixture(root)
            for commit in ("main", "A" * 40, "1" * 39, "2" * 40):
                with self.subTest(commit=commit), patch.object(route, "git", self.git):
                    with self.assertRaises(ValueError):
                        route.prepare(repo, commit, root / "not-created", pins)
            def dirty(repo, *args):
                return "?? authored-change" if args[0] == "status" else self.git(repo, *args)
            with patch.object(route, "git", dirty), self.assertRaisesRegex(ValueError, "not_clean"):
                route.prepare(repo, COMMIT, root / "not-created", pins)
            self.assertFalse((root / "not-created").exists())
            for origin in route.ORIGINS:
                def selected(repo, *args):
                    return origin if args[0] == "remote" else self.git(repo, *args)
                with patch.object(route, "git", selected):
                    route.inputs(repo, COMMIT, pins)
            for origin in ("https://github.com/other/repo.git",
                           "https://github.com/halc8312-lookalike/skyrim-ios-preflight.git",
                           "https://github.com.evil.invalid/halc8312/skyrim-ios-preflight.git",
                           "https://user@github.com/halc8312/skyrim-ios-preflight.git"):
                def wrong_origin(repo, *args):
                    return origin if args[0] == "remote" else self.git(repo, *args)
                with patch.object(route, "git", wrong_origin), self.assertRaisesRegex(ValueError, "origin"):
                    route.inputs(repo, COMMIT, pins)

    def test_changed_or_ambiguous_extraction_anchors_refuse(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(route, "git", self.git):
            root = Path(temp)
            repo, values, pins, _, _ = self.fixture(root)
            name = "ci/run-ios-owned-host-tests.sh"
            for original, replacement in ((route.LOOP, route.LOOP * 2), ("done\n", "changed_done\n"),
                                           (route.HARNESS, "changed_harness")):
                data = values[name].decode().replace(original, replacement).encode()
                (repo / name).write_bytes(data)
                pins["files"][name] = route.digest(data)
                with self.assertRaises(ValueError):
                    route.prepare(repo, COMMIT, root / "not-created", pins)
                self.assertFalse((root / "not-created").exists())

    def test_output_must_be_fresh_and_outside_source(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(route, "git", self.git):
            root = Path(temp)
            repo, _, pins, _, _ = self.fixture(root)
            for output in (repo, repo / "generated", root):
                with self.assertRaisesRegex(ValueError, "fresh_separate"):
                    route.prepare(repo, COMMIT, output, pins)

    def test_workflow_is_manual_standard_bounded_and_unsigned(self):
        # Archived candidates are fixtures, not active workflows or quota advice.
        fixtures = Path(__file__).parent / "testdata"
        text = (fixtures / "ios-prepare-fix-build-only.yml").read_text()
        self.assertIn("workflow_dispatch:", text)
        self.assertIn("runs-on: macos-26\n", text)
        self.assertIn("timeout-minutes: 30", text)
        self.assertIn("persist-credentials: false", text)
        self.assertIn("python3 ci/run-bounded-command.py 1500", text)
        self.assertIn("SKYRT_OWNED_HOST_SCOPE='' python3", text)
        self.assertNotIn("pull_request:", text)
        self.assertNotIn("secrets.", text)
        self.assertNotIn("-xlarge", text)
        self.assertNotIn("-large", text)
        codemagic = (fixtures / "codemagic-build-only.fragment.yaml").read_text()
        self.assertIn("instance_type: mac_mini_m2", codemagic)
        self.assertIn("max_build_duration: 30", codemagic)
        self.assertIn("SKYRT_REVIEWED_SOURCE_COMMIT", codemagic)
        self.assertIn('"${CM_COMMIT:?Missing CI commit}"', codemagic)
        self.assertIn("python3 ci/run-bounded-command.py 1500", codemagic)
        self.assertIn("SKYRT_OWNED_HOST_SCOPE='' python3", codemagic)
        self.assertNotIn("triggering:", codemagic)

    def test_result_verification_binds_package_images_and_honest_scope(self):
        # Fake image bodies exercise only result/hash admission. The original
        # pinned packager, not this authored fixture, validates real Mach-O.
        with tempfile.TemporaryDirectory() as temp, patch.object(route, "git", self.git):
            root = Path(temp)
            repo, _, pins, _, _ = self.fixture(root)
            output = root / "derived"
            route.prepare(repo, COMMIT, output, pins)
            (root / "reference-pins.json").write_text(json.dumps(pins))
            built = repo / "out/owned-host"
            built.mkdir(parents=True)
            images = {name: ("authored-" + name).encode() for name in (
                "OwnedGameHost", "Frameworks/PrivateGuest.framework/PrivateGuest",
                "Frameworks/WgpuNative.framework/WgpuNative")}
            ipa = built / "OwnedGameHost-0.1.0-unsigned.ipa"
            with zipfile.ZipFile(ipa, "w") as archive:
                for name, data in images.items():
                    archive.writestr("Payload/OwnedGameHost.app/" + name, data)
            meta = dict(images={name: dict(bytes=len(data), sha256=route.digest(data))
                                for name, data in images.items()},
                        ipa_bytes=ipa.stat().st_size, ipa_sha256=route.digest(ipa.read_bytes()),
                        public_fixture_only=True, unsigned_device_ipa=True,
                        signature_commands_present=False, provisioning_profile_present=False)
            metadata = built / "device-ipa-metadata.json"
            metadata.write_text(json.dumps(meta))
            (built / "device-xcodebuild.log").write_text("** BUILD SUCCEEDED **\n")
            for name in ("public-host-tests.log", "public-xma-init-tests.log", "public-xma-device-tests.log",
                         "public-audio-observer-tests.log", "public-signed-policy-tests.log", "public-driver-tests.log",
                         "public-wgpu-build.log", "fixture-device-build.log", "host-device-build.log",
                         "device-ipa-audit.log"):
                (built / name).write_text("authored nonempty log\n")
            with patch.object(route, "ROOT", root):
                result = route.verify(repo, COMMIT, output)
                self.assertTrue(result["unsigned_device_build_succeeded"])
                for name in ("simulator_host_built", "simulator_xctest_executed", "audio_sdk_executed",
                             "physical_iphone_verified", "game_executed", "private_framework_packaged"):
                    self.assertIs(result[name], False)
                meta["images"]["OwnedGameHost"]["sha256"] = "0" * 64
                metadata.write_text(json.dumps(meta))
                with self.assertRaisesRegex(ValueError, "image_hash_mismatch"):
                    route.verify(repo, COMMIT, output)


if __name__ == "__main__":
    unittest.main()
