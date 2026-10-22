"""Pure public proof-parser regressions; no SDK or game input is opened."""
import copy
import importlib.util
import json
import hashlib
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("owned_sdk_driver", Path(__file__).with_name("run_sdk_tests.py"))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class PublicDriverTests(unittest.TestCase):
    def report(self):
        hashes = dict(unsigned_fixture_sha256="a" * 64, public_xex_sha256="b" * 64,
                      trusted_binding_sha256="c" * 64)
        report = dict(kind="public_owned_host_fullchain_xctest", passed=True, state=5,
                      policy=0, exit_code=0, function_imports=2, registered_functions=2,
                      function_count=3, renderer_initialized=1, gpu_errors=0, decoded_frames=0,
                      separate_fresh_process=True, public_fixture_only=True,
                      game_executed=False, physical_iphone_verified=False, ready_to_play=False,
                      signature_authority_authenticated=False, private_guest_framework_verified=False,
                      game_frame_rendered=False, mapped_inode_attested=False, **hashes)
        return hashes, report

    def console(self, report):
        return (f"Test Case '{driver.CASE}' started.\n" + driver.PREFIX + json.dumps(report) + "\n"
                f"Test Case '{driver.CASE}' passed (0.1 seconds).\n"
                "Executed 1 test, with 0 failures (0 unexpected) in 0.1 seconds\n"
                "** TEST EXECUTE SUCCEEDED **\n")

    def test_one_exact_case_and_fixed_report(self):
        hashes, report = self.report()
        valid = self.console(report)
        self.assertEqual(driver.validate_result(valid, hashes), report)
        for bad in [valid + valid,
                    valid.replace("started.", "skipped."),
                    valid.replace("Executed 1 test", "Executed 2 tests"),
                    valid.replace("0 failures", "1 failure"),
                    valid.replace("** TEST EXECUTE SUCCEEDED **", "** TEST SUCCEEDED **"),
                    valid.replace("** TEST EXECUTE SUCCEEDED **", "** TEST EXECUTE FAILED **"),
                    valid + "** TEST EXECUTE FAILED **\n",
                    valid + f"Test Case 'other' skipped.\n"]:
            with self.assertRaises(ValueError):
                driver.validate_result(bad, hashes)

    def test_changed_outcomes_hashes_and_json_duplicates_refuse(self):
        hashes, report = self.report()
        for key, value in [("state", 4), ("gpu_errors", 1), ("function_imports", True),
                           ("decoded_frames", 1), ("game_executed", True),
                           ("unsigned_fixture_sha256", "d" * 64), ("unexpected", False)]:
            changed = copy.deepcopy(report)
            changed[key] = value
            with self.assertRaises(ValueError):
                driver.validate_result(self.console(changed), hashes)
        for raw in ['{"passed":true,"passed":false}', '{"a":NaN}', '{"a":Infinity}']:
            with self.assertRaises(ValueError):
                driver.strict_json(raw)

    def test_sdk_summary_requires_one_pass_and_no_skip_failure(self):
        summary = dict(totalTestCount=1, passedTests=1, failedTests=0,
                       skippedTests=0, expectedFailures=0)
        driver.validate_summary(summary)
        for key in summary:
            changed = summary.copy()
            changed[key] = True
            with self.assertRaises(ValueError):
                driver.validate_summary(changed)
            changed = summary.copy()
            del changed[key]
            with self.assertRaises(ValueError):
                driver.validate_summary(changed)
        for key in ["failedTests", "skippedTests", "expectedFailures"]:
            changed = summary.copy()
            changed[key] = 1
            with self.assertRaises(ValueError):
                driver.validate_summary(changed)

    def test_package_dependency_guard_refuses_debug_stub_and_private_eager_load(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "OwnedGameHost.app"
            app.mkdir()
            binding, xex = b"public binding", b"self-authored XEX"
            (app / "OwnedGuestBinding.json").write_bytes(binding)
            (app / "PublicFixture").mkdir()
            (app / "PublicFixture/default.xex").write_bytes(xex)
            hashes = []
            for name in ["PrivateGuest", "WgpuNative"]:
                binary = app / f"Frameworks/{name}.framework/{name}"
                binary.parent.mkdir(parents=True)
                data = ("self-authored " + name).encode()
                binary.write_bytes(data)
                hashes.append(hashlib.sha256(data).hexdigest())
            wgpu = "\t@rpath/WgpuNative.framework/WgpuNative (compatibility version 1.0.0)\n"
            system = "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
            driver.verify_package(app, binding, xex, *hashes, wgpu + system)
            debug_stub = "\t@rpath/OwnedGameHost.debug.dylib (compatibility version 0.0.0)\n"
            private_load = "\t@rpath/PrivateGuest.framework/PrivateGuest (compatibility version 1.0.0)\n"
            for dependencies in [debug_stub + system, wgpu + private_load + system, wgpu + wgpu + system]:
                with self.assertRaises(ValueError):
                    driver.verify_package(app, binding, xex, *hashes, dependencies)

    def test_single_run_command_has_no_iteration_or_retry_mode(self):
        command = driver.make_test_command(["xcodebuild"], ["CODE_SIGNING_ALLOWED=NO"], Path("public.xcresult"))
        self.assertEqual(command[-1], "test-without-building")
        self.assertIn("-only-testing:OwnedGameHostTests/OwnedHostFullChainTests/" + driver.TEST, command)
        self.assertEqual(command[command.index("-parallel-testing-enabled") + 1], "NO")
        self.assertFalse(any("iteration" in word or "retry" in word for word in command))


if __name__ == "__main__":
    unittest.main()
