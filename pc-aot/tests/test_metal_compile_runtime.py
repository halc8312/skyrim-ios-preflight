"""Portable control/validation tests only: no Apple compilation or GPU evidence."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_compile_runtime", ROOT / "experiments/metal-compile/runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


def synthetic_report(repeats=2, offline=True):
    """Authored JSON for parser tests; deliberately not returned by a Metal process."""
    result = {
        "schema": 1, "status": "passed", "scope": runtime.SCOPE,
        "source_options": copy.deepcopy(runtime.SOURCE_OPTIONS),
        "gpu_execution_verified": True, "repeat": repeats, "offline_library_requested": offline,
        "os_version": [26, 0, 0], "device": {"name": "SYNTHETIC NON-GPU", "supports_metal4": True},
        "compiler_setup_ns": 100, "samples": [], "rejections": [],
        "specialization_scope": "vertex lookup plus fragment and compute specialization",
        **{key: False for key in runtime.FALSE_EVIDENCE},
    }
    # Explicit bit patterns are independent of the runner's float-to-bits calculation.
    color = [0x3e400000, 0x3ee00000, 0x3f000000, 0x3f800000]
    compute = [3, 7, 10, 0x12345678, 4, 14, 10, 0x12345678,
               5, 21, 10, 0x12345678, 6, 28, 10, 0x12345678]
    for iteration in range(1, repeats + 1):
        modes = ["legacy_source", "metal4_source"] if iteration % 2 else ["metal4_source", "legacy_source"]
        if offline:
            modes.append("offline_metallib")
        for api in modes:
            result["samples"].append({
                "api": api, "iteration": iteration,
                "library_operation": "load" if api == "offline_metallib" else "compile",
                "library_ns": 101, "specialization_ns": 102, "render_pso_ns": 103, "compute_pso_ns": 104,
                "render_bits": color * 16, "compute_words": list(compute), "command_status": "completed",
            })
    for api in ("legacy_source", "metal4_source"):
        result["rejections"].append({
            "api": api, "library_ns": 105, "returned_nil": True, "error_present": True,
            "error": {"domain": "SyntheticCompilerError", "code": 3},
        })
    return result


class MetalRuntimeReportTests(unittest.TestCase):
    def validate(self, report):
        runtime.validate_native(report, 2, True)

    def test_valid_control_reports_with_and_without_offline(self):
        self.validate(synthetic_report())
        runtime.validate_native(synthetic_report(1, False), 1, False)

    def test_all_pixels_and_compute_threads_are_checked_even_if_native_claims_pass(self):
        for key, index in (("render_bits", 63), ("compute_words", 15)):
            with self.subTest(key=key):
                report = synthetic_report()
                report["samples"][-1][key][index] ^= 1
                with self.assertRaisesRegex(ValueError, "readback mismatch"):
                    self.validate(report)

    def test_missing_duplicate_reordered_or_mislabeled_routes_fail(self):
        mutations = (
            lambda r: r["samples"].pop(),
            lambda r: r["samples"].__setitem__(1, copy.deepcopy(r["samples"][0])),
            lambda r: r["samples"].reverse(),
            lambda r: r["samples"][2].__setitem__("library_operation", "compile"),
        )
        for mutation in mutations:
            report = synthetic_report()
            mutation(report)
            with self.assertRaises(ValueError):
                self.validate(report)

    def test_both_real_errors_require_nil_and_diagnostic(self):
        mutations = (
            lambda r: r["rejections"].pop(),
            lambda r: r["rejections"][1].__setitem__("returned_nil", False),
            lambda r: r["rejections"][0].__setitem__("error_present", False),
            lambda r: r["rejections"][1].__setitem__("error", {}),
        )
        for mutation in mutations:
            report = synthetic_report()
            mutation(report)
            with self.assertRaises(ValueError):
                self.validate(report)

    def test_options_are_exact_and_false_evidence_cannot_be_promoted(self):
        for key, value in (("fast_math", True), ("language", "4.0"), ("fast_math", 0)):
            report = synthetic_report()
            report["source_options"][key] = value
            with self.assertRaisesRegex(ValueError, "options differ"):
                self.validate(report)
        for key in runtime.FALSE_EVIDENCE:
            report = synthetic_report()
            report[key] = True
            with self.assertRaisesRegex(ValueError, "evidence claim"):
                self.validate(report)

    def test_unsupported_or_unexecuted_cannot_be_a_pass(self):
        mutations = (
            lambda r: r.__setitem__("status", "unsupported"),
            lambda r: r.__setitem__("gpu_execution_verified", False),
            lambda r: r.__setitem__("os_version", [15, 0, 0]),
            lambda r: r["device"].__setitem__("supports_metal4", False),
            lambda r: r["samples"][0].__setitem__("command_status", "error"),
        )
        for mutation in mutations:
            report = synthetic_report()
            mutation(report)
            with self.assertRaises(ValueError):
                self.validate(report)

    def test_invalid_durations_and_non_integer_words_fail(self):
        for value in (-1, True, float("nan"), "100"):
            report = synthetic_report()
            report["samples"][0]["library_ns"] = value
            with self.assertRaises(ValueError):
                self.validate(report)
        report = synthetic_report()
        report["samples"][0]["compute_words"][0] = 3.0
        with self.assertRaises(ValueError):
            self.validate(report)


class MetalRuntimeControlTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="metal-runtime-control-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.output = self.directory / "fresh evidence with spaces"

    def arguments(self, *extra):
        return runtime.parse_args(["--output", str(self.output), "--repeat", "1", *extra])

    def test_non_mac_records_explicit_unsupported_with_source_hashes_and_no_process(self):
        with patch.object(runtime.platform, "system", return_value="Windows"), \
                patch.object(runtime.subprocess, "Popen") as spawn:
            report = runtime.run_experiment(self.arguments())
        spawn.assert_not_called()
        self.assertEqual(report["status"], "unsupported")
        self.assertIs(report["native_compilation_verified"], False)
        self.assertIs(report["gpu_execution_verified"], False)
        for name in ("fixture.metal", "runtime.mm"):
            self.assertEqual(report["inputs"][name]["sha256"], hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        self.assertEqual(report, json.loads((self.output / "result.json").read_text(encoding="utf-8")))
        before = (self.output / "result.json").read_bytes()
        with self.assertRaises(FileExistsError):
            runtime.run_experiment(self.arguments())
        self.assertEqual(before, (self.output / "result.json").read_bytes())

    def test_old_mac_os_is_unsupported_before_compilation(self):
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("15.7", (), "")), \
                patch.object(runtime.subprocess, "Popen") as spawn:
            report = runtime.run_experiment(self.arguments())
        self.assertEqual(report["status"], "unsupported")
        spawn.assert_not_called()

    def test_empty_library_fails_and_does_not_claim_native_execution(self):
        library = self.directory / "empty.metallib"
        library.write_bytes(b"")
        report = runtime.run_experiment(self.arguments("--library", str(library)))
        self.assertEqual(report["status"], "failed")
        self.assertIn("empty", report["failure"])
        self.assertFalse(report["native_compilation_verified"])

    def test_interruption_retains_report_then_propagates(self):
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.0", (), "")), \
                patch.object(runtime, "probe", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                runtime.run_experiment(self.arguments())
        report = json.loads((self.output / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "interrupted")
        self.assertFalse(report["gpu_execution_verified"])

    def test_sdk_failure_retains_failure_instead_of_native_pass(self):
        with patch.object(runtime.platform, "system", return_value="Darwin"), \
                patch.object(runtime.platform, "mac_ver", return_value=("26.0", (), "")), \
                patch.object(runtime, "probe", side_effect=RuntimeError("SDK discovery failed")):
            report = runtime.run_experiment(self.arguments())
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["native_compilation_verified"])
        self.assertIn("SDK discovery", report["failure"])

    def run_child(self, script, timeout=10):
        self.output.mkdir()
        report = {"commands": []}
        command = [sys.executable, "-u", "-c", script]
        event = runtime.run_command(command, "synthetic-child", self.output, report, os.environ.copy(), timeout)
        self.assertEqual(report["commands"], [event])
        return event

    def test_real_synthetic_child_retains_both_streams_and_nonzero_exit(self):
        event = self.run_child("import sys; print('SYNTHETIC stdout'); print('SYNTHETIC stderr', file=sys.stderr); sys.exit(23)")
        self.assertEqual(event["returncode"], 23)
        self.assertEqual(event["status"], "completed")
        self.assertIn("SYNTHETIC stdout", (self.output / event["stdout"]).read_text())
        self.assertIn("SYNTHETIC stderr", (self.output / event["stderr"]).read_text())

    def test_real_synthetic_child_timeout_retains_partial_output_and_is_reaped(self):
        self.output.mkdir()
        report = {"commands": []}
        with self.assertRaisesRegex(RuntimeError, "deadline"):
            runtime.run_command([sys.executable, "-u", "-c", "import time; print('before timeout'); time.sleep(20)"],
                                "timeout", self.output, report, os.environ.copy(), 1)
        event = report["commands"][0]
        self.assertEqual(event["status"], "timed_out")
        self.assertIsNotNone(event["returncode"])
        self.assertIn("before timeout", (self.output / event["stdout"]).read_text())
        self.assertLess(event["duration_ns"], 8_000_000_000)

    def test_command_interrupt_stops_child_before_propagating(self):
        self.output.mkdir()
        report = {"commands": []}
        process = unittest.mock.Mock()
        process.pid = 12345
        process.wait.side_effect = [KeyboardInterrupt, 0]
        process.returncode = -9
        with patch.object(runtime.subprocess, "Popen", return_value=process), \
                patch.object(runtime.os, "name", "nt"):
            with self.assertRaises(KeyboardInterrupt):
                runtime.run_command(["synthetic"], "interrupted", self.output, report, {}, 10)
        process.kill.assert_called_once()
        self.assertEqual(process.wait.call_count, 2)
        self.assertEqual(report["commands"][0]["status"], "interrupted")

    def test_cli_does_not_return_success_for_unsupported(self):
        with patch.object(runtime.platform, "system", return_value="Windows"), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runtime.main(["--output", str(self.output)]), 78)


if __name__ == "__main__":
    unittest.main()
