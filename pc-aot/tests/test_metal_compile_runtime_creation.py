"""Runtime process-ownership controls only; no Metal compiler or GPU calls."""
import importlib.util
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_runtime_creation", ROOT / "experiments/metal-compile/runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class RuntimeCreationFailureTests(unittest.TestCase):
    def test_creation_failure_restores_handler_and_propagates_original_error(self):
        original_handler = signal.getsignal(signal.SIGINT)
        error = OSError("authored process creation failure")
        report = {"commands": []}
        with tempfile.TemporaryDirectory() as value, \
                patch.object(runtime.subprocess, "Popen", side_effect=error), \
                patch.object(runtime, "stop_process") as stop:
            with self.assertRaises(OSError) as caught:
                runtime.run_command(["not-launched"], "creation-failure", Path(value), report,
                                    os.environ.copy(), 5)
        self.assertIs(caught.exception, error)
        self.assertEqual(signal.getsignal(signal.SIGINT), original_handler)
        stop.assert_not_called()
        self.assertEqual(report["commands"][0]["status"], "launch_failed")
        self.assertFalse(report["commands"][0]["process_started"])


@unittest.skipUnless(os.name == "posix", "real process-group signal control requires POSIX")
class RuntimeCreationOwnershipTests(unittest.TestCase):
    def interrupted_creation(self, handler, expected_exception):
        output = ROOT / "local/tests" / f"runtime-creation-interrupt-{time.time_ns()}-{os.getpid()}"
        output.mkdir(parents=True, exist_ok=False)
        report = {"commands": []}
        original_handler = signal.getsignal(signal.SIGINT)
        original_spawn = runtime.subprocess.Popen
        children = []

        def spawn_then_signal(*args, **kwargs):
            child = original_spawn(*args, **kwargs)
            children.append(child)
            # A real child exists, but the caller has not received its Popen object.
            signal.raise_signal(signal.SIGINT)
            return child

        try:
            signal.signal(signal.SIGINT, handler)
            with patch.object(runtime.subprocess, "Popen", side_effect=spawn_then_signal), \
                    patch.object(runtime, "stop_process", wraps=runtime.stop_process) as stop:
                with self.assertRaises(expected_exception):
                    runtime.run_command([sys.executable, "-c", "import time; time.sleep(60)"],
                                        "creation-interrupt", output, report, os.environ.copy(), 5)
            self.assertEqual(signal.getsignal(signal.SIGINT), handler)
            self.assertEqual(len(children), 1)
            self.assertEqual(children[0].returncode, -signal.SIGKILL)
            self.assertEqual(stop.call_count, 1)
            event = report["commands"][0]
            expected_status = "interrupted" if issubclass(expected_exception, (KeyboardInterrupt, SystemExit)) else "process_error"
            self.assertEqual(event["status"], expected_status)
            self.assertTrue(event["process_started"])
            self.assertEqual(event["returncode"], -signal.SIGKILL)
            self.assertTrue((output / event["stdout"]).is_file())
            self.assertTrue((output / event["stderr"]).is_file())
        finally:
            signal.signal(signal.SIGINT, original_handler)
            for child in children:
                if child.returncode is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=5)

    def test_sigint_before_assignment_reaps_owned_child(self):
        self.interrupted_creation(signal.default_int_handler, KeyboardInterrupt)

    def test_system_exit_handler_keeps_original_behavior_after_cleanup(self):
        def exit_handler(signum, frame):
            raise SystemExit("authored handler exit")
        self.interrupted_creation(exit_handler, SystemExit)

    def test_custom_error_handler_keeps_original_error_after_cleanup(self):
        def error_handler(signum, frame):
            raise ValueError("authored handler error")
        self.interrupted_creation(error_handler, ValueError)


if __name__ == "__main__":
    unittest.main()
