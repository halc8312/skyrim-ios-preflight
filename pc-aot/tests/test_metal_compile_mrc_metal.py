"""Portable report/runner controls. Mocked tools are not Apple/MRC/GPU evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("mrc_metal_tests", ROOT / "experiments/metal-compile/mrc_metal.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def diagnostic():
    return {"present": True, "domain": "SyntheticCompiler", "code": 7, "description": "authored diagnostic",
            "full_description": "original synthetic diagnostic", "user_info_description": "{synthetic = true;}"}


def synthetic_native(unsupported=False):
    rows = []
    if not unsupported:
        for name in probe.CASES:
            controlled, invalid = name == "controlled_publication", name == "invalid_source"
            row = {"case": name, "status": "passed", "library_compile_attempts": 1, "callbacks": 1,
                   "notifications": 1, "retirements": 1, "caller_pool_expired_before_launch": True,
                   "retirement_after_launch_exit": True, "tracked_owner_retired": True,
                   "callback_pool_drained_before_observation": True, "original_callback_objects_preserved": True,
                   "notification_before_launch_exit": not controlled, "controlled_publication": controlled,
                   "destroy_requested_before_controlled_publication": controlled, "library_created": not invalid,
                   "library_error": diagnostic() if invalid else {"present": False},
                   "exact_compute_readback_verified": not invalid}
            if name == "warning_source":
                row["library_error"] = diagnostic()
                row["library_error"]["description"] += " " + probe.WARNING_MARKER
                row["warning_marker_present"] = True
            if not invalid:
                row.update(command_completed=True, compute_words=list(probe.EXPECTED_WORDS),
                           pipeline_error={"present": False}, command_error={"present": False})
            rows.append(row)
    result = {"schema": 1, "scope": probe.SCOPE, "manual_reference_counting": True,
              "status": "unsupported" if unsupported else "passed", "cases": rows,
              "legacy_async_library_verified": not unsupported, "exact_compute_readback_verified": not unsupported,
              **{key: False for key in probe.FALSE_EVIDENCE}}
    if unsupported:
        result["reason"] = "no_default_metal_device"
    else:
        result["device"] = {"name": "synthetic"}
    return result


class MrcMetalReportTests(unittest.TestCase):
    def test_success_requires_exact_gpu_output_and_owner_retirement(self):
        probe.validate_native(synthetic_native())
        for key, value in (("notifications", 2), ("retirements", True), ("tracked_owner_retired", False),
                           ("retirement_after_launch_exit", False), ("callback_pool_drained_before_observation", False),
                           ("compute_words", [0] * 16), ("original_callback_objects_preserved", False)):
            native = synthetic_native()
            native["cases"][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(native)

    def test_invalid_source_is_observed_rejection_not_gpu_success(self):
        for key, value in (("library_created", True), ("library_error", {"present": False}),
                           ("compute_words", [0] * 16), ("exact_compute_readback_verified", True)):
            native = synthetic_native()
            native["cases"][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(native)

    def test_library_warning_is_preserved_without_reclassifying_success(self):
        native = synthetic_native()
        native["cases"][0]["library_error"] = diagnostic()
        probe.validate_native(native)
        self.assertEqual(native["cases"][0]["library_error"]["full_description"], "original synthetic diagnostic")

    def test_explicit_warning_case_requires_real_error_and_its_marker(self):
        for missing in ("error", "marker", "library"):
            native = synthetic_native()
            row = native["cases"][3]
            if missing == "error":
                row["library_error"] = {"present": False}
            elif missing == "marker":
                row["library_error"]["description"] = "a different warning"
            else:
                row["library_created"] = False
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                probe.validate_native(native)

    def test_controlled_publication_cannot_claim_real_timeout_or_cancellation(self):
        for key, value in (("destroy_requested_before_controlled_publication", False),
                           ("notification_before_launch_exit", True), ("controlled_publication", False)):
            native = synthetic_native()
            native["cases"][2][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(native)
        for key in probe.FALSE_EVIDENCE:
            native = synthetic_native()
            native[key] = True
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(native)

    def test_unsupported_device_cannot_promote_evidence(self):
        probe.validate_native(synthetic_native(True), unsupported=True)
        for key, value in (("legacy_async_library_verified", True), ("cases", synthetic_native()["cases"]),
                           ("reason", "some_other_failure"), ("schema", True)):
            native = synthetic_native(True)
            native[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate_native(native, unsupported=True)

    def test_diagnostics_and_partial_cases_cannot_be_fabricated(self):
        native = synthetic_native()
        del native["cases"][1]["library_error"]["full_description"]
        with self.assertRaises(ValueError):
            probe.validate_native(native)
        native = synthetic_native()
        native["cases"] = native["cases"][:2]
        with self.assertRaises(ValueError):
            probe.validate_native(native)


class MrcMetalRunnerTests(unittest.TestCase):
    def fake_commands(self, sdk, calls, *, unsupported=False, timeout=False, stderr="", malformed=False):
        def execute(argv, label, output, report, env, wait, *, deadline):
            argv = [str(value) for value in argv]
            calls.append((argv, label, dict(env), wait, deadline))
            event = {"label": label, "returncode": 0, "timed_out": False, "interrupted": False}
            report["commands"].append(event)
            if label == "sdk-version":
                return event, "26.5", ""
            if label == "sdk-path":
                return event, str(sdk), ""
            if label.endswith("-compile"):
                Path(argv[argv.index("-o") + 1]).write_bytes(b"synthetic fake executable; not Apple evidence")
            if label.endswith("-run"):
                if timeout:
                    event.update(returncode=-9, timed_out=True)
                    return event, "", "synthetic timeout"
                if unsupported:
                    event["returncode"] = 78
                Path(argv[argv.index("--report") + 1]).write_text(
                    '{"schema":1,"schema":1}' if malformed else json.dumps(synthetic_native(unsupported)), encoding="utf-8")
                return event, "", stderr
            return event, "", ""
        return execute

    def test_unsupported_host_retains_source_hashes_and_refuses_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "fresh"
            args = probe.parse_args(["--output", str(output)])
            with patch.object(probe.platform, "system", return_value="Linux"), patch.object(probe.control, "run_command") as command:
                report = probe.run(args)
            command.assert_not_called()
            self.assertEqual(report["status"], "unsupported_host")
            self.assertFalse(report["legacy_async_library_verified"])
            self.assertEqual((output / "mrc_metal.mm").read_bytes(), (probe.HERE / "mrc_metal.mm").read_bytes())
            self.assertEqual(json.loads((output / "result.json").read_text()), report)
            with self.assertRaises(FileExistsError):
                probe.run(args)

    def test_mocked_routes_use_mrc_legacy_metal_and_separate_sanitized_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            calls = []
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, calls)), \
                    patch.dict(probe.os.environ, {"CXXFLAGS": "-fobjc-arc"}):
                report = probe.run(probe.parse_args(["--output", str(sdk / "fresh")]))
            self.assertEqual(report["status"], "passed")
            self.assertEqual([row["variant"] for row in report["variants"]], ["normal", "asan-ubsan"])
            for argv, label, env, wait, deadline in calls:
                self.assertGreater(deadline, 0)
                self.assertEqual(wait, 60)
                self.assertNotIn("CXXFLAGS", env)
                if label.endswith("-compile"):
                    self.assertIn("clang++", argv)
                    self.assertIn("-fno-objc-arc", argv)
                    self.assertNotIn("-fobjc-arc", argv)
                    self.assertIn("Metal", argv)
                    self.assertNotIn("-fsanitize=thread", argv)
                    self.assertEqual("-fsanitize=address,undefined" in argv, label.startswith("asan-ubsan"))
            for key in probe.FALSE_EVIDENCE:
                self.assertIs(report[key], False)

    def test_native_unsupported_stops_without_claiming_unrun_variant(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            calls = []
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, calls, unsupported=True)):
                report = probe.run(probe.parse_args(["--output", str(sdk / "fresh")]))
            self.assertEqual(report["status"], "unsupported_device")
            self.assertEqual(len(report["variants"]), 1)
            self.assertEqual(report["variants"][0]["status"], "unsupported")
            self.assertFalse(report["exact_compute_readback_verified"])

    def test_timeout_is_not_replaced_with_missing_json_error(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            with patch.object(probe.platform, "system", return_value="Darwin"), \
                    patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, [], timeout=True)):
                report = probe.run(probe.parse_args(["--output", str(sdk / "fresh")]))
            self.assertEqual(report["status"], "failed")
            self.assertTrue(report["commands"][-1]["timed_out"])
            self.assertNotIn("FileNotFoundError", report["reason"])
            self.assertFalse(report["legacy_async_library_verified"])

    def test_stderr_or_duplicate_native_json_is_retained_failure(self):
        for kwargs in ({"stderr": "synthetic sanitizer finding"}, {"malformed": True}):
            with self.subTest(kwargs=kwargs), tempfile.TemporaryDirectory() as directory:
                sdk = Path(directory)
                output = sdk / "fresh"
                with patch.object(probe.platform, "system", return_value="Darwin"), \
                        patch.object(probe.control, "run_command", side_effect=self.fake_commands(sdk, [], **kwargs)):
                    report = probe.run(probe.parse_args(["--output", str(output)]))
                self.assertEqual(report["status"], "failed")
                self.assertTrue((output / "normal-native.json").is_file())
                self.assertFalse(report["exact_compute_readback_verified"])

    def test_absolute_budget_is_passed_and_prevents_next_command(self):
        now = [0.0]
        deadlines = []
        def command(argv, label, output, report, env, wait, *, deadline):
            deadlines.append(deadline)
            now[0] = 181.0
            return {"returncode": 0, "timed_out": False}, "26.5", ""
        with tempfile.TemporaryDirectory() as directory, patch.object(probe.platform, "system", return_value="Darwin"), \
                patch.object(probe.time, "monotonic", side_effect=lambda: now[0]), \
                patch.object(probe.control, "run_command", side_effect=command):
            report = probe.run(probe.parse_args(["--output", str(Path(directory) / "fresh")]))
        self.assertEqual(deadlines, [180.0])
        self.assertEqual(report["status"], "failed")
        self.assertIn("budget exhausted", report["reason"])

    def test_interruption_retains_terminal_atomic_report(self):
        for error in (KeyboardInterrupt, SystemExit):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "fresh"
                with patch.object(probe.platform, "system", return_value="Darwin"), \
                        patch.object(probe.control, "run_command", side_effect=error), self.assertRaises(error):
                    probe.run(probe.parse_args(["--output", str(output)]))
                self.assertEqual(json.loads((output / "result.json").read_text())["status"], "interrupted")
                self.assertFalse((output / "result.tmp").exists())

    def test_invalid_limits_and_duplicate_variants_are_rejected(self):
        for argv in (["--budget", "nan"], ["--budget", "721"], ["--budget", "0"], ["--timeout", "-1"],
                     ["--variants", "normal", "normal"]):
            with self.subTest(argv=argv), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", *argv])


if __name__ == "__main__":
    unittest.main()
