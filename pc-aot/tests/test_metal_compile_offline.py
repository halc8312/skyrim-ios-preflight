"""Synthetic control tests for the offline runner; these are not Apple/GPU evidence."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_compile_offline", ROOT / "experiments/metal-compile/offline.py")
offline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(offline)

# Execute real short-lived Python children to test stream capture, process exit,
# timeout and partial outputs. No child here is a Metal compiler. In particular,
# the fabricated MTLB prefix only tests the runner's documented structural check.
SYNTHETIC_TOOL = r'''
import os
from pathlib import Path
import sys
import time
import subprocess
name, *args = sys.argv[1:]
mode = os.environ.get("OFFLINE_SYNTHETIC_MODE", "success")
if "--version" in args:
    print("SYNTHETIC CONTROL TOOL; NOT APPLE METAL", flush=True)
    raise SystemExit(6 if mode == "version-unsupported" else 0)
output = Path(args[args.index("-o") + 1])
print("synthetic " + name + " stdout", flush=True)
print("synthetic " + name + " stderr", file=sys.stderr, flush=True)
if name == "metal":
    if mode == "missing-air":
        raise SystemExit(0)
    output.write_bytes(b"" if mode == "empty-air" else b"synthetic AIR; not executable")
    if mode == "timeout":
        time.sleep(5)
    if mode == "grandchild-timeout":
        child_code = ("import os,sys,time\nfrom pathlib import Path\n"
                      "print('synthetic grandchild stdout', flush=True)\n"
                      "print('synthetic grandchild stderr', file=sys.stderr, flush=True)\n"
                      "Path(sys.argv[1]).write_text(str(os.getpid()))\n"
                      "time.sleep(30)\n")
        subprocess.Popen([sys.executable, "-c", child_code, str(output.parent / "grandchild.pid")],
                         stdout=sys.stdout, stderr=sys.stderr)
        time.sleep(30)
    if mode == "compile-error" or (mode == "second-error" and output.parent.name == "trial-02"):
        raise SystemExit(23)
else:
    output.write_bytes(b"WRONG" if mode == "invalid-library" else
                      b"MTLB" if mode == "header-only" else b"MTLB synthetic payload; not executable")
    if mode == "link-error":
        raise SystemExit(24)
'''


class MetalOfflineControlTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="metal-offline-control-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.bin = self.directory / "tool path with spaces"
        self.bin.mkdir()
        for name in ("metal.exe", "metallib.exe"):
            (self.bin / name).write_bytes(b"synthetic placeholder; not an Apple executable")
        self.shim = self.directory / "synthetic.py"
        self.shim.write_text(SYNTHETIC_TOOL, encoding="utf-8")
        self.output = self.directory / "fresh results with spaces"
        self.original_popen = subprocess.Popen
        self.calls = []

    def arguments(self, *extra):
        return offline.parse_args(["--output", str(self.output), "--tools", str(self.bin),
                                   "--repeat", "1", *extra])

    def child(self, command, **kwargs):
        self.assertIs(kwargs["shell"], False)
        self.assertNotEqual(kwargs["stdout"], subprocess.PIPE)
        self.assertNotEqual(kwargs["stderr"], subprocess.PIPE)
        if Path(command[0]).name.casefold() == "taskkill.exe":
            return self.original_popen(command, **kwargs)
        self.calls.append((command, kwargs))
        self.assertIn(Path(command[0]).name, ("metal.exe", "metallib.exe"))
        if os.name == "nt":
            self.assertTrue(kwargs["creationflags"] & subprocess.CREATE_NEW_PROCESS_GROUP)
        else:
            self.assertIs(kwargs["start_new_session"], True)
        return self.original_popen([sys.executable, str(self.shim), Path(command[0]).stem, *command[1:]], **kwargs)

    def run_synthetic(self, mode="success", *extra):
        with patch.object(offline.platform, "system", return_value="Windows"), \
                patch.dict(os.environ, {"OFFLINE_SYNTHETIC_MODE": mode}), \
                patch.object(offline.subprocess, "Popen", side_effect=self.child):
            report = offline.run_experiment(self.arguments(*extra))
        self.assertEqual(report, json.loads((self.output / "result.json").read_text(encoding="utf-8")))
        for key in ("gpu_execution_verified", "physical_iphone_verified", "game_rendering_verified",
                    "performance_improvement_verified"):
            self.assertIs(report[key], False)
        for event in report["commands"]:
            self.assertTrue((self.output / event["stdout"]).is_file())
            self.assertTrue((self.output / event["stderr"]).is_file())
            self.assertGreaterEqual(event["duration_ns"], 0)
        return report

    def test_synthetic_success_keeps_source_options_hashes_and_separate_repeats(self):
        report = self.run_synthetic("success", "--repeat", "2")
        self.assertEqual(report["status"], "passed")
        self.assertTrue(report["offline_compile_verified"])
        self.assertEqual(len(report["trials"]), 2)
        self.assertEqual(len(report["commands"]), 6)
        self.assertEqual(report["source"]["sha256"], hashlib.sha256(offline.SOURCE.read_bytes()).hexdigest())
        self.assertEqual((self.output / "fixture.metal").read_bytes(), offline.SOURCE.read_bytes())
        for info in report["tools"].values():
            self.assertIn("SYNTHETIC CONTROL TOOL", info["version_output"])
            self.assertEqual(info["sha256"], hashlib.sha256(Path(info["path"]).read_bytes()).hexdigest())
        for trial in report["trials"]:
            self.assertEqual(trial["status"], "passed")
            self.assertGreater(trial["compile_duration_ns"], 0)
            self.assertGreater(trial["link_duration_ns"], 0)
            self.assertEqual(len(trial["artifacts"]), 2)
            for artifact in trial["artifacts"]:
                data = Path(artifact["path"]).read_bytes()
                self.assertEqual(artifact["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(artifact["bytes"], len(data))
        compiles = [event["argv"] for event in report["commands"] if event["label"].startswith("compile-")]
        self.assertNotEqual(compiles[0][-1], compiles[1][-1])
        self.assertIn("air64-apple-ios18.0", compiles[0])
        self.assertIn("-std=ios-metal2.4", compiles[0])
        self.assertIn("-DPROBE_BIAS=3", compiles[0])
        self.assertIn("-fno-fast-math", compiles[0])
        self.assertNotIn("-ffast-math", compiles[0])

    def test_compile_error_keeps_partial_air_streams_and_does_not_link(self):
        report = self.run_synthetic("compile-error")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual(report["status"], "failed")
        event = report["commands"][-1]
        self.assertEqual(event["returncode"], 23)
        self.assertEqual(event["label"], "compile-01")
        self.assertIn("synthetic metal stderr", (self.output / event["stderr"]).read_text())
        self.assertEqual(len(report["trials"][0]["artifacts"]), 1)
        self.assertFalse((self.output / "trial-01/fixture.metallib").exists())

    def test_link_error_is_failure_even_if_mtlb_output_exists(self):
        report = self.run_synthetic("link-error")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual(report["commands"][-1]["returncode"], 24)
        self.assertEqual(len(report["trials"][0]["artifacts"]), 2)
        self.assertTrue((self.output / "trial-01/fixture.metallib").read_bytes().startswith(b"MTLB"))

    def test_timeout_preserves_partial_streams_and_output_and_stops_sequence(self):
        report = self.run_synthetic("timeout", "--timeout", "0.5")
        self.assertFalse(report["offline_compile_verified"])
        event = report["commands"][-1]
        self.assertTrue(event["timed_out"])
        self.assertIsNone(event["returncode"])
        self.assertIn("synthetic metal stdout", (self.output / event["stdout"]).read_text())
        self.assertIn("synthetic metal stderr", (self.output / event["stderr"]).read_text())
        self.assertEqual(len(report["trials"][0]["artifacts"]), 1)
        self.assertNotIn("link-01", [event["label"] for event in report["commands"]])

    def test_timeout_with_inherited_grandchild_logs_has_a_bounded_return(self):
        started = time.monotonic()
        report = self.run_synthetic("grandchild-timeout", "--timeout", "1")
        elapsed = time.monotonic() - started
        # This is a deadlock guard, not a timing/performance benchmark. The old
        # PIPE/communicate path could wait for the descendant's 30-second sleep.
        self.assertLess(elapsed, 1 + offline.TREE_TERMINATION_TIMEOUT + 2 * offline.PARENT_REAP_TIMEOUT + 3)
        self.assertFalse(report["offline_compile_verified"])
        self.assertTrue((self.output / "trial-01/grandchild.pid").is_file())
        event = report["commands"][-1]
        self.assertTrue(event["timed_out"])
        self.assertTrue(event["termination"]["parent_exited"])
        if os.name == "nt":
            self.assertEqual(event["termination"]["returncode"], 0)
            self.assertIn("/T", event["termination"]["argv"])
            self.assertEqual(event["termination"]["argv"][2], str(event["pid"]))
        else:
            self.assertTrue(event["termination"]["tree_signal_sent"])
        self.assertIn("synthetic grandchild stdout", (self.output / event["stdout"]).read_text())
        self.assertIn("synthetic grandchild stderr", (self.output / event["stderr"]).read_text())

    def test_interruption_is_recorded_and_reraised_after_child_cleanup(self):
        def interrupting_child(command, **kwargs):
            process = self.child(command, **kwargs)
            if "-c" not in command:
                return process
            original_wait = process.wait
            interrupted = False
            artifact = Path(command[-1])

            def wait(timeout):
                nonlocal interrupted
                if not interrupted:
                    interrupted = True
                    deadline = time.monotonic() + 3
                    while not artifact.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertTrue(artifact.exists())
                    raise KeyboardInterrupt()
                return original_wait(timeout=timeout)

            process.wait = wait
            return process

        with patch.object(offline.platform, "system", return_value="Windows"), \
                patch.dict(os.environ, {"OFFLINE_SYNTHETIC_MODE": "timeout"}), \
                patch.object(offline.subprocess, "Popen", side_effect=interrupting_child):
            with self.assertRaises(KeyboardInterrupt):
                offline.run_experiment(self.arguments())
        report = json.loads((self.output / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "interrupted")
        self.assertFalse(report["offline_compile_verified"])
        event = report["commands"][-1]
        self.assertTrue(event["interrupted"])
        self.assertTrue(event["termination"]["parent_exited"])
        self.assertIn("synthetic metal stdout", (self.output / event["stdout"]).read_text())
        self.assertIn("synthetic metal stderr", (self.output / event["stderr"]).read_text())

    def test_large_raw_logs_are_retained_but_returned_preview_is_bounded(self):
        self.output.mkdir()
        amount = 3 * offline.LOG_PREVIEW_BYTES
        report = {"commands": []}
        command = [sys.executable, "-c", f"import sys; sys.stdout.write('x' * {amount}); sys.stderr.write('y' * {amount})"]
        event, stdout, stderr = offline.run_command(command, "large-log", self.output, report, os.environ.copy(), 5)
        self.assertEqual(event["returncode"], 0)
        self.assertEqual(len(stdout), offline.LOG_PREVIEW_BYTES)
        self.assertEqual(len(stderr), offline.LOG_PREVIEW_BYTES)
        self.assertEqual((self.output / event["stdout"]).stat().st_size, amount)
        self.assertEqual((self.output / event["stderr"]).stat().st_size, amount)
        self.assertTrue(event["stdout_preview_truncated"])
        self.assertTrue(event["stderr_preview_truncated"])

    def test_failed_tree_termination_and_parent_reap_remain_explicit_and_bounded(self):
        self.output.mkdir()
        parent = Mock(pid=12345)
        parent.poll.return_value = None
        parent.wait.side_effect = subprocess.TimeoutExpired("synthetic parent", offline.PARENT_REAP_TIMEOUT)
        helper = Mock()
        helper.wait.side_effect = subprocess.TimeoutExpired("synthetic taskkill", offline.TREE_TERMINATION_TIMEOUT)
        with patch.object(offline.subprocess, "Popen", return_value=helper), \
                patch.object(offline.os, "killpg", create=True, side_effect=PermissionError("synthetic rejection")):
            result = offline.terminate_tree(parent, self.output, "failed-termination")
        self.assertFalse(result["parent_exited"])
        self.assertTrue(result["children_may_remain"])
        self.assertIn("parent_error", result)
        parent.wait.assert_called_once_with(timeout=offline.PARENT_REAP_TIMEOUT)
        if os.name == "nt":
            self.assertTrue(result["termination_helper_still_running"])
            self.assertEqual([call.kwargs["timeout"] for call in helper.wait.call_args_list],
                             [offline.TREE_TERMINATION_TIMEOUT, offline.PARENT_REAP_TIMEOUT])
        else:
            self.assertIn("synthetic rejection", result["error"])

    def test_non_metallib_and_header_only_successful_link_are_rejected(self):
        for mode in ("invalid-library", "header-only"):
            with self.subTest(mode=mode):
                self.output = self.directory / mode
                report = self.run_synthetic(mode)
                self.assertEqual(report["commands"][-1]["returncode"], 0)
                self.assertFalse(report["offline_compile_verified"])
                self.assertIn("Invalid metallib", report["error"])
                self.assertEqual(len(report["trials"][0]["artifacts"]), 2)

    def test_missing_or_empty_air_is_not_linked(self):
        for mode in ("missing-air", "empty-air"):
            with self.subTest(mode=mode):
                self.output = self.directory / mode
                report = self.run_synthetic(mode)
                self.assertFalse(report["offline_compile_verified"])
                self.assertEqual(report["commands"][-1]["label"], "compile-01")

    def test_failure_after_successful_repeat_does_not_verify_whole_run(self):
        report = self.run_synthetic("second-error", "--repeat", "3")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual([trial["status"] for trial in report["trials"]], ["passed", "failed"])
        self.assertTrue((self.output / "trial-01/fixture.metallib").is_file())
        self.assertFalse((self.output / "trial-03").exists())

    def test_unsupported_version_probe_is_explicit_and_not_a_compile_failure(self):
        report = self.run_synthetic("version-unsupported")
        self.assertTrue(report["offline_compile_verified"])
        self.assertTrue(all(not info["version_available"] for info in report["tools"].values()))
        self.assertTrue(all(info["version_returncode"] == 6 for info in report["tools"].values()))

    def test_missing_tools_keep_failure_report_without_running_any_process(self):
        (self.bin / "metallib.exe").unlink()
        with patch.object(offline.platform, "system", return_value="Windows"), \
                patch.object(offline.subprocess, "Popen") as run:
            report = offline.run_experiment(self.arguments())
        run.assert_not_called()
        self.assertFalse(report["offline_compile_verified"])
        self.assertIn("missing", report["error"])
        self.assertTrue((self.output / "result.json").is_file())

    def test_existing_output_is_refused_without_changing_it_or_running_tools(self):
        self.output.mkdir()
        sentinel = self.output / "keep.bin"
        sentinel.write_bytes(b"existing evidence")
        with patch.object(offline.subprocess, "Popen") as run:
            with self.assertRaises(FileExistsError):
                offline.run_experiment(self.arguments())
        run.assert_not_called()
        self.assertEqual(list(self.output.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"existing evidence")

    def test_environment_flags_are_removed_and_reported(self):
        with patch.dict(os.environ, {key: "unrelated injected option" for key in offline.ENV_OPTIONS}):
            report = self.run_synthetic()
        self.assertEqual(set(report["removed_environment_options"]), set(offline.ENV_OPTIONS))
        for _, options in self.calls:
            self.assertFalse(set(options["env"]) & set(offline.ENV_OPTIONS))

    def test_main_returns_failure_for_missing_tools(self):
        with patch.object(offline.platform, "system", return_value="Windows"), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = offline.main(["--output", str(self.output), "--tools", str(self.directory / "absent")])
        self.assertEqual(result, 1)
        self.assertFalse(json.loads((self.output / "result.json").read_text())["offline_compile_verified"])


    def test_shared_budget_covers_all_resolvers_probes_compile_and_link(self):
        clock, waits = [0.0], []
        sdk = self.directory / "synthetic SDK"
        sdk.mkdir()
        durations = iter((1, 1, 1, 1, 1, 2, 1))

        def child(command, **kwargs):
            if "--show-sdk-path" in command:
                message = str(sdk)
            elif "--find" in command:
                message = str(self.bin / (command[-1] + ".exe"))
            else:
                message = "SYNTHETIC BUDGET CONTROL; NOT APPLE METAL"
            kwargs["stdout"].write((message + "\n").encode())
            if "-o" in command:
                artifact = Path(command[command.index("-o") + 1])
                artifact.write_bytes(b"synthetic AIR" if "-c" in command else b"MTLB synthetic payload")
            process = Mock(pid=12345)

            def wait(timeout):
                waits.append(timeout)
                clock[0] += next(durations)
                return 0

            process.wait.side_effect = wait
            return process

        args = self.arguments("--budget", "8", "--timeout", "4", "--repeat", "2")
        args.tools = None
        with patch.object(offline.platform, "system", return_value="Darwin"), \
                patch.object(offline.shutil, "which", return_value="synthetic-xcrun"), \
                patch.object(offline.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(offline.subprocess, "Popen", side_effect=child) as spawn:
            report = offline.run_experiment(args)
        self.assertEqual(spawn.call_count, 7)
        self.assertEqual(waits, [4, 4, 4, 4, 4, 3, 1])
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertFalse(report["offline_compile_verified"])
        self.assertEqual([row["status"] for row in report["trials"]], ["passed", "failed"])
        self.assertEqual(len(report["trials"][0]["artifacts"]), 2)
        denied = report["commands"][-1]
        self.assertEqual(denied["label"], "compile-02")
        self.assertFalse(denied["process_started"])
        self.assertTrue(denied["budget_exhausted"])
        self.assertEqual(denied["timeout_seconds"], 0)
        self.assertEqual(report, json.loads((self.output / "result.json").read_text()))
        self.assertIn("SYNTHETIC", (self.output / report["commands"][-2]["stdout"]).read_text())

    def test_budget_exhausted_during_inputs_starts_no_tool_and_keeps_snapshot(self):
        clock = [0.0]
        original = offline.file_info

        def inspected(path):
            clock[0] = 2.0
            return original(path)

        with patch.object(offline.platform, "system", return_value="Windows"), \
                patch.object(offline.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(offline, "file_info", side_effect=inspected), \
                patch.object(offline.subprocess, "Popen") as spawn:
            report = offline.run_experiment(self.arguments("--budget", "1"))
        spawn.assert_not_called()
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual((self.output / "fixture.metal").read_bytes(), offline.SOURCE.read_bytes())
        self.assertFalse(report["commands"][0]["process_started"])

    def test_process_startup_consumes_budget_and_timeout_keeps_partial_logs(self):
        self.output.mkdir()
        report = {"commands": []}
        process = Mock(pid=12345)
        process.wait.side_effect = subprocess.TimeoutExpired("synthetic", 1)

        def child(command, **kwargs):
            kwargs["stdout"].write(b"synthetic partial output")
            return process

        with patch.object(offline.time, "monotonic", side_effect=[8.0, 9.0, 10.0]), \
                patch.object(offline.subprocess, "Popen", side_effect=child), \
                patch.object(offline, "terminate_tree", return_value={"synthetic_cleanup": True}) as cleanup:
            event, stdout, _ = offline.run_command(["synthetic"], "limited", self.output, report, {}, 30, deadline=10)
        process.wait.assert_called_once_with(timeout=1)
        cleanup.assert_called_once()
        self.assertEqual(event["timeout_cap_seconds"], 30)
        self.assertEqual(event["timeout_seconds"], 2)
        self.assertTrue(event["timed_out"])
        self.assertTrue(event["budget_exhausted"])
        self.assertIn("partial output", stdout)
        with self.assertRaises(offline.BudgetExhausted):
            offline.require_success(event)

    def test_inherited_test_deadline_never_extends_explicit_deadline(self):
        self.output.mkdir()
        for explicit, expected in ((None, 2), (12, 2), (9, 1)):
            with self.subTest(explicit=explicit):
                process = Mock(pid=12345)
                process.wait.return_value = 0
                report = {"commands": []}
                with patch.object(offline.time, "monotonic", return_value=8), \
                        patch.object(offline.subprocess, "Popen", return_value=process):
                    event, _, _ = offline.run_command(["synthetic"], "inherited", self.output, report,
                        {offline.TEST_DEADLINE_ENV: "10"}, 30, deadline=explicit)
                process.wait.assert_called_once_with(timeout=expected)
                self.assertTrue(event["test_deadline_inherited"])
        process = Mock(pid=12345)
        process.wait.return_value = 0
        with patch.object(offline.time, "monotonic", side_effect=AssertionError("unexpected deadline clock")), \
                patch.object(offline.subprocess, "Popen", return_value=process):
            offline.run_command(["synthetic"], "legacy", self.output, {"commands": []}, {}, 30)
        process.wait.assert_called_once_with(timeout=30)

    def test_invalid_or_expired_inherited_deadline_keeps_logs_without_launch(self):
        self.output.mkdir()
        for value in ("", "nan", "inf", "-inf", "0", "-1", "invalid", "1e999", "7"):
            with self.subTest(value=value):
                report = {"commands": []}
                with patch.object(offline.time, "monotonic", return_value=8), \
                        patch.object(offline.subprocess, "Popen") as spawn, self.assertRaises(offline.TrialError):
                    offline.run_command(["synthetic"], "invalid", self.output, report,
                        {offline.TEST_DEADLINE_ENV: value}, 30)
                spawn.assert_not_called()
                event = report["commands"][0]
                self.assertFalse(event["process_started"])
                self.assertTrue((self.output / event["stderr"]).is_file())
                self.assertTrue((self.output / "000-invalid.json").is_file())
                self.assertTrue(event["budget_exhausted"] if value == "7" else offline.TEST_DEADLINE_ENV in event["error"])

class MetalOfflineArgumentsTests(unittest.TestCase):
    def test_cli_language_standards_follow_actual_apple_compiler_spellings(self):
        # Accepted spellings from the installed Apple metal 32023.850 diagnostic.
        # Exercise CLI parsing through argv generation, including the platform
        # distinction that the initial real Windows smoke test exposed.
        standards = {
            "1.2": {"ios": "ios-metal1.2", "macos": "macos-metal1.2"},
            "2.4": {"ios": "ios-metal2.4", "macos": "macos-metal2.4"},
            "3.0": {"ios": "metal3.0", "macos": "metal3.0"},
            "3.1": {"ios": "metal3.1", "macos": "metal3.1"},
            "3.2": {"ios": "metal3.2", "macos": "metal3.2"},
            "4.0": {"ios": "metal4.0", "macos": "metal4.0"},
        }
        for language, targets in standards.items():
            for target, expected in targets.items():
                with self.subTest(language=language, target=target):
                    args = offline.parse_args(["--output", "fresh", "--target", target, "--language", language])
                    command = offline.compile_command("metal", "fixture.metal", "fixture.air", args.target,
                                                      args.deployment, args.language)
                    self.assertEqual([arg for arg in command if arg.startswith("-std=")], ["-std=" + expected])
                    self.assertIn("-DPROBE_BIAS=3", command)
                    self.assertIn("-fno-fast-math", command)

    def test_default_and_explicit_targets_are_not_inferred_from_host(self):
        ios = offline.parse_args(["--output", "fresh"])
        mac = offline.parse_args(["--output", "fresh", "--target", "macos"])
        self.assertEqual((ios.target, ios.deployment, ios.language, ios.repeat), ("ios", "18.0", "2.4", 3))
        self.assertEqual((mac.target, mac.deployment), ("macos", "15.0"))
        for target, deployment, triple in (("ios", "18.0", "air64-apple-ios18.0"),
                                           ("macos", "15.0", "air64-apple-macosx15.0")):
            command = offline.compile_command("/tool path/metal", "/source path/file.metal", "/out/file.air",
                                              target, deployment, "2.4", "/sdk with spaces")
            self.assertIn(triple, command)
            self.assertEqual(command[command.index("-isysroot") + 1], "/sdk with spaces")
            self.assertEqual(command[0], "/tool path/metal")
            self.assertIn("/source path/file.metal", command)
            self.assertNotIn("simulator", " ".join(command))

    def test_invalid_versions_unbounded_repeats_and_timeouts_are_rejected(self):
        bad_options = [("--repeat", "0"), ("--repeat", "21"), ("--timeout", "0"),
                       ("--timeout", "601"), ("--timeout", "nan"), ("--timeout", "inf"),
                       ("--language", "2.4.0"), ("--language", "2.4 -ffast-math"),
                       ("--deployment", "18.0;whoami"), ("--target", "simulator"),
                       *(("--budget", value) for value in ("0", "-1", "3601", "nan", "inf", "1e309"))]
        for option in bad_options:
            with self.subTest(option=option), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    offline.parse_args(["--output", "fresh", *option])
                self.assertEqual(error.exception.code, 2)
        self.assertIsNone(offline.parse_args(["--output", "fresh"]).budget)
        self.assertEqual(offline.parse_args(["--output", "fresh", "--budget", "3600"]).budget, 3600)

    def test_windows_search_is_limited_to_product_roots_and_absolute_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary).resolve()
            program_files = directory / "Program Files"
            known = program_files / "Metal Developer Tools/5.3/bin"
            unrelated = program_files / "UnrelatedProduct/bin"
            path_bin = directory / "path-bin"
            for candidate in (known, unrelated, path_bin):
                candidate.mkdir(parents=True)
            candidates = offline.windows_candidates({"ProgramFiles": str(program_files),
                                                     "PATH": f";relative;{path_bin}"})
            self.assertIn(known, candidates)
            self.assertIn(path_bin, candidates)
            self.assertNotIn(unrelated, candidates)
            self.assertTrue(all(path.is_absolute() for path in candidates))

    def test_mac_resolves_requested_sdk_and_both_tools_through_xcrun(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary).resolve()
            sdk = directory / "selected sdk"
            sdk.mkdir()
            for name in ("metal", "metallib"):
                (directory / name).write_bytes(b"synthetic executable identity")
            calls = []

            def query(command, *unused):
                calls.append(command)
                result = sdk if command[-1] == "--show-sdk-path" else directory / command[-1]
                return {"returncode": 0, "timed_out": False}, str(result) + "\n", ""

            for target, expected in (("ios", "iphoneos"), ("macos", "macosx")):
                with self.subTest(target=target), patch.object(offline.shutil, "which", return_value="/usr/bin/xcrun"), \
                        patch.object(offline, "run_command", side_effect=query):
                    calls.clear()
                    report = {}
                    tools, selected_sdk = offline.resolve_tools(None, target, "Darwin", directory,
                                                                report, {}, 1)
                    self.assertEqual(selected_sdk, sdk)
                    self.assertEqual(set(tools), {"metal", "metallib"})
                    self.assertEqual(calls, [["/usr/bin/xcrun", "--sdk", expected, "--show-sdk-path"],
                                             ["/usr/bin/xcrun", "--sdk", expected, "--find", "metal"],
                                             ["/usr/bin/xcrun", "--sdk", expected, "--find", "metallib"]])


if __name__ == "__main__":
    unittest.main()
