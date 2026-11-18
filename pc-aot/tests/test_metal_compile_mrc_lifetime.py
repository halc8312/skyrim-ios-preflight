"""Portable runner/report controls only; these do not execute Foundation or MRC."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("mrc_lifetime_tests", ROOT / "experiments/metal-compile/mrc_lifetime.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def synthetic_native():
    rows = []
    for name in probe.CASES:
        failure = name.startswith(("nil_", "throw_"))
        late = name == "late_after_destroy_request"
        after = name.endswith("after_success")
        rows.append({"case": name, "notifications": 1, "retirements": 1,
                     "callback_attempts": 2 if after else 1, "faults": int(failure),
                     "snapshot_survived_caller_pool": True, "first_payload_preserved": True,
                     "error_code": 7 if late or name == "early_warning" else (12 if name.startswith("throw_") else 11) if failure and not after else 0,
                     "notification_before_launch_exit": not late, "retirement_after_launch_exit": True,
                     "retirement_inside_notification": False, "timeout_did_not_cancel": late,
                     "objc_exception_observed": name.startswith("throw_"), "tracked_objects_balanced": True,
                     "objects_created": {"owner": 1, "task": 0 if failure else 1, "token": 2}})
    return {"schema": 1, "scope": probe.SCOPE, "status": "passed", "manual_reference_counting": True,
            "foundation_ownership_verified": True, **{key: False for key in probe.FALSE_EVIDENCE}, "cases": rows}


class MrcReportTests(unittest.TestCase):
    def test_authored_cases_require_exact_notification_and_retirement(self):
        probe.validate_native(synthetic_native())
        for field, bad in (("notifications", 2), ("retirements", 0), ("callback_attempts", True),
                           ("retirement_after_launch_exit", False), ("retirement_inside_notification", True),
                           ("tracked_objects_balanced", False), ("snapshot_survived_caller_pool", False)):
            native = synthetic_native()
            native["cases"][0][field] = bad
            with self.subTest(field=field), self.assertRaises(ValueError):
                probe.validate_native(native)

    def test_nil_or_exception_after_success_cannot_erase_result_or_fault(self):
        for index in (5, 6):
            for field, bad in (("faults", 0), ("error_code", 12), ("first_payload_preserved", False)):
                native = synthetic_native()
                native["cases"][index][field] = bad
                with self.subTest(index=index, field=field), self.assertRaises(ValueError):
                    probe.validate_native(native)

    def test_late_timeout_and_warning_require_observations(self):
        for index, field, bad in ((1, "timeout_did_not_cancel", False), (1, "notification_before_launch_exit", True),
                                   (2, "error_code", 0), (4, "objc_exception_observed", False)):
            native = synthetic_native()
            native["cases"][index][field] = bad
            with self.subTest(field=field), self.assertRaises(ValueError):
                probe.validate_native(native)

    def test_scope_arc_missing_cases_and_metal_claims_are_rejected(self):
        for key, bad in [("schema", True), ("manual_reference_counting", False), ("cases", []),
                         *[(key, True) for key in probe.FALSE_EVIDENCE]]:
            native = synthetic_native()
            native[key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(native)


class MrcRunnerTests(unittest.TestCase):
    def fake_commands(self, sdk, calls, *, failure_label=None, invalid_json=False, runtime_stderr=""):
        def execute(argv, label, output, report, env, timeout, *, deadline):
            argv = [str(value) for value in argv]
            calls.append((argv, label, dict(env), timeout, deadline))
            event = {"label": label, "returncode": 0, "timed_out": False, "interrupted": False}
            report["commands"].append(event)
            if label == failure_label:
                event.update(returncode=-9, timed_out=True)
                return event, "", "synthetic timeout"
            text = ""
            if label == "sdk-version":
                text = "26.5\n"
            elif label == "sdk-path":
                text = str(sdk)
            elif label.endswith("-compile"):
                Path(argv[argv.index("-o") + 1]).write_bytes(b"synthetic executable; not an Apple binary")
            elif label.endswith("-run"):
                Path(argv[argv.index("--report") + 1]).write_text(
                    '{"schema":1,"schema":1}' if invalid_json else json.dumps(synthetic_native()), encoding="utf-8")
            return event, text, runtime_stderr if label.endswith("-run") else ""
        return execute

    def test_unsupported_host_preserves_inputs_and_refuses_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "fresh"
            args = probe.parse_args(["--output", str(output)])
            with patch.object(probe.platform, "system", return_value="Windows"), patch.object(probe.control, "run_command") as command:
                report = probe.run(args)
            command.assert_not_called()
            self.assertEqual(report["status"], "unsupported_host")
            self.assertFalse(report["foundation_ownership_verified"])
            self.assertEqual((output / "owner_retirement.h").read_bytes(), (probe.HERE / "owner_retirement.h").read_bytes())
            self.assertEqual(json.loads((output / "result.json").read_text()), report)
            with self.assertRaises(FileExistsError):
                probe.run(args)

    def test_mocked_three_variant_orchestration_keeps_mrc_and_sanitizers_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "SDK"
            sdk.mkdir()
            calls = []
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, calls)), \
                    patch.dict(probe.os.environ, {"CXXFLAGS": "-fobjc-arc"}):
                report = probe.run(probe.parse_args(["--output", str(Path(directory) / "fresh")]))
            self.assertEqual(report["status"], "passed")
            self.assertEqual([row["variant"] for row in report["variants"]], ["normal", "asan-ubsan", "tsan"])
            for argv, label, env, timeout, deadline in calls:
                self.assertNotIn("CXXFLAGS", env)
                self.assertGreater(deadline, 0)
                self.assertEqual(timeout, 60)
                if label.endswith("-compile"):
                    self.assertIn("clang++", argv)
                    self.assertIn("-fno-objc-arc", argv)
                    self.assertNotIn("-fobjc-arc", argv)
                    self.assertNotIn("Metal", argv)
                    flags = [value for value in argv if value.startswith("-fsanitize=")]
                    self.assertEqual(flags, {"normal-compile": [], "asan-ubsan-compile": ["-fsanitize=address,undefined"],
                                             "tsan-compile": ["-fsanitize=thread"]}[label])
            for key in probe.FALSE_EVIDENCE:
                self.assertIs(report[key], False)

    def test_timeout_remains_process_failure_without_native_json(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            calls = []
            output = sdk / "fresh"
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, calls, failure_label="normal-run")):
                report = probe.run(probe.parse_args(["--output", str(output)]))
            self.assertEqual(report["status"], "failed")
            self.assertTrue(report["commands"][-1]["timed_out"])
            self.assertNotIn("FileNotFoundError", report["reason"])
            self.assertEqual(report["variants"][0]["status"], "failed")
            self.assertFalse(report["foundation_ownership_verified"])

    def test_duplicate_native_json_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, [], invalid_json=True)):
                report = probe.run(probe.parse_args(["--output", str(sdk / "fresh"), "--variants", "normal"]))
            self.assertEqual(report["status"], "failed")
            self.assertIn("Duplicate", report["reason"])

    def test_runtime_diagnostics_fail_even_with_successful_json(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            output = sdk / "fresh"
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(
                        sdk, [], runtime_stderr="synthetic sanitizer diagnostic")):
                report = probe.run(probe.parse_args(["--output", str(output), "--variants", "normal"]))
            self.assertEqual(report["status"], "failed")
            self.assertIn("runtime emitted diagnostics", report["reason"])
            self.assertTrue((output / "normal-native.json").is_file())
            self.assertFalse(report["foundation_ownership_verified"])

    def test_absolute_budget_stops_tool_admission(self):
        now = [0.0]
        calls = []
        def command(argv, label, output, report, env, timeout, *, deadline):
            calls.append(deadline)
            now[0] = 181.0
            return {"returncode": 0, "timed_out": False}, "26.5", ""
        with tempfile.TemporaryDirectory() as directory, patch.object(probe.platform, "system", return_value="Darwin"), \
                patch.object(probe.time, "monotonic", side_effect=lambda: now[0]), \
                patch.object(probe.control, "run_command", side_effect=command):
            report = probe.run(probe.parse_args(["--output", str(Path(directory) / "fresh")]))
        self.assertEqual(calls, [180.0])
        self.assertEqual(report["status"], "failed")
        self.assertIn("budget exhausted", report["reason"])

    def test_keyboard_and_systemexit_keep_terminal_report(self):
        for exception in (KeyboardInterrupt, SystemExit):
            with self.subTest(exception=exception), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "fresh"
                with patch.object(probe.platform, "system", return_value="Darwin"), \
                        patch.object(probe.control, "run_command", side_effect=exception), self.assertRaises(exception):
                    probe.run(probe.parse_args(["--output", str(output)]))
                report = json.loads((output / "result.json").read_text())
                self.assertEqual(report["status"], "interrupted")
                self.assertFalse(report["foundation_ownership_verified"])
                self.assertFalse((output / "result.tmp").exists())

    def test_invalid_budgets_and_duplicate_variants_rejected(self):
        for argv in (["--budget", "0"], ["--budget", "nan"], ["--budget", "721"], ["--timeout", "inf"],
                     ["--variants", "normal", "normal"]):
            with self.subTest(argv=argv), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", *argv])


if __name__ == "__main__":
    unittest.main()
