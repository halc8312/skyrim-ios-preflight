"""Authored parser/staging/refusal controls. No Apple tools or game inputs."""
import copy
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
d = load("test_v2_runner", HERE / "run_v2_sdk_tests.py")
s = load("test_v2_stage", HERE / "prepare_v2_sdk_stage.py")
b = load("test_v2_builder", HERE / "build_public_sdk_fixture.py")
old_tests = load("test_v2_old_public_data", HERE / "test_run_sdk_tests.py")

class V2FixtureTests(unittest.TestCase):
    def report(self, normal_play=False):
        hashes, result = old_tests.PublicDriverTests().report()
        result.update(kind="public_owned_host_v2_fullchain_xctest", v2_proof=copy.deepcopy(d.PROOF),
                      audio_output_verified=False, xma_pcm_verified=False)
        if normal_play:
            result.update(kind="public_owned_host_v2_normal_play_xctest", normal_play_api_invoked=True)
        return hashes, result

    def console(self, result, normal_play=False):
        case = d.NORMAL_CASE if normal_play else d.CASE
        prefix = d.NORMAL_PREFIX if normal_play else d.PREFIX
        return (f"Test Case '{case}' started.\n" + prefix + json.dumps(result) + "\n"
                f"Test Case '{case}' passed (0.1 seconds).\n"
                "Executed 1 test, with 0 failures (0 unexpected) in 0.1 seconds\n"
                "** TEST EXECUTE SUCCEEDED **\n")

    def test_exact_one_case_and_all_typed_outcomes(self):
        hashes, report = self.report()
        self.assertEqual(d.validate_result(self.console(report), hashes), report)
        for key, value in report.items():
            bad = copy.deepcopy(report)
            bad.pop(key)
            with self.subTest(missing=key), self.assertRaises(ValueError):
                d.validate_result(self.console(bad), hashes)
        for key in d.PROOF:
            for value in (True, False, d.PROOF[key] + 1, float(d.PROOF[key])):
                bad = copy.deepcopy(report)
                bad["v2_proof"][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    d.validate_result(self.console(bad), hashes)
        for key in ("game_executed", "physical_iphone_verified", "audio_output_verified", "xma_pcm_verified"):
            bad = dict(report, **{key: True})
            with self.assertRaises(ValueError):
                d.validate_result(self.console(bad), hashes)

    def test_missing_duplicate_failed_skipped_or_v1_case_refuses(self):
        hashes, report = self.report()
        text = self.console(report)
        for bad in (text + text, text.replace("started.", "skipped."), text.replace("passed (", "failed ("),
                    text.replace(d.CASE, d.sdk.CASE), text.replace("Executed 1 test", "Executed 2 tests"),
                    text.replace("0 failures", "1 failure"), text.replace("EXECUTE SUCCEEDED", "SUCCEEDED"),
                    text.replace(d.PREFIX, "MISSING "), text + d.PREFIX + json.dumps(report) + "\n",
                    text + "Modifying properties of a view's layer off the main thread\n",
                    text.replace('"provider_refusals": 10', '"provider_refusals": 10, "provider_refusals": 10')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                d.validate_result(bad, hashes)

    def test_fixed_numeric_stage_diagnostics_never_replace_success_evidence(self):
        hashes, report = self.report()
        stages = (10, 11, 20, 21, 30, 31, 40, 41, 50, 51)
        markers = "".join("OWNED_HOST_PUBLIC_V2_STAGE " + str(stage) + "\n" for stage in stages)
        self.assertEqual(d.validate_result(markers + self.console(report), hashes), report)
        for console in (markers, markers + self.console(report).replace("passed (", "failed ("),
                        markers + self.console(report).replace(d.PREFIX, "MISSING ")):
            with self.assertRaises(ValueError): d.validate_result(console, hashes)
        swift = (HERE / "tests/OwnedHostV2FullChainTests.swift").read_text()
        self.assertEqual([int(value) for value in re.findall(r"v2Stage\((\d+)\)", swift)], list(stages))
        self.assertIn('print("OWNED_HOST_PUBLIC_V2_STAGE \\(stage)")\n    fflush(stdout)', swift)
        self.assertIn("watchdog.schedule(deadline: .now() + 35)", swift)
        self.assertIn("executionTimeAllowance = 40", swift)

    def test_independent_summary_has_one_pass_no_skips_and_strict_ints(self):
        summary = dict(totalTestCount=1, passedTests=1, failedTests=0, skippedTests=0, expectedFailures=0)
        d.validate_summary(summary)
        for bad in ([], True, None):
            with self.assertRaises(ValueError): d.validate_summary(bad)
        for key in summary:
            for value in (True, float(summary[key]), summary[key] + 1):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    d.validate_summary(dict(summary, **{key: value}))
            bad = summary.copy(); del bad[key]
            with self.assertRaises(ValueError): d.validate_summary(bad)

    def test_command_has_only_v2_selector_and_preserves_timeouts_signing(self):
        command = d.make_test_command(["xcodebuild", "-destination", "exact"], ["CODE_SIGNING_ALLOWED=NO"], Path("new.xcresult"))
        self.assertEqual([x for x in command if x.startswith("-only-testing:")],
                         ["-only-testing:OwnedGameHostTests/OwnedHostV2FullChainTests/" + d.TEST])
        self.assertEqual(command[-1], "test-without-building")
        self.assertEqual(command[command.index("-parallel-testing-enabled") + 1], "NO")
        self.assertEqual(command[command.index("-maximum-test-execution-time-allowance") + 1], "40")
        self.assertIn("CODE_SIGNING_ALLOWED=NO", command)
        original = (HERE / "tests/OwnedHostV2FullChainTests.swift").read_text()
        self.assertIn("func " + d.TEST + "()", original)
        self.assertTrue(original.startswith("#if PUBLIC_V2_SDK_FIXTURE\n"))

    def test_normal_play_requires_its_exact_typed_api_and_proof_contract(self):
        hashes, report = self.report(normal_play=True)
        self.assertEqual(d.validate_result(self.console(report, True), hashes, True), report)
        for key in report:
            bad = copy.deepcopy(report); del bad[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                d.validate_result(self.console(bad, True), hashes, True)
        for value in (False, 1, 0, "true", None):
            bad = dict(report, normal_play_api_invoked=value)
            with self.subTest(api=value), self.assertRaises(ValueError):
                d.validate_result(self.console(bad, True), hashes, True)
        for key in d.PROOF:
            bad = copy.deepcopy(report); bad["v2_proof"][key] += 1
            with self.subTest(proof=key), self.assertRaises(ValueError):
                d.validate_result(self.console(bad, True), hashes, True)
        for key in ("game_executed", "physical_iphone_verified", "audio_output_verified", "xma_pcm_verified"):
            with self.subTest(claim=key), self.assertRaises(ValueError):
                d.validate_result(self.console(dict(report, **{key: True}), True), hashes, True)
        with self.assertRaises(ValueError):
            d.validate_result(self.console(dict(report, unexpected=True), True), hashes, True)

    def test_normal_play_rejects_diagnostic_fallback_extra_cases_and_missing_success(self):
        hashes, report = self.report(True)
        _, diagnostic = self.report()
        text = self.console(report, True)
        for bad in (self.console(diagnostic), self.console(diagnostic, True),
                    text.replace(d.NORMAL_PREFIX, d.PREFIX), text.replace(d.NORMAL_CASE, d.CASE),
                    text.replace("passed (", "failed ("), text.replace("started.", "skipped."),
                    text.replace("Executed 1 test", "Executed 2 tests"),
                    text.replace("0 failures", "1 failure"), text.replace("EXECUTE SUCCEEDED", "EXECUTE FAILED"),
                    text + text, text + self.console(diagnostic),
                    text + d.PREFIX + json.dumps(diagnostic) + "\n",
                    text + d.NORMAL_PREFIX + json.dumps(report) + "\n"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                d.validate_result(bad, hashes, True)
        # Normal evidence must not be relabeled as the original diagnostic gate.
        for bad in (text, self.console(report),
                    self.console(diagnostic) + d.NORMAL_PREFIX + json.dumps(report) + "\n"):
            with self.assertRaises(ValueError): d.validate_result(bad, hashes)

    def test_normal_selector_changes_only_selected_test_and_keeps_external_limits(self):
        args = (["xcodebuild", "-destination", "exact"], ["CODE_SIGNING_ALLOWED=NO"], Path("new.xcresult"))
        diagnostic = d.make_test_command(*args)
        normal = d.make_test_command(*args, normal_play=True)
        self.assertEqual(normal, [part.replace("/" + d.TEST, "/" + d.NORMAL_TEST) for part in diagnostic])
        self.assertEqual([x for x in normal if x.startswith("-only-testing:")],
                         ["-only-testing:OwnedGameHostTests/OwnedHostV2FullChainTests/" + d.NORMAL_TEST])
        swift = (HERE / "tests/OwnedHostV2FullChainTests.swift").read_text()
        self.assertRegex(swift, r"func " + d.TEST + r"\(\) async throws \{\s*try await runPublicFixture\(mode: \.diagnostic\)")
        self.assertRegex(swift, r"func " + d.NORMAL_TEST + r"\(\) async throws \{\s*try await runPublicFixture\(mode: \.normalPlay\)")
        self.assertIn("case .diagnostic: return (OHStartV1(token, 30), false)", swift)
        self.assertIn("case .normalPlay: return (OHPlayV1(token), true)", swift)
        self.assertIn('normalPlay ? "GameNormalPlay" : "Game"', swift)
        self.assertIn('normalPlay ? "PublicOwnedContentNormalPlay" : "PublicOwnedContent"', swift)
        self.assertIn('report["normal_play_api_invoked"] = entry.normalPlayAPIInvoked', swift)
        self.assertIn("XCTAssertNotEqual(repeated, 0)", swift)
        self.assertEqual(len(re.findall(r"func testPublicOwnedHostV2\w+\(\)", swift)), 2)

    def stage_inputs(self, root):
        base, framework, resources = root / "base", root / "fixture", root / "resources"
        for path in (base / "app", base / "native", base / "include", base / "Libraries",
                     base / "Resources", base / "Frameworks/WgpuNative.framework", base / "sdk-test/native",
                     framework, resources):
            path.mkdir(parents=True, exist_ok=True)
        (base / "project.yml").write_text((HERE.parent / "project.yml").read_text().replace(
            "        SKYRIMRendererBackend: native-hal\n", "        SKYRIMRendererBackend: webgpu\n"))
        (base / "app/OwnedHost-Bridging-Header.h").write_text('#include "OwnedHost.h"\n')
        (base / "include/PrivateGuestABIV2.h").write_text("// public authored stand-in\n")
        (base / "Libraries/libios_owned_game_host.a").write_bytes(b"authored library placeholder")
        (base / "Frameworks/WgpuNative.framework/WgpuNative").write_bytes(b"authored public placeholder")
        (framework / "PrivateGuest").write_bytes(b"authored V2 placeholder, not executable")
        data = b"self authored fixture bytes" * 2
        binding = dict(abi_version=0x10000, xex_sha256=hashlib.sha256(data).hexdigest(), unsigned_framework_sha256="a" * 64)
        (base / "Resources/OwnedGuestBinding.json").write_text(json.dumps(binding))
        binding["abi_version"] = 0x20000
        (resources / "OwnedGuestBinding.json").write_text(json.dumps(binding))
        (resources / "default.xex").write_bytes(data)
        return base, framework, resources

    def test_fresh_stage_has_separate_bundle_one_class_and_exact_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve(strict=True)
            base, fixture, resources = self.stage_inputs(root)
            before = {p.relative_to(base): p.read_bytes() for p in base.rglob("*") if p.is_file()}
            output = s.prepare(base, fixture, resources, root / "new-v2")
            binding, xex, _ = d.validate_stage(output)
            self.assertEqual(binding, (resources / "OwnedGuestBinding.json").read_bytes())
            self.assertEqual(xex, (resources / "default.xex").read_bytes())
            self.assertEqual(list((output / "sdk-test/tests").iterdir()),
                             [output / "sdk-test/tests/OwnedHostV2FullChainTests.swift"])
            self.assertIn("PublicV2SDKProbe.h", (output / "app/OwnedHost-Bridging-Header.h").read_text())
            self.assertEqual(before, {p.relative_to(base): p.read_bytes() for p in base.rglob("*") if p.is_file()})
            with self.assertRaises(ValueError): s.prepare(base, fixture, resources, output)

    def test_native_stage_preserves_renderer_and_omits_legacy_framework(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve(strict=True)
            base, fixture, resources = self.stage_inputs(root)
            project = base / "project.yml"
            project.write_text(project.read_text().replace("SKYRIMRendererBackend: webgpu",
                                                           "SKYRIMRendererBackend: native-hal"))
            with self.assertRaises(ValueError):
                s.prepare(base, fixture, resources, root / "rejected")
            self.assertFalse((root / "rejected").exists())
            binary = base / "Frameworks/WgpuNative.framework/WgpuNative"
            binary.unlink()
            binary.parent.rmdir()
            output = s.prepare(base, fixture, resources, root / "native-v2")
            self.assertEqual(d.sdk.renderer_kind(output), "native-hal")
            self.assertIsNone(d.sdk.staged_gpu_sha(output))
            self.assertTrue((output / "Frameworks/PrivateGuest.framework/PrivateGuest").is_file())

    def test_stage_refuses_wrong_abi_identity_overlap_and_symlink_before_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve(strict=True)
            base, fixture, resources = self.stage_inputs(root)
            binding_path = resources / "OwnedGuestBinding.json"
            original = binding_path.read_bytes()
            for change in ({"abi_version": True}, {"abi_version": 0x10000}, {"xex_sha256": "b" * 64}):
                value = json.loads(original); value.update(change); binding_path.write_text(json.dumps(value))
                with self.assertRaises(ValueError): s.prepare(base, fixture, resources, root / "refused")
                self.assertFalse((root / "refused").exists())
            binding_path.write_bytes(original)
            old_binding = base / "Resources/OwnedGuestBinding.json"
            old_original = old_binding.read_bytes()
            packet = (resources / "default.xex").read_bytes()
            short = b"authored-too-short"
            for path, data in ((binding_path, original), (old_binding, old_original)):
                value = json.loads(data)
                value["xex_sha256"] = hashlib.sha256(short).hexdigest()
                path.write_text(json.dumps(value))
            (resources / "default.xex").write_bytes(short)
            with self.assertRaises(ValueError): s.prepare(base, fixture, resources, root / "refused")
            self.assertFalse((root / "refused").exists())
            binding_path.write_bytes(original); old_binding.write_bytes(old_original)
            (resources / "default.xex").write_bytes(packet)
            with self.assertRaises(ValueError): s.prepare(base, fixture, resources, base / "nested")
            (base / "native/link").symlink_to(base / "app/OwnedHost-Bridging-Header.h")
            with self.assertRaises(ValueError): s.prepare(base, fixture, resources, root / "refused")
            self.assertFalse((root / "refused").exists())

    def test_crlf_source_stage_and_validation_preserve_v2_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve(strict=True)
            base, fixture, resources = self.stage_inputs(root)
            source = base / "project.yml"
            source.write_bytes(source.read_text().replace("\n", "\r\n").encode())
            output = s.prepare(base, fixture, resources, root / "new-v2")
            project = output / "project.yml"
            self.assertNotIn(b"\r", project.read_bytes())
            expected = d.validate_stage(output)
            project.write_bytes(project.read_bytes().replace(b"\n", b"\r\n"))
            self.assertEqual(d.validate_stage(output), expected)

    def test_project_anchors_refuse_drift_or_already_selected_stage(self):
        original = (HERE.parent / "project.yml").read_text()
        changed = s.project_v2(original)
        self.assertEqual(changed.count("PUBLIC_V2_SDK_FIXTURE"), 1)
        self.assertEqual(changed.count("-Wl,-u,_OHReadPublicV2ProofV1"), 1)
        for bad in (changed, original + original, original.replace('SWIFT_VERSION: "5.0"', 'SWIFT_VERSION: "6.0"')):
            with self.assertRaises(ValueError): s.project_v2(bad)

    def evidence(self, version):
        ir = "declare i32 @setjmp(ptr) #0\n call i32 @setjmp(ptr %p) #0\nattributes #0 = { returns_twice }\n"
        ir += "call i1 @skyrt_xma_mmio_read_u32(i32 0, ptr %p)\ncall i1 @skyrt_xma_mmio_write_u32(i32 0, i32 1)\n"
        symbols = ""; assembly = ""
        for name in ("_setjmp", "_longjmp", "__Unwind_Resume"):
            symbols += "0000000000001000 T " + name + "\n"
            assembly += name + ":\n0000000000001000\tadrp\tx16, 0\n0000000000001004\tbr\tx16\n"
            assembly += "0000000000002000\tbl\t" + name + "\n"
        return [version, "\n".join(b.exports(version)) + "\n", "", ir, symbols, assembly]

    def test_builder_requires_exact_selected_exports_and_real_code_properties(self):
        for version in (1, 2): b.validate_code_evidence(*self.evidence(version))
        good = self.evidence(2)
        for index, value in [(1, "\n".join(b.exports(1))), (1, good[1] + "_extra\n"), (2, "_external"),
                             (3, good[3].replace("returns_twice", "nounwind")),
                             (3, good[3].replace("@skyrt_xma_mmio_write_u32", "@other")),
                             (4, good[4].replace("_longjmp", "_other")),
                             (5, good[5].replace("\tbr\tx16", "\tblr\tx16")),
                             (5, good[5].replace("\tadrp\tx16", "\tsub\tsp"))]:
            bad = good.copy(); bad[index] = value
            with self.subTest(index=index), self.assertRaises(ValueError): b.validate_code_evidence(*bad)
        for value in (True, 0, 3, 0x20000, "2"):
            with self.assertRaises(ValueError): b.exports(value)

    def test_source_plan_selects_real_v2_layout_adapter_without_v1_alias(self):
        plan = b.source_plan(Path("public"), 2)
        self.assertEqual([p.name for p, _ in plan], ["owned_abi_v2.cpp", "private_layout_v2.cpp", "PublicOwnedFixtureV2.cpp"])
        self.assertEqual([p.name for p, _ in b.source_plan(Path("public"), 1)], ["owned_abi.cpp", "PublicOwnedFixture.cpp"])
        with self.assertRaises(ValueError): b.source_plan(Path("public"), 0)

    def test_mmio_declarations_or_comments_cannot_replace_actual_instructions(self):
        good = self.evidence(2)
        names = ("skyrt_xma_mmio_read_u32", "skyrt_xma_mmio_write_u32")
        for name in names:
            for replacement in ("declare i1 @", "; call i1 @"):
                bad = good.copy()
                bad[3] = good[3].replace("call i1 @" + name, replacement + name)
                with self.subTest(name=name, replacement=replacement), self.assertRaises(ValueError):
                    b.validate_code_evidence(*bad)
        invoked = good.copy()
        invoked[3] = invoked[3].replace("call i1 @skyrt_xma_mmio_read_u32", "%value = invoke zeroext i1 @skyrt_xma_mmio_read_u32")
        invoked[3] = invoked[3].replace("call i1 @skyrt_xma_mmio_write_u32", "tail call zeroext i1 @skyrt_xma_mmio_write_u32")
        b.validate_code_evidence(*invoked)

    def test_public_header_overlay_imports_only_declared_frozen_dependencies(self):
        lab = HERE.parents[2]
        paths = ["skyrim-recomp-web/cpp/ppc_context.h",
                 "tools/ios-resident-guest-probe/apple-backend/prepare_fixture.py",
                 "tools/windows-ios-guest-probe/owned-guest-abi/mmio_overlay.py",
                 "tools/windows-ios-guest-probe/audit_private_guest_symbols.py"]
        with tempfile.TemporaryDirectory() as temporary:
            kit = Path(temporary).resolve(strict=True)
            for name in paths:
                destination = kit / name; destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(lab / name, destination)
            code = ("import importlib.util,sys; from pathlib import Path; "
                    "s=importlib.util.spec_from_file_location('builder',sys.argv[1]); "
                    "m=importlib.util.module_from_spec(s);s.loader.exec_module(m); "
                    "original,header=m.prepare_header(Path(sys.argv[2]),2); "
                    "assert 'skyrt_xma_mmio_write_u32' in header and 'returns_twice' in header; "
                    "assert original==(Path(sys.argv[2])/'skyrim-recomp-web/cpp/ppc_context.h').read_bytes()")
            command = [sys.executable, "-B", "-c", code, str(HERE / "build_public_sdk_fixture.py"), str(kit)]
            good = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(good.returncode, 0, good.stderr.decode())
            (kit / paths[-1]).unlink()
            refused = subprocess.run(command, capture_output=True, timeout=10)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn(b"audit_private_guest_symbols", refused.stderr)

    def test_failed_build_never_launches_or_publishes_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve(strict=True)
            base, fixture, resources = self.stage_inputs(root)
            stage = s.prepare(base, fixture, resources, root / "v2")
            calls = []
            def refuse_build(command, cwd, prefix, deadline, message, **kwargs):
                calls.append(command)
                if "build-for-testing" in command: raise RuntimeError("authored build refusal")
                return 0
            with mock.patch.object(d.sys, "platform", "darwin"), mock.patch.object(d.mobile, "run_command", side_effect=refuse_build):
                with self.assertRaisesRegex(RuntimeError, "authored build refusal"):
                    d.main(["--stage", str(stage), "--output", str(root / "evidence"), "--destination",
                            "platform=iOS Simulator,id=12345678-1234-1234-1234-123456789abc"])
            self.assertEqual(len(calls), 2)
            self.assertFalse(any("test-without-building" in c for c in calls))
            self.assertFalse((root / "evidence/public-v2-fullchain-result.json").exists())

    def successful_command_run(self, normal_play, refusal=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve(strict=True)
        base, fixture, resources = self.stage_inputs(root)
        stage = s.prepare(base, fixture, resources, root / "v2")
        output = root / "evidence"
        hashes = d.validate_stage(stage)[2]
        _, report = self.report(normal_play)
        report.update(hashes)
        summary = dict(totalTestCount=1, passedTests=1, failedTests=0, skippedTests=0, expectedFailures=0)
        if refusal == "skipped": summary.update(passedTests=0, skippedTests=1)
        if refusal == "wrong_api": report["normal_play_api_invoked"] = False
        text = self.console(report, normal_play)
        if refusal == "diagnostic_fallback":
            _, diagnostic = self.report(); diagnostic.update(hashes)
            text = self.console(diagnostic)
        calls = []

        def command(command, cwd, prefix, deadline, message, **kwargs):
            calls.append((command, deadline, kwargs))
            if "build-for-testing" in command:
                app = output / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app"
                (app / "PublicFixture").mkdir(parents=True)
                shutil.copytree(stage / "Frameworks", app / "Frameworks")
                shutil.copy2(stage / "Resources/OwnedGuestBinding.json", app / "OwnedGuestBinding.json")
                shutil.copy2(stage / "Resources/PublicFixture/default.xex", app / "PublicFixture/default.xex")
                (app / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": d.BUNDLE}))
            if "otool" in command:
                prefix.with_suffix(".stdout.log").write_text(
                    "host:\n\t@rpath/WgpuNative.framework/WgpuNative (compatibility version 1.0.0)\n")
            if "test-without-building" in command:
                kwargs["result"].mkdir()
                prefix.with_suffix(".stdout.log").write_text(text)
            if "xcresulttool" in command and "help" not in command:
                value = summary if command[4] == "summary" else {"testNodes": [
                    {"nodeIdentifier": d.NORMAL_TEST if normal_play else d.TEST, "result": "Passed"}]}
                prefix.with_suffix(".stdout.log").write_text(json.dumps(value))
            return 0

        argv = ["--stage", str(stage), "--output", str(output), "--destination",
                "platform=iOS Simulator,id=12345678-1234-1234-1234-123456789abc"]
        if normal_play: argv.append("--normal-play")
        console = io.StringIO()
        with mock.patch.object(d.sys, "platform", "darwin"), \
             mock.patch.object(d.mobile, "run_command", side_effect=command), \
             mock.patch.object(d.time, "monotonic", return_value=1000), contextlib.redirect_stdout(console):
            if refusal:
                with self.assertRaises(ValueError): d.main(argv)
            else:
                self.assertEqual(d.main(argv), 0)
                count = len(calls)
                with self.assertRaises(FileExistsError): d.main(argv)
                self.assertEqual(len(calls), count)  # Never reuse old evidence or launch again.
        return output, calls, report, console.getvalue()

    def test_cli_modes_have_separate_results_one_selector_and_same_deadline(self):
        for normal in (False, True):
            with self.subTest(normal_play=normal):
                output, calls, report, console = self.successful_command_run(normal)
                name = "public-v2-normal-play-result.json" if normal else "public-v2-fullchain-result.json"
                other = "public-v2-fullchain-result.json" if normal else "public-v2-normal-play-result.json"
                self.assertEqual(json.loads((output / name).read_bytes()), dict(
                    report, xcodebuild_exit_code=0, external_build_test_deadline_seconds=300))
                self.assertFalse((output / other).exists())
                tests = [(command, kwargs) for command, _, kwargs in calls if "test-without-building" in command]
                self.assertEqual(len(tests), 1)
                command, kwargs = tests[0]
                self.assertEqual([part for part in command if part.startswith("-only-testing:")],
                                 ["-only-testing:OwnedGameHostTests/OwnedHostV2FullChainTests/" +
                                  (d.NORMAL_TEST if normal else d.TEST)])
                self.assertEqual(kwargs["result"].name, "OwnedGameHostV2NormalPlay.xcresult" if normal else "OwnedGameHostV2.xcresult")
                self.assertEqual({deadline for _, deadline, _ in calls}, {1300})
                exports = [kwargs for command, _, kwargs in calls if "xcresulttool" in command]
                self.assertEqual(len(exports), 4)
                self.assertTrue(all(kwargs["cap"] == 15 for kwargs in exports))
                key = "public_owned_host_v2_normal_play_passed" if normal else "public_owned_host_v2_fullchain_passed"
                self.assertEqual(json.loads(console), {key: 1, "game_executed": False,
                    "physical_iphone_verified": False, "audio_output_verified": False,
                    "private_guest_framework_verified": False})

    def test_normal_successful_command_still_refuses_skip_wrong_api_or_diagnostic_fallback(self):
        for refusal in ("skipped", "wrong_api", "diagnostic_fallback"):
            with self.subTest(refusal=refusal):
                output, calls, _, console = self.successful_command_run(True, refusal)
                self.assertFalse((output / "public-v2-normal-play-result.json").exists())
                self.assertFalse((output / "public-v2-fullchain-result.json").exists())
                self.assertNotIn("public_owned_host_v2_normal_play_passed", console)
                self.assertEqual(len([c for c, _, _ in calls if "test-without-building" in c]), 1)

    def failed_test_run(self, bundle="directory", broken_summary=False, expired=False, normal_play=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve(strict=True)
        base, fixture, resources = self.stage_inputs(root)
        stage = s.prepare(base, fixture, resources, root / "v2")
        output = root / "evidence"
        original = RuntimeError("authored xcodebuild failure remains authoritative")
        calls = []
        hashes, result = self.report(normal_play)
        failed_summary = dict(totalTestCount=1, passedTests=0, failedTests=1,
                              skippedTests=0, expectedFailures=0)
        exported_tests = {"testNodes": [{"nodeIdentifier": d.NORMAL_CASE if normal_play else d.CASE, "result": "Failed"}]}

        def command(command, cwd, prefix, deadline, message, **kwargs):
            calls.append((command, deadline, kwargs))
            if "build-for-testing" in command:
                app = output / "DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app"
                (app / "PublicFixture").mkdir(parents=True)
                shutil.copytree(stage / "Frameworks", app / "Frameworks")
                shutil.copy2(stage / "Resources/OwnedGuestBinding.json", app / "OwnedGuestBinding.json")
                shutil.copy2(stage / "Resources/PublicFixture/default.xex", app / "PublicFixture/default.xex")
                (app / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": d.BUNDLE}))
            if "otool" in command:
                prefix.with_suffix(".stdout.log").write_text(
                    "host:\n\t@rpath/WgpuNative.framework/WgpuNative (compatibility version 1.0.0)\n")
            if "test-without-building" in command:
                path = kwargs["result"]
                if bundle == "directory": path.mkdir()
                if bundle == "file": path.write_bytes(b"authored non-bundle marker")
                # Even a complete success-looking console must not override a
                # nonzero test command or synthesize an aggregate result.
                prefix.with_suffix(".stdout.log").write_text(self.console(result, normal_play))
                raise original
            if "xcresulttool" in command and "help" not in command:
                if command[4] == "summary" and broken_summary:
                    raise RuntimeError("authored diagnostic export failure")
                value = failed_summary if command[4] == "summary" else exported_tests
                prefix.with_suffix(".stdout.log").write_text(json.dumps(value))
            return 0

        console = io.StringIO()
        clock = mock.patch.object(d.time, "monotonic", side_effect=[0, 301]) if expired else contextlib.nullcontext()
        with mock.patch.object(d.sys, "platform", "darwin"), \
             mock.patch.object(d.mobile, "run_command", side_effect=command), \
             mock.patch.object(d, "validate_summary") as summary_gate, \
             mock.patch.object(d, "validate_result") as result_gate, clock, contextlib.redirect_stdout(console):
            with self.assertRaises(RuntimeError) as raised:
                d.main(["--stage", str(stage), "--output", str(output), "--destination",
                        "platform=iOS Simulator,id=12345678-1234-1234-1234-123456789abc"] +
                       (["--normal-play"] if normal_play else []))
            self.assertIs(raised.exception, original)
            summary_gate.assert_not_called(); result_gate.assert_not_called()
        self.assertFalse((output / "public-v2-fullchain-result.json").exists())
        self.assertFalse((output / "public-v2-normal-play-result.json").exists())
        self.assertNotIn("public_owned_host_v2_fullchain_passed", console.getvalue())
        self.assertNotIn("public_owned_host_v2_normal_play_passed", console.getvalue())
        self.assertEqual(len({deadline for _, deadline, _ in calls}), 1)
        return output, calls, failed_summary, exported_tests

    def test_failed_test_preserves_original_error_and_exports_actual_failed_bundle_data(self):
        output, calls, summary, tests = self.failed_test_run()
        self.assertEqual(json.loads((output / "xcresult-summary.json").read_bytes()), summary)
        self.assertEqual(json.loads((output / "xcresult-tests.json").read_bytes()), tests)
        exports = [(command, kwargs) for command, _, kwargs in calls if "xcresulttool" in command]
        self.assertEqual([command[2:] for command, _ in exports], [
            ["help", "get", "test-results", "summary"],
            ["get", "test-results", "summary", "--path", str(output / "OwnedGameHostV2.xcresult")],
            ["help", "get", "test-results", "tests"],
            ["get", "test-results", "tests", "--path", str(output / "OwnedGameHostV2.xcresult")]])
        self.assertTrue(all(kwargs["cap"] == 15 for _, kwargs in exports))

    def test_failed_diagnostic_export_still_attempts_tests_and_preserves_original_error(self):
        output, calls, _, tests = self.failed_test_run(broken_summary=True)
        self.assertFalse((output / "xcresult-summary.json").exists())
        self.assertEqual(json.loads((output / "xcresult-tests.json").read_bytes()), tests)
        self.assertTrue(any(command[2:5] == ["get", "test-results", "tests"] for command, _, _ in calls))

    def test_normal_failed_test_cannot_publish_success_and_keeps_actual_failure_diagnostics(self):
        output, calls, summary, tests = self.failed_test_run(normal_play=True)
        self.assertEqual(json.loads((output / "xcresult-summary.json").read_bytes()), summary)
        self.assertEqual(json.loads((output / "xcresult-tests.json").read_bytes()), tests)
        selected = [command for command, _, _ in calls if "test-without-building" in command]
        self.assertEqual(len(selected), 1)
        self.assertIn("-only-testing:OwnedGameHostTests/OwnedHostV2FullChainTests/" + d.NORMAL_TEST, selected[0])
        self.assertEqual(selected[0][selected[0].index("-resultBundlePath") + 1],
                         str(output / "OwnedGameHostV2NormalPlay.xcresult"))
        exports = [(command, kwargs) for command, _, kwargs in calls if "xcresulttool" in command and "help" not in command]
        self.assertEqual([command[-1] for command, _ in exports], [str(output / "OwnedGameHostV2NormalPlay.xcresult")] * 2)
        self.assertTrue(all(kwargs["cap"] == 15 for _, kwargs in exports))

    def test_missing_nonbundle_or_expired_result_never_launches_diagnostic_commands(self):
        for bundle, expired in (("missing", False), ("file", False), ("directory", True)):
            with self.subTest(bundle=bundle, expired=expired):
                output, calls, _, _ = self.failed_test_run(bundle=bundle, expired=expired)
                self.assertFalse(any("xcresulttool" in command for command, _, _ in calls))
                self.assertFalse((output / "xcresult-summary.json").exists())
                self.assertFalse((output / "xcresult-tests.json").exists())

if __name__ == "__main__":
    unittest.main()
