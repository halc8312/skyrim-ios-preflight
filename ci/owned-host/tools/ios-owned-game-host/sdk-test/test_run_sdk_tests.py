"""Pure public proof-parser regressions; no SDK or game input is opened."""
import copy
import contextlib
import io
import importlib.util
import json
import hashlib
import tempfile
from pathlib import Path
import unittest
import unittest.mock

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
                      game_frame_rendered=False, mapped_inode_attested=False,
                      main_thread_attach_rejected=True, metal_layer_required_setters_on_main=True,
                      metal_layer_off_main_calls=0, **hashes)
        return hashes, report

    def trace_report(self):
        return dict(abi_version=1, struct_size=32, captured_count=7,
                    expected_parent_count=2, matched_parent_count=2, rx_parent_count=2,
                    flags=15, reserved=0, passed=True, public_cpp_only=True,
                    private_guest_fatal_path_verified=False)

    def console(self, report, trace=None, reverse=False):
        if trace is None:
            trace = self.trace_report()
        cases = [(driver.CASE, driver.PREFIX, report),
                 (driver.TRACE_CASE, driver.TRACE_PREFIX, trace)]
        if reverse:
            cases.reverse()
        text = ""
        for case, prefix, value in cases:
            text += (f"Test Case '{case}' started.\n" + prefix + json.dumps(value) + "\n"
                     f"Test Case '{case}' passed (0.1 seconds).\n"
                     "Executed 1 test, with 0 failures (0 unexpected) in 0.1 seconds\n")
        return (text + "Executed 2 tests, with 0 failures (0 unexpected) in 0.2 seconds\n"
                "** TEST EXECUTE SUCCEEDED **\n")

    def test_two_exact_serial_cases_and_fixed_reports_in_either_order(self):
        hashes, report = self.report()
        for reverse in (False, True):
            valid = self.console(report, reverse=reverse)
            self.assertEqual(driver.validate_result(valid, hashes), report)
            self.assertEqual(driver.validate_native_trace_result(valid), self.trace_report())

    def test_missing_duplicate_failed_skipped_extra_or_partial_cases_refuse(self):
        hashes, report = self.report()
        valid = self.console(report)
        for bad in [valid + valid,
                    valid.replace("started.", "skipped."),
                    valid.replace("passed (0.1", "failed (0.1", 1),
                    valid.replace(f"Test Case '{driver.TRACE_CASE}' started.\n", ""),
                    valid.replace(f"Test Case '{driver.TRACE_CASE}' passed (0.1 seconds).\n", ""),
                    valid.replace("Executed 2 tests", "Executed 1 test"),
                    valid.replace("Executed 2 tests", "Executed 3 tests"),
                    valid.replace("0 failures", "1 failure"),
                    valid.replace("** TEST EXECUTE SUCCEEDED **", "** TEST SUCCEEDED **"),
                    valid.replace("** TEST EXECUTE SUCCEEDED **", "** TEST EXECUTE FAILED **"),
                    valid + "** TEST EXECUTE FAILED **\n",
                    valid + f"Test Case 'other' skipped.\n"]:
            for validate in (lambda text: driver.validate_result(text, hashes),
                             driver.validate_native_trace_result):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    validate(bad)
        # Parallel/interleaved execution is not a serial two-case proof.
        bad = (f"Test Case '{driver.CASE}' started.\n"
               f"Test Case '{driver.TRACE_CASE}' started.\n"
               f"Test Case '{driver.CASE}' passed (0.1 seconds).\n"
               f"Test Case '{driver.TRACE_CASE}' passed (0.1 seconds).\n"
               "Executed 2 tests, with 0 failures (0 unexpected) in 0.2 seconds\n"
               "** TEST EXECUTE SUCCEEDED **\n")
        with self.assertRaises(ValueError):
            driver.validate_console(bad)

    def test_uikit_layer_thread_violation_rejects_otherwise_passing_run(self):
        hashes, report = self.report()
        for apostrophe in ("'", "’"):
            bad = self.console(report) + f"Modifying properties of a view{apostrophe}s layer off the main thread is not allowed\n"
            with self.assertRaises(ValueError):
                driver.validate_result(bad, hashes)

    def test_changed_outcomes_hashes_and_json_duplicates_refuse(self):
        hashes, report = self.report()
        for key, value in [("state", 4), ("gpu_errors", 1), ("function_imports", True),
                           ("decoded_frames", 1), ("game_executed", True),
                           ("main_thread_attach_rejected", False),
                           ("metal_layer_required_setters_on_main", False),
                           ("metal_layer_off_main_calls", 1),
                           ("metal_layer_off_main_calls", False),
                           ("unsigned_fixture_sha256", "d" * 64), ("unexpected", False)]:
            changed = copy.deepcopy(report)
            changed[key] = value
            with self.assertRaises(ValueError):
                driver.validate_result(self.console(changed), hashes)
        for raw in ['{"passed":true,"passed":false}', '{"a":NaN}', '{"a":Infinity}']:
            with self.assertRaises(ValueError):
                driver.strict_json(raw)

    def test_native_trace_report_requires_exact_typed_public_outcome(self):
        _, report = self.report()
        trace = self.trace_report()
        for count in (3, 32):
            changed = dict(trace, captured_count=count)
            self.assertEqual(driver.validate_native_trace_result(self.console(report, changed)), changed)
        for key, value in [("abi_version", True), ("struct_size", 31),
                           ("captured_count", True), ("captured_count", 2),
                           ("captured_count", 33), ("captured_count", 7.0),
                           ("expected_parent_count", 1), ("matched_parent_count", 1),
                           ("rx_parent_count", 1), ("flags", 7), ("reserved", 1),
                           ("passed", 1), ("public_cpp_only", False),
                           ("private_guest_fatal_path_verified", True), ("unknown", False)]:
            changed = dict(trace, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                driver.validate_native_trace_result(self.console(report, changed))
        for key in trace:
            changed = trace.copy()
            del changed[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                driver.validate_native_trace_result(self.console(report, changed))

    def test_missing_duplicate_malformed_or_nonobject_reports_refuse(self):
        hashes, report = self.report()
        valid = self.console(report)
        for prefix, validate in [(driver.PREFIX, lambda text: driver.validate_result(text, hashes)),
                                 (driver.TRACE_PREFIX, driver.validate_native_trace_result)]:
            line = next(line for line in valid.splitlines() if line.startswith(prefix)) + "\n"
            for bad in [valid.replace(line, ""), valid + line,
                        valid + prefix + "not JSON\n",
                        valid.replace(line, prefix + '{"passed":true,"passed":true}\n'),
                        valid.replace(line, prefix + "[]\n")]:
                with self.subTest(prefix=prefix, bad=bad), self.assertRaises(ValueError):
                    validate(bad)

    def test_sdk_summary_requires_two_passes_and_no_skip_failure(self):
        summary = dict(totalTestCount=2, passedTests=2, failedTests=0,
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
        for key in ["totalTestCount", "passedTests"]:
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

    def test_failure_tails_are_bounded_and_read_only_own_regular_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "public-build"
            logs = [prefix.with_suffix(".stdout.log"), prefix.with_suffix(".stderr.log")]
            logs[0].write_bytes(b"OMITTED_HEAD" + b"a" * (driver.LOG_TAIL_BYTES * 2) + b"OWN_STDOUT_END")
            logs[1].write_bytes(b"b" * (driver.LOG_TAIL_BYTES * 2) + b"OWN_STDERR_END")
            unrelated = root / "unrelated.txt"
            unrelated.write_bytes(b"UNRELATED_DO_NOT_READ")
            actual_open = driver.os.open
            opened = []
            def record_open(path, *args, **kwargs):
                opened.append(Path(path))
                return actual_open(path, *args, **kwargs)
            with unittest.mock.patch.object(driver.os, "open", side_effect=record_open):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    driver.print_command_log_tails(prefix)
            text = output.getvalue()
            self.assertEqual(opened, logs)
            self.assertNotIn("OMITTED_HEAD", text)
            self.assertNotIn("UNRELATED_DO_NOT_READ", text)
            self.assertIn("OWN_STDOUT_END", text)
            self.assertIn("OWN_STDERR_END", text)
            headers = [json.loads(line.split(" ", 1)[1]) for line in text.splitlines()
                       if line.startswith("PUBLIC_SDK_COMMAND_LOG_TAIL ")]
            self.assertEqual(len(headers), 2)
            self.assertTrue(all(row["available"] and row["truncated"]
                                and row["utf8_bytes"] <= driver.LOG_TAIL_BYTES for row in headers))
            self.assertLess(len(text.encode("utf-8")), 2 * (driver.LOG_TAIL_BYTES + 512))
            self.assertEqual(unrelated.read_bytes(), b"UNRELATED_DO_NOT_READ")

    def test_nonzero_or_missing_result_still_fails_and_success_prints_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "public-test"
            prefix.with_suffix(".stdout.log").write_bytes(b"PUBLIC_TEST_FAILURE\n")
            prefix.with_suffix(".stderr.log").write_bytes(b"\xff" * (driver.LOG_TAIL_BYTES * 2))
            existing = root / "public.xcresult"
            existing.mkdir()
            for code, result in [(7, None), (7, existing), (0, root / "missing.xcresult")]:
                output = io.StringIO()
                with contextlib.redirect_stdout(output), self.assertRaisesRegex(RuntimeError, "unchanged failure"):
                    driver.require_sdk_command_success(code, prefix, "unchanged failure", result)
                self.assertIn("PUBLIC_TEST_FAILURE", output.getvalue())
                self.assertLess(len(output.getvalue().encode("utf-8")), 2 * (driver.LOG_TAIL_BYTES + 512))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                driver.require_sdk_command_success(0, prefix, "unchanged failure", existing)
            self.assertEqual(output.getvalue(), "")
            # Missing logs cannot hide/replace the original SDK failure.
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "original failure"):
                driver.require_sdk_command_success(1, root / "missing-logs", "original failure")

    def test_single_run_command_has_no_iteration_or_retry_mode(self):
        command = driver.make_test_command(["xcodebuild"], ["CODE_SIGNING_ALLOWED=NO"], Path("public.xcresult"))
        self.assertEqual(command[-1], "test-without-building")
        self.assertIn("-only-testing:OwnedGameHostTests/OwnedHostFullChainTests/" + driver.TEST, command)
        self.assertEqual({word for word in command if word.startswith("-only-testing:")}, {
            "-only-testing:OwnedGameHostTests/OwnedHostFullChainTests/" + driver.TEST,
            "-only-testing:OwnedGameHostTests/NativeTraceSDKProbeTests/" + driver.TRACE_TEST,
        })
        self.assertEqual(sum(word.startswith("-only-testing:") for word in command), 2)
        self.assertEqual(command[command.index("-parallel-testing-enabled") + 1], "NO")
        self.assertFalse(any("iteration" in word or "retry" in word for word in command))


if __name__ == "__main__":
    unittest.main()
