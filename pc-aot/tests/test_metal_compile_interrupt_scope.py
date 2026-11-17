"""SIGINT ownership controls; no Metal tools or game inputs are executed."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_interrupt_control", ROOT / "experiments/metal-compile/offline.py")
offline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(offline)


class InterruptScopeTests(unittest.TestCase):
    def test_spawn_error_restores_original_handler(self):
        previous = signal.getsignal(signal.SIGINT)
        with self.assertRaisesRegex(OSError, "authored spawn failure"):
            with offline.defer_sigint_during_creation():
                raise OSError("authored spawn failure")
        self.assertEqual(signal.getsignal(signal.SIGINT), previous)

    def test_custom_handler_is_delivered_after_ownership_and_restored(self):
        previous = signal.getsignal(signal.SIGINT)
        observed, owned = [], False
        def custom(signum, frame):
            observed.append((signum, owned, signal.getsignal(signal.SIGINT)))
        try:
            signal.signal(signal.SIGINT, custom)
            with offline.defer_sigint_during_creation():
                signal.raise_signal(signal.SIGINT)
                self.assertEqual(observed, [])
                owned = True
            self.assertEqual(observed, [(signal.SIGINT, True, custom)])
            self.assertEqual(signal.getsignal(signal.SIGINT), custom)
        finally:
            signal.signal(signal.SIGINT, previous)

    def test_ignored_handler_remains_ignored(self):
        previous = signal.getsignal(signal.SIGINT)
        try:
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            with offline.defer_sigint_during_creation():
                self.assertEqual(signal.getsignal(signal.SIGINT), signal.SIG_IGN)
                signal.raise_signal(signal.SIGINT)
            self.assertEqual(signal.getsignal(signal.SIGINT), signal.SIG_IGN)
        finally:
            signal.signal(signal.SIGINT, previous)

    def test_os_default_handler_is_not_replaced(self):
        previous = signal.getsignal(signal.SIGINT)
        try:
            signal.signal(signal.SIGINT, signal.SIG_DFL)
            with offline.defer_sigint_during_creation():
                self.assertEqual(signal.getsignal(signal.SIGINT), signal.SIG_DFL)
            self.assertEqual(signal.getsignal(signal.SIGINT), signal.SIG_DFL)
        finally:
            signal.signal(signal.SIGINT, previous)

    def test_nonmain_thread_never_changes_signal_handler(self):
        completed = []
        def worker():
            with offline.defer_sigint_during_creation():
                completed.append(True)
        with patch.object(offline.signal, "signal") as setter:
            thread = threading.Thread(target=worker)
            thread.start()
            thread.join(timeout=2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(completed, [True])
        setter.assert_not_called()


@unittest.skipUnless(os.name == "posix", "real owned-process group cleanup requires POSIX")
class InterruptOwnershipTests(unittest.TestCase):
    def run_interrupt(self, handler=None, expected_exception=KeyboardInterrupt):
        output = ROOT / "local/tests" / f"metal-creation-interrupt-{time.time_ns()}-{os.getpid()}"
        output.mkdir(parents=True, exist_ok=False)
        report = {"commands": []}
        original_spawn = offline.subprocess.Popen
        previous = signal.getsignal(signal.SIGINT)
        children = []

        def interrupt_before_return(*args, **kwargs):
            child = original_spawn(*args, **kwargs)
            children.append(child)
            signal.raise_signal(signal.SIGINT)
            return child

        try:
            if handler is not None:
                signal.signal(signal.SIGINT, handler)
            expected_handler = signal.getsignal(signal.SIGINT)
            with patch.object(offline.subprocess, "Popen", side_effect=interrupt_before_return):
                with self.assertRaises(expected_exception):
                    offline.run_command([sys.executable, "-c", "import time; time.sleep(60)"],
                                        "creation-interrupt", output, report, os.environ.copy(), 5)
            self.assertEqual(signal.getsignal(signal.SIGINT), expected_handler)
            self.assertEqual(len(children), 1)
            self.assertEqual(children[0].returncode, -signal.SIGKILL)
            event = json.loads((output / "000-creation-interrupt.json").read_text())
            self.assertEqual(event["interrupted"], issubclass(expected_exception, (KeyboardInterrupt, SystemExit)))
            self.assertTrue(event["process_started"])
            self.assertTrue(event["termination"]["parent_exited"])
        finally:
            signal.signal(signal.SIGINT, previous)
            for child in children:
                if child.returncode is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=5)

    def test_signal_before_popen_returns_is_delayed_until_owned_child_cleanup(self):
        self.run_interrupt()

    def test_custom_system_exit_handler_still_reaps_owned_child_and_reraises(self):
        def exit_handler(signum, frame):
            raise SystemExit("authored handler exit")
        self.run_interrupt(exit_handler, SystemExit)

    def test_custom_error_handler_still_reaps_owned_child_and_reraises(self):
        def error_handler(signum, frame):
            raise ValueError("authored handler error")
        self.run_interrupt(error_handler, ValueError)


if __name__ == "__main__":
    unittest.main()
