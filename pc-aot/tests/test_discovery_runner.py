"""Synthetic-only discovery harness checks; never invoke the game translator."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("discovery_runner", PROJECT / "scripts/run_discovery.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def synthetic_pe(path):
    data = bytearray(160)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, 0x14C)
    struct.pack_into("<H", data, 88, 0x10B)
    path.write_bytes(data)


class DiscoveryRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.root_patch = patch.object(runner, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def test_output_scope_and_existing_run_are_never_overwritten(self):
        with self.assertRaises(ValueError):
            runner.new_run(self.root / "docs/run")
        run = runner.new_run(self.root / "private/run")
        (run / "evidence.txt").write_text("retain")
        with self.assertRaises(FileExistsError):
            runner.new_run(run)
        self.assertEqual((run / "evidence.txt").read_text(), "retain")

    def test_private_symlink_cannot_escape_workspace(self):
        outside = self.root / "outside"
        outside.mkdir()
        try:
            (self.root / "private").symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(str(error))
        with self.assertRaises(ValueError):
            runner.new_run(self.root / "private/run")

    def test_candidate_snapshot_hash_and_architecture_are_required(self):
        candidate = self.root / "candidate.exe"
        synthetic_pe(candidate)
        original = candidate.read_bytes()
        with self.assertRaises(ValueError):
            runner.snapshot_input(candidate, self.root / "mismatch.exe", "0" * 64)
        self.assertEqual(candidate.read_bytes(), original)
        self.assertEqual((self.root / "mismatch.exe").read_bytes(), original)
        result = runner.snapshot_input(candidate, self.root / "matched.exe", runner.sha256(candidate))
        self.assertFalse(result["version_identity_verified"])
        data = bytearray(original)
        struct.pack_into("<H", data, 68, 0x8664)
        candidate.write_bytes(data)
        with self.assertRaises(ValueError):
            runner.snapshot_input(candidate, self.root / "wrong-arch.exe", runner.sha256(candidate))

    def test_distribution_metadata_and_installed_bytes_are_recorded(self):
        package = self.root / "installed.py"
        package.write_text("__version__ = 'stale-not-authoritative'\n")
        def distribution(name):
            return SimpleNamespace(version=runner.REQUIREMENTS[name], files=["installed.py"],
                                   locate_file=lambda path: self.root / path)
        with patch.object(runner.importlib.metadata, "distribution", side_effect=distribution):
            evidence = runner.package_evidence()
        self.assertEqual(evidence["capstone"]["metadata_version"], "5.0.9")
        self.assertEqual(evidence["pefile"]["files"][0]["sha256"], runner.sha256(package))
        with patch.object(runner.importlib.metadata, "distribution", return_value=SimpleNamespace(version="0")):
            with self.assertRaises(ValueError):
                runner.package_evidence()

    def test_missing_truncated_and_wrong_schema_are_distinct(self):
        path = self.root / "functions.json"
        self.assertEqual(runner.inspect_output(path, "functions")["state"], "missing")
        path.write_text('{"functions":')
        self.assertEqual(runner.inspect_output(path, "functions")["state"], "truncated_or_invalid_json")
        path.write_text('{}')
        self.assertEqual(runner.inspect_output(path, "functions")["state"], "invalid_schema")

    @unittest.skipUnless(shutil.which("git"), "Git required")
    def test_archive_uses_pinned_bytes_and_excludes_lifting_code(self):
        repo = self.root / "repo"
        (repo / "tools").mkdir(parents=True)
        def git(*args):
            return subprocess.run(["git", "-C", str(repo), *args], check=True,
                                  capture_output=True, text=True).stdout.strip()
        git("init", "-q")
        git("config", "core.autocrlf", "false")
        for name in (*runner.SOURCE_FILES, "tools/lift.py"):
            (repo / name).write_bytes(b"# committed synthetic source\n")
        git("add", ".")
        git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false", "commit", "-qm", "Fixture")
        revision = git("rev-parse", "HEAD")
        git("config", "core.autocrlf", "true")
        (repo / "tools/discover.py").write_text("# changed worktree, must not be copied\n")
        run = runner.new_run(self.root / "local/archive")
        with patch.object(runner, "PIN", revision):
            result = runner.materialize_source(shutil.which("git"), repo, run)
        self.assertEqual((run / "source/tools/discover.py").read_bytes(), b"# committed synthetic source\n")
        self.assertEqual((run / "source/tools/peimg.py").read_bytes(), b"# committed synthetic source\n")
        self.assertEqual(git("config", "core.autocrlf"), "true")
        self.assertFalse((run / "source/tools/lift.py").exists())
        self.assertEqual(result["archive_sha256"], runner.sha256(run / "source.tar"))

    def prepare_fake_run(self):
        (self.root / "scripts").mkdir()
        for name in ("run_discovery.py", "run-bounded.py", "prepare_translation.py", "steam_inputs.py"):
            shutil.copyfile(PROJECT / "scripts" / name, self.root / "scripts" / name)
        source = {"name": "skydream", "path": "vendor/skydream", "revision": runner.PIN,
                  "python_requirements": runner.REQUIREMENTS}
        (self.root / "source-lock.json").write_text(json.dumps({"sources": [source]}))
        candidate = self.root / "candidate.exe"
        synthetic_pe(candidate)
        return candidate

    @unittest.skipUnless(os.name == "posix", "Owned process groups require POSIX")
    def test_completed_failed_and_timeout_outputs_remain_distinct(self):
        candidate = self.prepare_fake_run()
        graph = {"image_base": 4194304, "entry": 4198400, "exports": {},
                 "functions": {"0x401000": [[4198400, 4198401]]}, "jump_tables": {}, "indirect_jumps": [], "iat": {}}
        for mode in ("complete", "partial-success", "timeout", "nonzero"):
            def materialize(_git, _repository, run):
                target = run / "source/tools"
                target.mkdir(parents=True)
                script = "import json,pathlib,sys,time\np=pathlib.Path(sys.argv[2])\nprint('synthetic discovery',flush=True)\n"
                script += f"p.write_text({json.dumps(graph)!r})\n" if mode in ("complete", "nonzero") else "p.write_text('{\\\"functions\\\":')\n"
                script += "pathlib.Path(str(p)+'.gaps').write_text('[]')\n"
                script += "time.sleep(30)\n" if mode == "timeout" else "raise SystemExit(65)\n" if mode == "nonzero" else ""
                (target / "discover.py").write_text(script)
                return {"synthetic_test_fixture": True}
            with self.subTest(mode=mode), patch.object(runner, "translator_state", return_value={"matches": True, "git": {"executable": "git"}}), patch.object(runner, "package_evidence", return_value={"synthetic_test_fixture": True}), patch.object(runner, "materialize_source", side_effect=materialize):
                code, manifest = runner.run_discovery(candidate, runner.sha256(candidate), self.root / "private" / mode, 1 if mode == "timeout" else 10)
            result = json.loads(manifest.read_text())
            expected = {"complete": (0, "completed"), "partial-success": (3, "failed"), "timeout": (124, "timed_out"), "nonzero": (65, "failed")}[mode]
            self.assertEqual((code, result["status"]), expected)
            self.assertEqual(result["workers"], 2)
            self.assertFalse(result["profile_approved"])
            self.assertFalse(result["generation_authorized"])
            self.assertFalse(result["memory_bounded"])
            self.assertEqual(result["candidate"]["original_sha256_after"], runner.sha256(candidate))
            self.assertTrue((manifest.parent / "discovery.log").is_file())
            if mode in ("partial-success", "timeout"):
                self.assertEqual(result["outputs"]["functions"]["state"], "truncated_or_invalid_json")

    @unittest.skipUnless(os.name == "posix", "Production preflight requires POSIX")
    def test_dirty_source_blocks_before_candidate_copy_or_execution(self):
        candidate = self.prepare_fake_run()
        with patch.object(runner, "translator_state", return_value={"matches": False}), patch.object(runner, "execute_owned") as execute:
            code, manifest = runner.run_discovery(candidate, runner.sha256(candidate), self.root / "private/dirty", 10)
        result = json.loads(manifest.read_text())
        self.assertEqual(code, 2)
        self.assertEqual(result["status"], "blocked")
        self.assertFalse(result["discovery_started"])
        execute.assert_not_called()
        lock = json.loads((self.root / "source-lock.json").read_text())
        lock["sources"][0]["revision"] = "0" * 40
        (self.root / "source-lock.json").write_text(json.dumps(lock))
        with patch.object(runner, "translator_state") as state, patch.object(runner, "execute_owned") as execute:
            code, manifest = runner.run_discovery(candidate, runner.sha256(candidate), self.root / "private/wrong-pin", 10)
        self.assertEqual(code, 2)
        self.assertEqual(json.loads(manifest.read_text())["status"], "blocked")
        state.assert_not_called()
        execute.assert_not_called()

    @unittest.skipUnless(os.name == "posix", "Signal forwarding requires POSIX")
    def test_cancellation_reaches_owned_deadline_helper(self):
        log = self.root / "owned.log"
        payload = "import os,time; print(os.getpid(),flush=True); time.sleep(30)"
        command = [sys.executable, str(PROJECT / "scripts/run-bounded.py"), "20", sys.executable, "-u", "-c", payload]
        helper = ("import os,sys; sys.path.insert(0," + repr(str(PROJECT / "scripts")) + "); "
                  "import run_discovery as r; "
                  "log=open(" + repr(str(log)) + ",'wb'); "
                  "code=r.execute_owned(" + repr(command) + "," + repr(str(self.root)) + ",os.environ.copy(),log); "
                  "log.close(); sys.exit(code)")
        process = subprocess.Popen([sys.executable, "-B", "-c", helper])
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and (not log.exists() or not log.read_text().strip()):
                time.sleep(0.05)
            child_pid = int(log.read_text().splitlines()[0])
            process.send_signal(signal.SIGTERM)
            self.assertEqual(process.wait(timeout=15), 143)
            with self.assertRaises(ProcessLookupError):
                os.kill(child_pid, 0)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)


if __name__ == "__main__":
    unittest.main()
