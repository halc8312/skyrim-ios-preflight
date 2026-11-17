"""Synthetic transfer/runner controls only; no Windows binary or GPU evidence."""
import base64
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


probe = load("windows_fixture_tested", ROOT / "experiments/metal-compile/windows_fixture.py")
runtime_controls = load("windows_fixture_synthetic_reports", ROOT / "tests/test_metal_compile_runtime.py")
# This is not the pinned metallib. Positive orchestration tests temporarily pin
# only this synthetic buffer; no real artifact or its base64 is tracked here.
SYNTHETIC_LIBRARY = b"MTLB" + b"X" * (probe.LIBRARY_BYTES - 4)
SYNTHETIC_SHA256 = hashlib.sha256(SYNTHETIC_LIBRARY).hexdigest()


def child_report():
    native = runtime_controls.synthetic_report(3, True, True, legacy_only=True)
    report = {"schema": 1, "scope": probe.runtime.SCOPE, "status": "passed", "repeat": 3,
              "comparison_mode": "legacy-only", "legacy_only_requested": True,
              "native_compilation_verified": True, "gpu_execution_verified": True,
              "async_library_requested": True, "async_adapter_verified": True,
              "async_apis": ["legacy_async_source"], "metal4_library_verified": False,
              "metal4_async_adapter_verified": False, "library_factory_probe_requested": False,
              "host": {"system": "Darwin"}, "native": native,
              "inputs": {"fixture.metal": {"sha256": probe.SOURCE_SHA256},
                         "offline.metallib": {"sha256": SYNTHETIC_SHA256}},
              **{key: False for key in probe.runtime.FALSE_EVIDENCE}}
    return report, native


class WindowsFixtureControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "fresh"
        self.encoded = base64.b64encode(SYNTHETIC_LIBRARY).decode("ascii")
        self.calls = []

    def invoke(self, *, encoded=None, missing_input=False, system="Darwin", budget=120,
               env_extra=None, code=0, timeout=False, mutate=None, missing_report=False,
               corrupt_report=False, mutate_library=False, mutate_original=False, interrupt=False,
               pin_synthetic=True):
        def command(argv, label, output, report, env, limit, *, deadline):
            argv = list(map(str, argv))
            self.calls.append((argv, dict(env), limit, deadline))
            self.assertNotIn(probe.INPUT_ENV, env)
            self.assertNotIn("CFLAGS", env)
            self.assertNotIn(self.encoded, json.dumps(argv))
            child = Path(argv[argv.index("--output") + 1])
            child.mkdir()
            native_report, native = child_report()
            if mutate:
                mutate(native_report, native)
            (child / "fixture.metal").write_bytes((output / "fixture.metal").read_bytes())
            (child / "offline.metallib").write_bytes(SYNTHETIC_LIBRARY if not mutate_library else b"MTLBwrong bytes")
            if not missing_report:
                (child / "result.json").write_text('{"schema":1,"schema":1}' if corrupt_report else json.dumps(native_report), encoding="utf-8")
                (child / "native.json").write_text(json.dumps(native), encoding="utf-8")
            if mutate_original:
                (output / "windows-fixture.metallib").write_bytes(b"changed original")
            event = {"label": label, "returncode": None if timeout else code, "timed_out": timeout,
                     "process_started": True, "budget_exhausted": False,
                     "stdout": "runtime.stdout.txt", "stderr": "runtime.stderr.txt"}
            (output / event["stdout"]).write_text("synthetic child only\n", encoding="utf-8")
            (output / event["stderr"]).write_text("synthetic tool failure" if code or timeout else "", encoding="utf-8")
            report["commands"].append(event)
            if interrupt:
                raise KeyboardInterrupt()
            return event, "", ""

        env = {"PATH": os.environ.get("PATH", ""), "CFLAGS": "must not reach child", **(env_extra or {})}
        if not missing_input:
            env[probe.INPUT_ENV] = self.encoded if encoded is None else encoded
        args = probe.parse_args(["--output", str(self.output), "--budget", str(budget)])
        with patch.dict(probe.os.environ, env, clear=True), \
                patch.object(probe.platform, "system", return_value=system), \
                patch.object(probe.time, "monotonic", return_value=100.0), \
                patch.object(probe.control, "run_command", side_effect=command), \
                patch.object(probe, "LIBRARY_SHA256", SYNTHETIC_SHA256 if pin_synthetic else probe.LIBRARY_SHA256):
            return probe.run(args)

    def saved(self):
        return json.loads((self.output / "result.json").read_text(encoding="utf-8"))

    def test_policy_pins_only_the_declared_artifact_and_source(self):
        self.assertEqual(probe.LIBRARY_BYTES, 11352)
        self.assertEqual(probe.LIBRARY_SHA256, "86dc2baa8ab47c4523318570c6eaed9053ed0b46200a311afd5b43cf2d9bfc0c")
        self.assertEqual(probe.SOURCE_BYTES, 1024)
        self.assertEqual(probe.SOURCE_SHA256, "e45755523aabf64eb236c01a77ae220e271b4ae6d0eabcf120d9a80cabbada87")
        self.assertEqual(probe.parse_args(["--output", "unused"]).budget, 120)
        with self.assertRaisesRegex(ValueError, "SHA-256 differs"):
            probe.decode_library(self.encoded)

    def test_bounded_strict_decode_rejects_absent_non_ascii_whitespace_and_wrong_bytes(self):
        candidates = (None, "", self.encoded + "A", self.encoded[:-1],
                      "!" + self.encoded[1:], "\u00e9" + self.encoded[1:], "\n" + self.encoded[1:],
                      base64.b64encode(b"NOPE" + SYNTHETIC_LIBRARY[4:]).decode("ascii"),
                      base64.b64encode(SYNTHETIC_LIBRARY[:-1] + b"Y").decode("ascii"))
        for index, value in enumerate(candidates):
            with self.subTest(index=index):
                self.output = self.root / ("invalid-" + str(index))
                report = self.invoke(encoded=value, missing_input=value is None)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["input_artifact_verified"])
                self.assertFalse(report["cross_host_gpu_verified"])
                self.assertEqual(self.calls, [])
                self.assertFalse((self.output / "windows-fixture.metallib").exists())
                self.assertNotIn(self.encoded, json.dumps(report))

    def test_mismatched_source_stops_before_payload_or_child(self):
        with patch.object(probe, "SOURCE_SHA256", "0" * 64):
            report = self.invoke()
        self.assertEqual(report["status"], "failed")
        self.assertIn("Source SHA-256 differs", report["reason"])
        self.assertFalse(report["input_artifact_verified"])
        self.assertEqual(self.calls, [])

    def test_success_requires_offline_and_both_legacy_routes_all_three_times(self):
        report = self.invoke()
        self.assertEqual(report["status"], "passed")
        self.assertEqual(self.saved(), report)
        self.assertTrue(report["cross_host_gpu_verified"])
        self.assertTrue(report["input_artifact_verified"])
        self.assertTrue(all(report[key] is False for key in probe.FALSE_EVIDENCE))
        argv, env, limit, deadline = self.calls[0]
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(argv[:3], [sys.executable, "-c", probe.RUNTIME_BOOTSTRAP])
        self.assertIn("--legacy-only", argv)
        self.assertIn("--async-library", argv)
        self.assertEqual(argv[argv.index("--repeat") + 1], "3")
        self.assertNotIn("--probe-library-factory", argv)
        self.assertEqual((float(argv[3]), limit, deadline), (210.0, 120.0, 220.0))
        self.assertEqual(float(argv[argv.index("--budget") + 1]), 110.0)
        self.assertNotIn(probe.INPUT_ENV, env)
        self.assertNotIn(self.encoded, json.dumps(report))
        self.assertEqual((self.output / "windows-fixture.metallib").read_bytes(), SYNTHETIC_LIBRARY)

    def test_child_bootstrap_sets_earlier_deadline_without_forwarding_input(self):
        arguments = ["-c", "210.0", "/authored/runtime.py", "--output", "/fresh", "--legacy-only"]
        with patch.object(sys, "argv", arguments), patch.dict(os.environ, {probe.control.TEST_DEADLINE_ENV: "220.0"}, clear=True), \
                patch("runpy.run_path") as run:
            exec(probe.RUNTIME_BOOTSTRAP, {})
            self.assertEqual(os.environ[probe.control.TEST_DEADLINE_ENV], "210.0")
            self.assertEqual(sys.argv, arguments[2:])
            self.assertNotIn(probe.INPUT_ENV, os.environ)
            run.assert_called_once_with("/authored/runtime.py", run_name="__main__")

    def test_inherited_deadline_shortens_both_parent_and_child_with_cleanup_reserve(self):
        report = self.invoke(env_extra={probe.control.TEST_DEADLINE_ENV: "150.0"})
        self.assertEqual(report["status"], "passed")
        argv, env, limit, deadline = self.calls[0]
        self.assertEqual((float(argv[3]), limit, deadline), (140.0, 50.0, 150.0))
        self.assertEqual(env[probe.control.TEST_DEADLINE_ENV], "150.0")

    def test_expired_or_invalid_budget_never_starts_a_child(self):
        for index, (budget, extra) in enumerate(((5, {}), (120, {probe.control.TEST_DEADLINE_ENV: "105"}),
                (120, {probe.control.TEST_DEADLINE_ENV: "nan"}))):
            with self.subTest(index=index):
                self.output = self.root / ("deadline-" + str(index))
                report = self.invoke(budget=budget, env_extra=extra)
                self.assertEqual(report["status"], "failed" if index == 2 else "budget_exhausted")
                self.assertFalse(report["cross_host_gpu_verified"])
                self.assertEqual(self.calls, [])

    def test_wrong_offline_gpu_word_missing_route_or_async_check_cannot_pass(self):
        changes = (
            lambda report, native: native["samples"][-1]["compute_words"].__setitem__(0, 99),
            lambda report, native: native["samples"][-1]["render_bits"].__setitem__(63, 0),
            lambda report, native: native["samples"].pop(),
            lambda report, native: native["async_retention_checks"].pop(),
            lambda report, native: native.__setitem__("metal4_library_verified", True),
            lambda report, native: report.__setitem__("gpu_execution_verified", False),
            lambda report, native: report.__setitem__("performance_improvement_verified", True),
            lambda report, native: report["inputs"]["offline.metallib"].__setitem__("sha256", "0" * 64),
        )
        for index, mutation in enumerate(changes):
            with self.subTest(index=index):
                self.output = self.root / ("wrong-result-" + str(index))
                report = self.invoke(mutate=mutation)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["cross_host_gpu_verified"])

    def test_claimed_hash_cannot_hide_changed_retained_library_or_original(self):
        for index, options in enumerate(({"mutate_library": True}, {"mutate_original": True})):
            with self.subTest(options=options):
                self.output = self.root / ("changed-" + str(index))
                report = self.invoke(**options)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["cross_host_gpu_verified"])

    def test_child_failure_timeout_missing_and_corrupt_reports_remain_failures(self):
        for index, options in enumerate(({"code": 1}, {"timeout": True}, {"code": 1, "missing_report": True},
                                         {"missing_report": True}, {"corrupt_report": True})):
            with self.subTest(options=options):
                self.output = self.root / ("child-failure-" + str(index))
                report = self.invoke(**options)
                self.assertEqual(report["status"], "failed")
                self.assertFalse(report["cross_host_gpu_verified"])
                self.assertEqual(self.saved(), report)
                if options.get("code"):
                    self.assertIn("exit status 1", report["reason"])
                    self.assertNotIn("FileNotFoundError", report["reason"])

    def test_interruption_keeps_partial_child_report_and_rethrows(self):
        with self.assertRaises(KeyboardInterrupt):
            self.invoke(interrupt=True)
        report = self.saved()
        self.assertEqual(report["status"], "interrupted")
        self.assertFalse(report["cross_host_gpu_verified"])
        self.assertIn("runtime", report)

    def test_non_mac_never_launches_and_existing_output_is_never_reused(self):
        report = self.invoke(system="Windows")
        self.assertEqual(report["status"], "unsupported_host")
        self.assertFalse(report["cross_host_gpu_verified"])
        self.assertEqual(self.calls, [])
        before = (self.output / "result.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.invoke()
        self.assertEqual((self.output / "result.json").read_bytes(), before)

    def test_public_checkout_output_and_invalid_limits_are_rejected(self):
        self.output = ROOT / "experiments/metal-compile/forbidden-windows-fixture"
        with self.assertRaises(probe.batch.BatchError):
            self.invoke()
        self.assertFalse(self.output.exists())
        for value in ("0", "-1", "nan", "inf", "3601"):
            with self.subTest(value=value), patch("sys.stderr"), self.assertRaises(SystemExit):
                probe.parse_args(["--output", "unused", "--budget", value])

    def test_main_exit_status_does_not_promote_missing_gpu_evidence(self):
        for status, expected in (("passed", 0), ("failed", 1), ("unsupported_host", 78)):
            with self.subTest(status=status), patch.object(probe, "run", return_value={"status": status}), patch("builtins.print"):
                self.assertEqual(probe.main(["--output", "unused"]), expected)


if __name__ == "__main__":
    unittest.main()
