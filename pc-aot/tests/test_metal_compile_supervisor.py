"""Authored POSIX supervision controls only; no Metal or game inputs."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_test_supervisor", ROOT / "scripts/supervise-metal-tests.py")
supervisor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(supervisor)


class MetalSupervisorBudgetTests(unittest.TestCase):
    def test_shared_deadline_reserves_cleanup_and_never_extends_inherited_budget(self):
        self.assertEqual(supervisor.tool_deadline(100, 60, 10), 150)
        self.assertEqual(supervisor.tool_deadline(100, 60, 10, "125"), 125)
        self.assertEqual(supervisor.tool_deadline(100, 60, 10, "170"), 150)
        for value in ("nan", "inf", "0", "-1", "bad"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                supervisor.tool_deadline(100, 60, 10, value)

    def test_reaped_worker_pid_is_never_signalled(self):
        process = Mock(pid=987654, returncode=0)
        report = {}
        with patch.object(supervisor.os, "killpg", create=True) as kill:
            supervisor.stop_worker(process, report, 0.1, 0.1)
        kill.assert_not_called()
        process.wait.assert_not_called()
        self.assertTrue(report["cleanup"]["worker_reaped"])
        self.assertFalse(report["cleanup"]["nested_process_groups_termination_verified"])


@unittest.skipUnless(os.name == "posix", "real process-group controls require POSIX; no Metal validation")
class MetalSupervisorProcessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("SKYRIM_METAL_SUPERVISOR_TEST_OUTPUT")
        cls.output = Path(value).resolve() if value else ROOT / "local/tests" / f"metal-supervisor-{time.time_ns()}-{os.getpid()}"
        cls.output.mkdir(parents=True, exist_ok=False)

    def prepare(self, code):
        directory = self.output / self._testMethodName
        directory.mkdir()
        child = directory / "authored-child.py"
        child.write_text(code, encoding="utf-8")
        return [sys.executable, "-u", str(child)], directory / "supervision"

    def run_child(self, code, *, budget=4, reserve=0.25, grace=0.5):
        command, output = self.prepare(code)
        report = supervisor.run_supervised(command, output, budget, cleanup_reserve=reserve,
                                           interrupt_grace=grace, reap_grace=1)
        self.assertEqual(report, json.loads((output / "result.json").read_text()))
        return report, output

    def test_success_keeps_streams_and_propagates_shared_deadline(self):
        report, output = self.run_child("import os, sys\nprint(os.environ['SKYRIM_METAL_TEST_DEADLINE'])\nprint('authored stderr', file=sys.stderr)\n")
        self.assertEqual(report["status"], "passed")
        self.assertEqual(float((output / report["stdout"]).read_text()), report["shared_tool_deadline"])
        self.assertEqual((output / report["stderr"]).read_text().strip(), "authored stderr")
        original = (output / "result.json").read_bytes()
        with self.assertRaises(FileExistsError):
            supervisor.run_supervised(["not launched"], output, 3)
        self.assertEqual(original, (output / "result.json").read_bytes())

    def test_child_failure_is_preserved_as_failure(self):
        report, output = self.run_child("import sys\nprint('before failure', flush=True)\nsys.exit(23)\n")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["worker_returncode"], 23)
        self.assertIn("before failure", (output / report["stdout"]).read_text())

    def test_sigint_allows_worker_to_reap_its_separate_session_child(self):
        code = """import os, signal, subprocess, sys, time
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
def stop(signum, frame):
    os.killpg(child.pid, signal.SIGKILL)
    child.wait(timeout=1)
    print('INNER_REAPED', child.returncode, flush=True)
    raise SystemExit(0)
signal.signal(signal.SIGINT, stop)
print('READY', flush=True)
while True:
    time.sleep(1)
"""
        report, output = self.run_child(code, budget=1.5, grace=1)
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual(report["worker_returncode"], 0)
        self.assertTrue(report["cleanup"]["sigint_sent"])
        self.assertFalse(report["cleanup"]["sigkill_sent"])
        self.assertIn("INNER_REAPED", (output / report["stdout"]).read_text())
        # This one authored child acknowledged its cleanup. The supervisor still
        # makes no blanket claim about every external or nested process group.
        self.assertFalse(report["cleanup"]["nested_process_groups_termination_verified"])

    def test_uncooperative_worker_gets_bounded_owned_group_kill_and_explicit_residual_limit(self):
        code = "import signal, time\nsignal.signal(signal.SIGINT, signal.SIG_IGN)\nprint('READY', flush=True)\nwhile True: time.sleep(1)\n"
        report, output = self.run_child(code, budget=1.5, grace=0.2)
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertEqual(report["worker_returncode"], -signal.SIGKILL)
        self.assertTrue(report["cleanup"]["sigint_sent"])
        self.assertTrue(report["cleanup"]["sigkill_sent"])
        self.assertTrue(report["cleanup"]["worker_reaped"])
        self.assertTrue(report["cleanup"]["nested_process_groups_may_remain"])
        self.assertIn("READY", (output / report["stdout"]).read_text())
        self.assertLess(report["elapsed_seconds"], 5)

    def test_expired_admission_never_starts_a_worker(self):
        command, output = self.prepare("raise SystemExit('must not start')\n")
        with patch.object(supervisor.time, "monotonic", side_effect=[100, 104, 105]), \
                patch.object(supervisor.subprocess, "Popen") as spawn:
            report = supervisor.run_supervised(command, output, 3, env={}, cleanup_reserve=0.5)
        spawn.assert_not_called()
        self.assertEqual(report["status"], "budget_exhausted")
        self.assertFalse(report["process_started"])
        self.assertTrue((output / "result.json").is_file())

    def test_manual_interrupt_retains_report_and_only_signals_owned_worker_group(self):
        command, output = self.prepare("# mocked worker, never executed\n")
        process = Mock(pid=123456, returncode=None)
        calls = 0

        def wait(timeout):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise KeyboardInterrupt
            process.returncode = -signal.SIGINT
            return process.returncode

        process.wait.side_effect = wait
        with patch.object(supervisor.subprocess, "Popen", return_value=process), \
                patch.object(supervisor.os, "killpg") as kill:
            with self.assertRaises(KeyboardInterrupt):
                supervisor.run_supervised(command, output, 4, env={}, cleanup_reserve=0.5)
        kill.assert_called_once_with(123456, signal.SIGINT)
        report = json.loads((output / "result.json").read_text())
        self.assertEqual(report["status"], "interrupted")
        self.assertTrue(report["cleanup"]["worker_reaped"])

    def test_sigint_inside_real_popen_before_assignment_reaps_the_owned_worker(self):
        code = """from pathlib import Path
import signal, sys, time
def stop(signum, frame):
    print('OWNED_CHILD_STOPPED', flush=True)
    raise SystemExit(0)
signal.signal(signal.SIGINT, stop)
Path(__file__).with_suffix('.ready').write_text('ready')
while True:
    time.sleep(0.05)
"""
        command, output = self.prepare(code)
        ready = Path(command[-1]).with_suffix(".ready")
        real_popen = supervisor.subprocess.Popen
        real_killpg = os.killpg
        owned = []
        factory_returned = []
        previous_handler = signal.getsignal(signal.SIGINT)

        def interrupted_factory(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            owned.append(process)
            deadline = time.monotonic() + 3
            while not ready.is_file() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.is_file(), "authored child did not establish its SIGINT handler")
            # This signal arrives while Popen's result has not yet been
            # assigned to run_supervised's local process variable.
            os.kill(os.getpid(), signal.SIGINT)
            factory_returned.append(True)
            return process

        try:
            signal.signal(signal.SIGINT, signal.default_int_handler)
            with patch.object(supervisor.subprocess, "Popen", side_effect=interrupted_factory), \
                    patch.object(supervisor.os, "killpg", wraps=real_killpg) as kill:
                with self.assertRaises(KeyboardInterrupt):
                    supervisor.run_supervised(command, output, 8, cleanup_reserve=0.5,
                                               interrupt_grace=1, reap_grace=1)
            self.assertEqual(factory_returned, [True])
            self.assertIs(signal.getsignal(signal.SIGINT), signal.default_int_handler)
            self.assertEqual(len(owned), 1)
            process = owned[0]
            kill.assert_called_once_with(process.pid, signal.SIGINT)
            report = json.loads((output / "result.json").read_text())
            self.assertEqual(report["status"], "interrupted")
            self.assertTrue(report["process_started"])
            self.assertEqual(report["worker_pid"], process.pid)
            self.assertEqual(report["worker_pgid"], process.pid)
            self.assertTrue(report["cleanup"]["worker_reaped"])
            self.assertFalse(report["cleanup"]["sigkill_sent"])
            self.assertEqual(process.returncode, 0)
            self.assertIn("OWNED_CHILD_STOPPED", (output / report["stdout"]).read_text())
            self.assertFalse(report["cleanup"]["nested_process_groups_termination_verified"])
        finally:
            signal.signal(signal.SIGINT, previous_handler)
            # Retain the failure evidence but do not leak this authored child
            # if the regression reappears before supervisor ownership is set.
            for process in owned:
                if process.returncode is None:
                    try:
                        real_killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=2)


if __name__ == "__main__":
    unittest.main()
