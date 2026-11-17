"""Authored inputs only. Native controls run in fresh private Linux processes."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


candidate = load("shader_candidate_check", ROOT / "experiments/shader-inputs/candidate_check.py")
fixtures = load("shader_candidate_authored_fixtures", ROOT / "tests/test_shader_input_study.py")


def authored_archive():
    records = []
    for marker, version in ((b"BSVS", fixtures.VS), (b"BSPS", fixtures.PS)):
        for index in range(20):
            code = fixtures.words(version, ((index + 1) << 16) | 0xfffe,
                                  *([index] * (index + 1)), 0x02000001,
                                  fixtures.DST, fixtures.SRC, fixtures.END)
            records.append(fixtures.record(code, marker=marker, key=index))
    return fixtures.bsa([(b"authored.fxp", b"".join(records))])[0]


class CandidateSelectionTests(unittest.TestCase):
    def test_stage_balanced_small_large_selection_is_deterministic(self):
        rows, summary = candidate.collect_candidates(authored_archive())
        selected = candidate.select_candidates(rows)
        self.assertEqual(summary["unique_keys"], 40)
        self.assertEqual(len(selected), 32)
        self.assertEqual(len({row["dxvk_key"] for row in selected}), 32)
        self.assertEqual([row["dxvk_key"] for row in selected],
                         [row["dxvk_key"] for row in candidate.select_candidates(list(reversed(rows)))])
        for stage in (1, 16):
            chosen = [row for row in selected if row["stage_vk"] == stage]
            self.assertEqual(len(chosen), 16)
            self.assertEqual([row["container_key"] for row in chosen], list(range(8)) + list(range(12, 20)))

    def test_insufficient_stage_and_selected_byte_cap_fail_explicitly(self):
        rows, _ = candidate.collect_candidates(authored_archive())
        with self.assertRaisesRegex(ValueError, "at least 16"):
            candidate.select_candidates(rows[:10])
        enlarged = [{**row, "analyzed_bytes": 1 << 20} for row in rows]
        with self.assertRaisesRegex(ValueError, "byte budget"):
            candidate.select_candidates(enlarged)

    def test_duplicate_keys_do_not_weight_selection(self):
        code = fixtures.record()
        raw, _ = fixtures.bsa([(b"duplicates.fxp", code + code)])
        rows, summary = candidate.collect_candidates(raw)
        self.assertEqual(summary["candidate_records"], 2)
        self.assertEqual(len(rows), 1)

    def test_malformed_shader_is_not_an_admitted_candidate(self):
        raw, _ = fixtures.bsa([(b"malformed.fxp", fixtures.record(fixtures.words(fixtures.VS, 0)))])
        rows, summary = candidate.collect_candidates(raw)
        self.assertEqual(rows, [])
        self.assertEqual(summary["candidate_records"], 0)


class CandidateCommandTests(unittest.TestCase):
    def test_expired_budget_never_starts_process_and_preserves_event(self):
        with tempfile.TemporaryDirectory() as value:
            output = Path(value)
            report = {"commands": []}
            with patch.object(candidate.time, "monotonic", return_value=10), \
                    patch.object(candidate.subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(RuntimeError, "deadline exhausted"):
                    candidate.run_command(["not-run"], "expired", output, report, 9)
            spawn.assert_not_called()
            event = json.loads((output / "expired.json").read_text())
            self.assertEqual(event["status"], "budget_exhausted")
            self.assertFalse(event["process_started"])

    def test_manual_interrupt_stops_only_owned_group_and_retains_failure(self):
        with tempfile.TemporaryDirectory() as value:
            output = Path(value)
            process = Mock(pid=123456, returncode=None)
            termination_signal = getattr(signal, "SIGKILL", 9)
            calls = 0
            def wait(timeout):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise KeyboardInterrupt
                process.returncode = -termination_signal
                return process.returncode
            # The first wait is interrupted while the process is still live.
            process.wait.side_effect = wait
            with patch.object(candidate.subprocess, "Popen", return_value=process), \
                    patch.object(candidate.signal, "SIGKILL", termination_signal, create=True), \
                    patch.object(candidate.os, "killpg", create=True) as kill:
                with self.assertRaises(KeyboardInterrupt):
                    candidate.run_command(["mocked"], "interrupt", output, {"commands": []}, time.monotonic() + 5)
            kill.assert_called_once_with(123456, termination_signal)
            event = json.loads((output / "interrupt.json").read_text())
            self.assertEqual(event["status"], "interrupted")
            self.assertTrue(event["process_started"])

    def test_zero_exit_without_isolation_or_exact_result_is_not_success(self):
        expected = candidate.study.validate_sm3(fixtures.shader(), b"BSVS")
        with tempfile.TemporaryDirectory() as value:
            output = Path(value)
            event = {"stdout": "stdout", "stderr": "stderr", "returncode": 0, "timed_out": False}
            (output / "stderr").write_text("")
            (output / "stdout").write_text("{}\n")
            with self.assertRaisesRegex(RuntimeError, "isolation"):
                candidate.check_analysis(event, output, expected)
            (output / "stdout").write_text(json.dumps(candidate.ISOLATION) + "\n")
            with self.assertRaisesRegex(RuntimeError, "mismatch"):
                candidate.check_analysis(event, output, expected)

    def test_oversized_native_log_is_rejected_before_parsing(self):
        with tempfile.TemporaryDirectory() as value:
            output = Path(value)
            (output / "stdout").write_bytes(b"x" * ((1 << 20) + 1))
            (output / "stderr").write_bytes(b"")
            with self.assertRaisesRegex(RuntimeError, "hard file limit"):
                candidate.read_rows({"stdout": "stdout", "stderr": "stderr"}, output)

    def test_timeout_reap_failure_preserves_timeout_without_repeating_cleanup(self):
        with tempfile.TemporaryDirectory() as value:
            output = Path(value)
            process = Mock(pid=123456, returncode=None)
            process.wait.side_effect = [candidate.subprocess.TimeoutExpired("authored", 1),
                                        candidate.subprocess.TimeoutExpired("authored reap", 5)]
            with patch.object(candidate.subprocess, "Popen", return_value=process), \
                    patch.object(candidate.signal, "SIGKILL", getattr(signal, "SIGKILL", 9), create=True), \
                    patch.object(candidate.os, "killpg", create=True) as kill:
                with self.assertRaisesRegex(RuntimeError, "could not be reaped"):
                    candidate.run_command(["mocked"], "timeout-reap", output, {"commands": []}, time.monotonic() + 5)
            self.assertEqual(process.wait.call_count, 2)
            self.assertEqual(kill.call_count, 1)
            event = json.loads((output / "timeout-reap.json").read_text())
            self.assertEqual(event["status"], "timeout")
            self.assertTrue(event["timed_out"])
            self.assertFalse(event["cleanup"]["parent_reaped"])


@unittest.skipUnless(os.name == "posix", "real owned-process SIGINT control requires POSIX")
class CandidateProcessControlTests(unittest.TestCase):
    def test_sigint_after_popen_return_before_caller_assignment_reaps_child(self):
        output = candidate.study.private_output(ROOT / "private/analysis" /
                  f"candidate-interrupt-control-{time.time_ns()}-{os.getpid()}")
        report = {"commands": []}
        original_spawn = candidate.subprocess.Popen
        original_handler = signal.getsignal(signal.SIGINT)
        owned = []

        def return_with_interrupt(*args, **kwargs):
            child = original_spawn(*args, **kwargs)
            owned.append(child)
            # The caller has not yet received/stored this Popen object.
            os.kill(os.getpid(), signal.SIGINT)
            return child

        try:
            with patch.object(candidate.subprocess, "Popen", side_effect=return_with_interrupt):
                with self.assertRaises(KeyboardInterrupt):
                    candidate.run_command([sys.executable, "-c", "import time; time.sleep(60)"],
                                          "creation-interrupt", output, report, time.monotonic() + 5)
            self.assertEqual(signal.getsignal(signal.SIGINT), original_handler)
            self.assertEqual(len(owned), 1)
            self.assertEqual(owned[0].returncode, -signal.SIGKILL)
            event = json.loads((output / "creation-interrupt.json").read_text())
            self.assertEqual(event["status"], "interrupted")
            self.assertTrue(event["process_started"])
            self.assertTrue(event["cleanup"]["parent_reaped"])
        finally:
            for child in owned:
                if child.returncode is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=5)


@unittest.skipUnless(sys.platform == "linux" and shutil.which("g++") and shutil.which("prlimit"),
                     "requires existing Linux compiler and hard limits; no native pass on other hosts")
class CandidateNativeControlTests(unittest.TestCase):
    def test_authored_native_bounds_malformed_and_resource_controls(self):
        base = os.environ.get("SKYRIM_SHADER_CANDIDATE_TEST_OUTPUT")
        output = Path(base) if base else ROOT / "private/analysis" / f"candidate-controls-{time.time_ns()}-{os.getpid()}"
        report = candidate.run(SimpleNamespace(output=output, archive=None))
        self.assertEqual(report["status"], "authored_checks_passed", report.get("failure"))
        self.assertEqual(len(report["authored_checks"]), 28)
        self.assertTrue(report["authored_checks_passed"])
        self.assertEqual(report["actual_candidates_submitted"], 0)
        self.assertFalse(report["spirv_compilation_performed"])


if __name__ == "__main__":
    unittest.main()
