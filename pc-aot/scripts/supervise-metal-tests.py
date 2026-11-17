#!/usr/bin/env python3
"""Bound the authored Metal experiment's unittest worker on POSIX hosts.

Tools receive an earlier shared deadline so their own process-group cleanup can
finish first. At the worker deadline, send SIGINT for cooperative Python cleanup;
only then use SIGKILL on the owned worker group. Other sessions are never scanned
or signalled. A hard fallback therefore cannot prove that every nested tool or
external Apple service exited, and its report explicitly says so.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SHARED_DEADLINE_ENV = "SKYRIM_METAL_TEST_DEADLINE"


def positive_budget(value):
    value = float(value)
    if not math.isfinite(value) or not 0 < value <= 720:
        raise argparse.ArgumentTypeError("budget must be finite, greater than zero, and at most 720 seconds")
    return value


def tool_deadline(started, budget, reserve, inherited=None):
    deadline = started + budget - reserve
    if inherited is not None:
        previous = float(inherited)
        if not math.isfinite(previous) or previous <= 0:
            raise ValueError("invalid inherited shared test deadline")
        deadline = min(deadline, previous)
    return deadline


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def stop_worker(process, report, interrupt_grace, reap_grace):
    cleanup = {"scope": "only the new worker process group; other sessions are not enumerated or signalled",
               "sigint_sent": False, "sigkill_sent": False, "worker_reaped": False,
               "nested_process_groups_termination_verified": False}
    report["cleanup"] = cleanup
    # Do not poll/reap before signalling. While returncode is None this Popen
    # still owns an unreaped child PID, preventing an unrelated reused PID from
    # being mistaken for the worker group. A reaped leader is never signalled.
    if process.returncode is None:
        try:
            os.killpg(process.pid, signal.SIGINT)
            cleanup["sigint_sent"] = True
        except OSError as error:
            cleanup["sigint_error"] = str(error)
        try:
            process.wait(timeout=interrupt_grace)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                cleanup["sigkill_sent"] = True
            except OSError as error:
                cleanup["sigkill_error"] = str(error)
            try:
                process.wait(timeout=reap_grace)
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup["reap_error"] = str(error)
        except OSError as error:
            cleanup["wait_error"] = str(error)
    cleanup["worker_reaped"] = process.returncode is not None
    cleanup["worker_returncode"] = process.returncode
    cleanup["nested_process_groups_may_remain"] = True
    cleanup["limits"] = ("SIGINT permits the test helpers to stop their own separate tool groups; "
                         "worker exit and worker-group SIGKILL do not establish termination of those groups or external services")


def run_supervised(command, output, budget, *, cwd=ROOT, env=None,
                   cleanup_reserve=10.0, interrupt_grace=10.0, reap_grace=5.0):
    started = time.monotonic()
    worker_deadline = started + budget
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": 1, "scope": "authored unittest worker supervision; not Metal/GPU validation",
        "status": "running", "argv": [str(part) for part in command], "cwd": str(cwd),
        "budget_seconds": budget, "cleanup_reserve_seconds": cleanup_reserve,
        "interrupt_grace_seconds": interrupt_grace, "reap_grace_seconds": reap_grace,
        "process_started": False, "new_session_requested": True,
        "stdout": "worker.stdout.log", "stderr": "worker.stderr.log",
        "budget_scope": "worker admission and wait; process startup, I/O, bounded cleanup and reporting can extend elapsed time",
        "descendant_limits": "known tool helpers receive an earlier shared deadline; other sessions are never broadly killed",
    }
    process = None
    write_report(output, report)
    try:
        source = Path(__file__).read_bytes()
        (output / "supervisor-source.py").write_bytes(source)
        report["source_sha256"] = hashlib.sha256(source).hexdigest()
        if os.name != "posix":
            report["status"] = "unsupported"
            report["failure"] = "this supervisor requires POSIX process groups"
            return report
        child_env = dict(os.environ if env is None else env)
        shared = tool_deadline(started, budget, cleanup_reserve, child_env.get(SHARED_DEADLINE_ENV))
        report["shared_tool_deadline"] = shared
        child_env[SHARED_DEADLINE_ENV] = repr(shared)
        with (output / report["stdout"]).open("wb") as stdout, (output / report["stderr"]).open("wb") as stderr:
            if min(worker_deadline, shared) <= time.monotonic():
                report["status"] = "budget_exhausted"
                report["failure"] = "no tool-work allowance remains before worker launch"
                return report
            process = subprocess.Popen(report["argv"], cwd=cwd, env=child_env, stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, shell=False, start_new_session=True)
            report.update(process_started=True, worker_pid=process.pid, worker_pgid=process.pid)
            write_report(output, report)
            try:
                process.wait(timeout=max(0.0, worker_deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                report["status"] = "budget_exhausted"
                report["failure"] = "unittest worker exhausted its aggregate budget"
                stop_worker(process, report, interrupt_grace, reap_grace)
            else:
                report["status"] = "passed" if process.returncode == 0 else "failed"
                if time.monotonic() > worker_deadline:
                    report["status"] = "budget_exhausted"
                    report["failure"] = "worker exit was observed after its budget"
            report["worker_returncode"] = process.returncode
    except (KeyboardInterrupt, SystemExit) as error:
        report["status"] = "interrupted"
        report["failure"] = type(error).__name__ + ": " + str(error)
        if process is not None:
            stop_worker(process, report, interrupt_grace, reap_grace)
            report["worker_returncode"] = process.returncode
        raise
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = type(error).__name__ + ": " + str(error)
        if process is not None:
            stop_worker(process, report, interrupt_grace, reap_grace)
            report["worker_returncode"] = process.returncode
    finally:
        report["elapsed_seconds"] = max(0.0, time.monotonic() - started)
        write_report(output, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--budget", required=True, type=positive_budget)
    args = parser.parse_args(argv)
    command = [sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
               "-p", "test_metal_compile_*.py", "-v"]
    try:
        report = run_supervised(command, args.output, args.budget)
    except FileExistsError:
        print("choose a fresh supervisor evidence directory", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("Metal unittest supervision interrupted; partial evidence retained", file=sys.stderr)
        return 130
    for key, stream in (("stdout", sys.stdout), ("stderr", sys.stderr)):
        path = args.output.resolve() / report[key]
        # A separate-session descendant could still hold this file open after a
        # hard fallback. Read only the observed extent, never chase a growing EOF.
        if path.is_file():
            with path.open("rb") as log:
                remaining = os.fstat(log.fileno()).st_size
                while remaining:
                    chunk = log.read(min(65536, remaining))
                    if not chunk:
                        break
                    stream.write(chunk.decode("utf-8", errors="replace"))
                    remaining -= len(chunk)
    print("METAL_TEST_SUPERVISOR status=" + report["status"])
    return 0 if report["status"] == "passed" else 124 if report["status"] == "budget_exhausted" else 78 if report["status"] == "unsupported" else 1


if __name__ == "__main__":
    def interrupt_from_term(signum, frame):
        raise KeyboardInterrupt("supervisor received SIGTERM")
    if os.name == "posix":
        signal.signal(signal.SIGTERM, interrupt_from_term)
    raise SystemExit(main())
