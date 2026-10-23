"""Authored parser/staging/refusal controls. No Apple tools or game inputs."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
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
    def report(self):
        hashes, result = old_tests.PublicDriverTests().report()
        result.update(kind="public_owned_host_v2_fullchain_xctest", v2_proof=copy.deepcopy(d.PROOF),
                      audio_output_verified=False, xma_pcm_verified=False)
        return hashes, result

    def console(self, result):
        return (f"Test Case '{d.CASE}' started.\n" + d.PREFIX + json.dumps(result) + "\n"
                f"Test Case '{d.CASE}' passed (0.1 seconds).\n"
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

    def stage_inputs(self, root):
        base, framework, resources = root / "base", root / "fixture", root / "resources"
        for path in (base / "app", base / "native", base / "include", base / "Libraries",
                     base / "Resources", base / "Frameworks/WgpuNative.framework", base / "sdk-test/native",
                     framework, resources):
            path.mkdir(parents=True, exist_ok=True)
        (base / "project.yml").write_text((HERE.parent / "project.yml").read_text())
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

if __name__ == "__main__":
    unittest.main()
