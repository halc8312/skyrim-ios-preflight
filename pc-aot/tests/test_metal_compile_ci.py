"""Execute the real CI shell helpers with authored fake children, never Metal.

The fake xcrun verifies working-directory/path handling only. Its output is not
AIR and cannot establish Apple SDK, compiler, GPU, or iPhone compatibility.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/check-metal-compile-mac.sh"
BASH = shutil.which("bash")


def shell_function(script, name):
    """Take the actual top-level function verbatim, including its Python body."""
    start = script.index(name + "() {\n")
    captured, heredoc = [], None
    for line in script[start:].splitlines(keepends=True):
        captured.append(line)
        if heredoc:
            if line.rstrip("\r\n") == heredoc:
                heredoc = None
            continue
        delimiter = re.search(r"<<\s*['\"]?([A-Za-z_]\w*)['\"]?", line)
        if delimiter:
            heredoc = delimiter.group(1)
        elif line.rstrip("\r\n") == "}":
            return "".join(captured)
    raise ValueError("CI helper closing line missing: " + name)


@unittest.skipUnless(os.name == "posix" and BASH, "actual CI bash functions require a POSIX host; not Metal evidence")
class MetalCiShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        explicit = os.environ.get("SKYRIM_METAL_CI_OUTPUT")
        request_output = os.environ.get("SKYRIM_METAL_REQUEST_OUTPUT")
        cls.output = Path(explicit).resolve() if explicit else \
            Path(request_output).resolve().parent / "ci-shell" if request_output else \
            ROOT / "local/tests" / f"metal-ci-{time.time_ns()}-{os.getpid()}"
        cls.output.mkdir(parents=True, exist_ok=False)
        script = SCRIPT.read_text(encoding="utf-8")
        (cls.output / "tested-check-metal-compile-mac.sh").write_text(script, encoding="utf-8")
        cls.helpers = "\n".join(shell_function(script, name) for name in (
            "run_bounded", "compile_probe", "remaining_budget", "run_experiment", "write_unit_summary"))
        cls.source_sha256 = hashlib.sha256(script.encode("utf-8")).hexdigest()

    def run_shell(self, name, body, *, fake=None, budget=15, workflow_budget=30):
        output = self.output / name
        output.mkdir()
        fakebin = output / "fakebin"
        fakebin.mkdir()
        if fake is not None:
            executable = fakebin / "xcrun"
            executable.write_text("#!/usr/bin/env python3\n" + fake, encoding="utf-8")
            executable.chmod(0o700)
        harness = output / "harness.sh"
        harness.write_text("#!/bin/bash\nset -euo pipefail\n"
                           + "ROOT=" + shlex.quote(str(ROOT)) + "\n"
                           + "RUN=" + shlex.quote(str(output)) + "\n"
                           + f"READINESS_DEADLINE=$((SECONDS + {budget}))\n"
                           + f"WORKFLOW_DEADLINE=$((SECONDS + {workflow_budget}))\n"
                           + "MODE=legacy-boundaries\nUNIT_RESULTS=()\n"
                           + self.helpers + "\n" + body + "\n", encoding="utf-8")
        environment = os.environ.copy()
        environment["PATH"] = str(fakebin) + os.pathsep + environment.get("PATH", "")
        start = time.monotonic()
        status = {"scope": "authored children executing real CI shell functions", "metal_api_verified": False,
                  "gpu_execution_verified": False, "ci_script_sha256": self.source_sha256}
        try:
            result = subprocess.run([BASH, str(harness)], cwd=output, env=environment,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15, check=False)
            status["returncode"] = result.returncode
            (output / "harness.stdout.log").write_bytes(result.stdout)
            (output / "harness.stderr.log").write_bytes(result.stderr)
        except subprocess.TimeoutExpired as error:
            status["harness_timed_out"] = True
            (output / "harness.stdout.log").write_bytes(error.stdout or b"")
            (output / "harness.stderr.log").write_bytes(error.stderr or b"")
            raise
        finally:
            status["elapsed_seconds"] = time.monotonic() - start
            (output / "test-control.json").write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
        return output, result, status

    def test_actual_compile_probe_resolves_source_from_evidence_cwd(self):
        fake = r'''
import json, pathlib, sys
arguments = sys.argv[1:]
sources = [pathlib.Path(value) for value in arguments if value.endswith('.metal')]
assert len(sources) == 1 and sources[0].is_absolute() and sources[0].is_file(), 'source missing from child cwd'
target = pathlib.Path(arguments[arguments.index('-o') + 1])
assert target.is_absolute() and target.parent.resolve() == pathlib.Path.cwd(), 'output must remain in run'
target.write_bytes(b'AUTHORED FAKE OUTPUT; THIS IS NOT METAL AIR')
print(json.dumps({'source': str(sources[0]), 'cwd': str(pathlib.Path.cwd()), 'output': str(target), 'real_compiler': False}))
'''
        output, result, _ = self.run_shell("absolute-source", "compile_probe authored", fake=fake)
        self.assertEqual(result.returncode, 0, result.stdout.decode() + result.stderr.decode())
        child = json.loads((output / "000-probe-authored.stdout.txt").read_text())
        self.assertEqual(Path(child["source"]).resolve(), (ROOT / "experiments/metal-compile/fixture.metal").resolve())
        self.assertEqual(Path(child["cwd"]), output)
        self.assertFalse(child["real_compiler"])
        self.assertTrue((output / "probe-authored.air").read_bytes().startswith(b"AUTHORED FAKE"))
        event = json.loads((output / "000-probe-authored.json").read_text())
        self.assertEqual(event["returncode"], 0)
        self.assertFalse(event["timed_out"])

    def test_actual_probe_rejects_zero_byte_output_even_when_child_exits_zero(self):
        fake = "import pathlib, sys\na=sys.argv[1:]\npathlib.Path(a[a.index('-o')+1]).touch()\n"
        output, result, _ = self.run_shell("empty-output", "compile_probe empty", fake=fake)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((output / "probe-empty.air").stat().st_size, 0)
        self.assertEqual(json.loads((output / "000-probe-empty.json").read_text())["returncode"], 0)

    def test_one_second_budget_stops_and_records_sleeping_authored_child(self):
        child = "import time; print('authored child started', flush=True); time.sleep(8)"
        body = "run_bounded authored-sleep 10 " + shlex.quote(sys.executable) + " -u -c " + shlex.quote(child)
        output, result, status = self.run_shell("deadline", body, budget=1)
        self.assertEqual(result.returncode, 124, result.stdout.decode() + result.stderr.decode())
        self.assertLess(status["elapsed_seconds"], 6)
        event = json.loads((output / "000-authored-sleep.json").read_text())
        self.assertTrue(event["timed_out"])
        self.assertEqual(event["timeout_seconds"], 1)
        self.assertIn("authored child started", (output / event["stdout"]).read_text())
        self.assertIsNotNone(event["termination"]["parent_returncode"])

    def test_expired_budget_refuses_to_launch_another_child(self):
        body = "run_bounded never-started 10 " + shlex.quote(sys.executable) + " -c 'raise SystemExit(99)'"
        output, result, _ = self.run_shell("expired", body, budget=0)
        self.assertEqual(result.returncode, 124)
        self.assertFalse((output / "000-never-started.json").exists())
        self.assertIn("budget exhausted", result.stderr.decode())

    def test_workflow_budget_can_be_shorter_than_readiness_budget(self):
        child = "import time; print('workflow child started', flush=True); time.sleep(8)"
        body = "run_bounded workflow-sleep 10 " + shlex.quote(sys.executable) + " -u -c " + shlex.quote(child)
        output, result, status = self.run_shell("workflow-deadline", body, budget=15, workflow_budget=1)
        self.assertEqual(result.returncode, 124, result.stdout.decode() + result.stderr.decode())
        self.assertLess(status["elapsed_seconds"], 6)
        event = json.loads((output / "000-workflow-sleep.json").read_text())
        self.assertTrue(event["timed_out"])
        self.assertEqual(event["timeout_seconds"], 1)
        self.assertIn("workflow child started", (output / event["stdout"]).read_text())

    def test_expired_workflow_budget_refuses_to_launch_during_readiness(self):
        body = "run_bounded workflow-never-started 10 " + shlex.quote(sys.executable) + " -c 'raise SystemExit(99)'"
        output, result, _ = self.run_shell("workflow-expired", body, budget=15, workflow_budget=0)
        self.assertEqual(result.returncode, 124)
        self.assertFalse((output / "000-workflow-never-started.json").exists())
        self.assertIn("budget exhausted", result.stderr.decode())

    def test_failed_unit_does_not_hide_later_independent_result(self):
        later = "from pathlib import Path; Path('later-observed').write_text('authored')"
        body = "run_experiment first 5 -c 'raise SystemExit(7)'\n" \
               + "run_experiment second 5 -c " + shlex.quote(later) + "\nwrite_unit_summary"
        output, result, _ = self.run_shell("independent-failure", body)
        self.assertEqual(result.returncode, 1)
        self.assertEqual((output / "later-observed").read_text(), "authored")
        summary = json.loads((output / "unit-summary.json").read_text())
        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["units"], [{"name": "first", "exit_code": 7}, {"name": "second", "exit_code": 0}])

    def test_unsupported_runtime_remains_non_success_in_full_mode(self):
        body = "MODE=full\nrun_experiment runtime 5 -c 'raise SystemExit(78)'\n" \
               + "run_experiment independent 5 -c 'raise SystemExit(0)'\nwrite_unit_summary"
        output, result, _ = self.run_shell("unit-unsupported", body)
        self.assertEqual(result.returncode, 78)
        summary = json.loads((output / "unit-summary.json").read_text())
        self.assertEqual(summary["status"], "unsupported")
        self.assertTrue(summary["metal4_runtime_requested"])

    def test_legacy_units_do_not_claim_metal4_or_iphone_validation(self):
        body = "run_experiment authored 5 -c 'raise SystemExit(0)'\nwrite_unit_summary"
        output, result, _ = self.run_shell("legacy-unit-pass", body)
        self.assertEqual(result.returncode, 0)
        summary = json.loads((output / "unit-summary.json").read_text())
        self.assertEqual(summary["status"], "passed")
        self.assertFalse(summary["metal4_runtime_requested"])
        self.assertFalse(summary["physical_iphone_verified"])
        self.assertFalse(summary["gameplay_improvement_verified"])

    def test_expired_unit_budget_is_recorded_without_launch(self):
        child = "from pathlib import Path; Path('should-not-exist').write_text('bad')"
        body = "run_experiment expired 5 -c " + shlex.quote(child) + "\nwrite_unit_summary"
        output, result, _ = self.run_shell("unit-expired", body, workflow_budget=0)
        self.assertEqual(result.returncode, 1)
        self.assertFalse((output / "should-not-exist").exists())
        summary = json.loads((output / "unit-summary.json").read_text())
        self.assertEqual(summary["units"], [{"name": "expired", "exit_code": 124}])
        self.assertEqual(summary["status"], "failed")


if __name__ == "__main__":
    unittest.main()
