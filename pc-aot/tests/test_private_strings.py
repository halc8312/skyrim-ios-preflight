"""Game-free lifecycle/path regression tests for the private diagnostic wrapper.

Only standard-library Python helpers run. No candidate, vendor source, packages,
compiler, lifted body, or game executable is required or invoked.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "private_strings_under_test", PROJECT / "scripts/check_private_strings.py")
diagnostic = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(diagnostic)


def process_role(role, work):
    """Three owned processes exercise the real nested command cleanup path."""
    work = Path(work)
    if role == "leaf":
        (work / "leaf.pid").write_text(str(os.getpid()))
        print("leaf evidence", flush=True)
        time.sleep(30)
        return 0
    if role not in ("parent", "worker"):
        raise ValueError("unknown synthetic process role")
    try:
        with diagnostic.scoped_interrupts():
            diagnostic.command(
                [sys.executable, "-B", __file__, "--process-role",
                 "leaf" if role == "worker" else "worker", str(work)],
                work, "nested" if role == "worker" else "oracle-adapter",
                seconds=20, memory=128)
    except diagnostic.DiagnosticInterrupted:
        (work / (role + ".interrupted")).write_text("retained interruption evidence")
        return 130
    return 0


class PrivateStringPaths(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix="private-string-paths-")
        self.addCleanup(self.fixture.cleanup)
        self.root = Path(self.fixture.name).resolve()
        (self.root / "private").mkdir()
        patch = mock.patch.object(diagnostic, "ROOT", self.root)
        patch.start()
        self.addCleanup(patch.stop)

    def test_owned_private_output_is_allowed(self):
        target = self.root / "private" / "not-yet-created" / "output"
        self.assertEqual(diagnostic.private_output(target), target)
        self.assertFalse(target.exists(), "Validation should not create files")

    def test_lexical_escape_is_rejected(self):
        for target in (self.root / "outside", self.root / "private" / ".." / "outside"):
            with self.subTest(path=target), self.assertRaisesRegex(RuntimeError, "outside"):
                diagnostic.private_output(target)

    def test_symlink_escape_is_rejected(self):
        outside = self.root / "outside"
        outside.mkdir()
        link = self.root / "private" / "escape"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Directory symlinks unavailable on this host: {error}")
        with self.assertRaisesRegex(RuntimeError, "outside"):
            diagnostic.private_output(link / "payload")
        self.assertEqual(list(outside.iterdir()), [])


class PrivateStringSignals(unittest.TestCase):
    def test_handlers_restored_after_normal_exit(self):
        before = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
        with diagnostic.scoped_interrupts():
            if os.name == "posix":
                self.assertTrue(all(callable(signal.getsignal(sig)) for sig in before))
        self.assertEqual({sig: signal.getsignal(sig) for sig in before}, before)

    def test_handlers_restored_after_exception(self):
        before = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
        with self.assertRaisesRegex(RuntimeError, "synthetic"):
            with diagnostic.scoped_interrupts():
                raise RuntimeError("synthetic scope failure")
        self.assertEqual({sig: signal.getsignal(sig) for sig in before}, before)

    def check_nested_cancellation(self, interruption):
        with tempfile.TemporaryDirectory(prefix="private-string-lifecycle-") as folder:
            work = Path(folder)
            descendants = []
            with (work / "parent.stdout").open("wb") as out, (work / "parent.stderr").open("wb") as err:
                parent = subprocess.Popen(
                    [sys.executable, "-B", __file__, "--process-role", "parent", str(work)],
                    stdout=out, stderr=err, start_new_session=True)
                try:
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        try:
                            leaf = int((work / "leaf.pid").read_text())
                            worker = json.loads((work / "oracle-adapter.command.json").read_text())["spawned_pid"]
                            inner = json.loads((work / "nested.command.json").read_text())["spawned_pid"]
                            if inner == leaf:
                                descendants = [worker, leaf]
                                break
                        except (FileNotFoundError, ValueError, KeyError):
                            pass
                        if parent.poll() is not None:
                            self.fail("Synthetic parent exited before descendants started: " + (work / "parent.stderr").read_text())
                        time.sleep(0.05)
                    self.assertEqual(len(descendants), 2, "Owned descendants did not become ready")
                    os.kill(parent.pid, interruption)
                    self.assertEqual(parent.wait(timeout=15), 130)
                    for pid in descendants:
                        with self.subTest(pid=pid), self.assertRaises(ProcessLookupError):
                            os.kill(pid, 0)
                    for role in ("parent", "worker"):
                        self.assertEqual((work / (role + ".interrupted")).read_text(), "retained interruption evidence")
                    for name in ("nested", "oracle-adapter"):
                        record = json.loads((work / (name + ".command.json")).read_text())
                        self.assertEqual(record["status"], "interrupted_or_deadline")
                        self.assertGreater(record["spawned_pid"], 0)
                        self.assertTrue((work / (name + ".stdout")).is_file())
                        self.assertTrue((work / (name + ".stderr")).is_file())
                    self.assertIn("leaf evidence", (work / "nested.stdout").read_text())
                finally:
                    # Failure cleanup is limited to sessions created by this fixture.
                    for name in ("oracle-adapter", "nested"):
                        try:
                            descendants.append(json.loads((work / (name + ".command.json")).read_text())["spawned_pid"])
                        except (FileNotFoundError, ValueError, KeyError):
                            pass
                    for pid in set(descendants + [parent.pid]):
                        try:
                            if os.getpgid(pid) == pid:
                                os.killpg(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    if parent.poll() is None:
                        parent.wait(timeout=5)

    @unittest.skipUnless(os.name == "posix", "POSIX sessions, signals and resource limits required")
    def test_sigint_reaps_nested_children_and_retains_evidence(self):
        self.check_nested_cancellation(signal.SIGINT)

    @unittest.skipUnless(os.name == "posix", "POSIX sessions, signals and resource limits required")
    def test_sigterm_reaps_nested_children_and_retains_evidence(self):
        self.check_nested_cancellation(signal.SIGTERM)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--process-role":
        raise SystemExit(process_role(sys.argv[2], sys.argv[3]))
    unittest.main()
