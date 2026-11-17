#!/usr/bin/env python3
"""Compile the authored fixture to AIR and metallib; never execute GPU work.

This standalone experiment is not connected to the app or its compile settings.
Apple documents metal -c and the separate metallib linker here:
https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Dev-Technique/Dev-Technique.html
The MSL specification defines language versions and -fno-fast-math:
https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf
"""
import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "fixture.metal"
MAX_REPEAT = 20
DEFAULT_TIMEOUT = 120.0
TREE_TERMINATION_TIMEOUT = 5.0
PARENT_REAP_TIMEOUT = 2.0
LOG_PREVIEW_BYTES = 65536
TEST_DEADLINE_ENV = "SKYRIM_METAL_TEST_DEADLINE"
ENV_OPTIONS = (
    "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET",
    "TVOS_DEPLOYMENT_TARGET", "WATCHOS_DEPLOYMENT_TARGET", "XROS_DEPLOYMENT_TARGET",
    "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
)


class TrialError(RuntimeError):
    pass


class BudgetExhausted(TrialError):
    pass


def file_info(path):
    path = Path(path)
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        initial_size = os.fstat(stream.fileno()).st_size
        remaining = initial_size
        # A compiler that could not be terminated may still append output.
        # Hash only the observed extent instead of chasing a growing EOF.
        while remaining:
            chunk = stream.read(min(1024 * 1024, remaining))
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
            remaining -= len(chunk)
        changed = size != initial_size or os.fstat(stream.fileno()).st_size != initial_size
    return {"path": str(path), "bytes": size, "sha256": digest.hexdigest(), "changed_during_hash": changed}


def clean_environment():
    env = os.environ.copy()
    removed = [key for key in ENV_OPTIONS if key in env]
    for key in ENV_OPTIONS:
        env.pop(key, None)
    return env, removed


def terminate_tree(process, output, prefix):
    """Bound both tree termination and parent reaping; never wait for pipe EOF."""
    result = {"pid": process.pid, "parent_exited": False, "tree_termination_confirmed": False}
    started = time.perf_counter_ns()
    if os.name == "nt":
        taskkill = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/taskkill.exe"
        command = [str(taskkill), "/PID", str(process.pid), "/T", "/F"]
        result["argv"] = command
        result["timeout_seconds"] = TREE_TERMINATION_TIMEOUT
        out = output / (prefix + ".terminate.stdout.txt")
        err = output / (prefix + ".terminate.stderr.txt")
        result.update(stdout=out.name, stderr=err.name)
        try:
            with out.open("wb") as stdout, err.open("wb") as stderr:
                killer = subprocess.Popen(command, shell=False, stdout=stdout, stderr=stderr,
                                          creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    result["returncode"] = killer.wait(timeout=TREE_TERMINATION_TIMEOUT)
                    result["tree_termination_confirmed"] = result["returncode"] == 0
                except subprocess.TimeoutExpired:
                    result["timed_out"] = True
                    killer.kill()
                    try:
                        result["returncode"] = killer.wait(timeout=PARENT_REAP_TIMEOUT)
                    except subprocess.TimeoutExpired:
                        result["termination_helper_still_running"] = True
        except OSError as error:
            result["error"] = f"{type(error).__name__}: {error}"
    else:
        result["method"] = "killpg(SIGKILL)"
        try:
            # Every command starts its own session, so this cannot signal the
            # caller's process group or an unrelated compiler invocation.
            os.killpg(process.pid, signal.SIGKILL)
            result["tree_signal_sent"] = True
        except OSError as error:
            result["error"] = f"{type(error).__name__}: {error}"
    try:
        # Also stop the direct child if taskkill was unavailable/unsuccessful.
        if process.poll() is None:
            process.kill()
        result["parent_returncode"] = process.wait(timeout=PARENT_REAP_TIMEOUT)
        result["parent_exited"] = True
    except (OSError, subprocess.TimeoutExpired) as error:
        result["parent_error"] = f"{type(error).__name__}: {error}"
    # A POSIX group signal has no per-descendant acknowledgement. Do not claim
    # that reaping the parent proves every descendant has exited.
    result["children_may_remain"] = not result["tree_termination_confirmed"]
    result["duration_ns"] = time.perf_counter_ns() - started
    return result


def log_preview(path):
    with path.open("rb") as stream:
        data = stream.read(LOG_PREVIEW_BYTES)
    size = path.stat().st_size
    return data.decode("utf-8", errors="replace"), size, size > len(data)


def command_deadline(deadline, env):
    """An inherited test-supervisor deadline may shorten, never extend, a run."""
    if TEST_DEADLINE_ENV not in env:
        return deadline
    try:
        inherited = float(env[TEST_DEADLINE_ENV])
    except (TypeError, ValueError):
        raise ValueError(f"invalid {TEST_DEADLINE_ENV}: expected positive finite monotonic seconds") from None
    if not math.isfinite(inherited) or inherited <= 0:
        raise ValueError(f"invalid {TEST_DEADLINE_ENV}: expected positive finite monotonic seconds")
    return inherited if deadline is None else min(deadline, inherited)


@contextlib.contextmanager
def defer_sigint_during_creation():
    """Keep a Python SIGINT handler from interrupting Popen ownership assignment.

    The caller must put both Popen and its assignment inside this scope. Defer
    callable handlers only, then restore and invoke that same handler. SIG_IGN,
    SIG_DFL and non-main threads remain untouched. No blocked signal mask is
    inherited by the child. Repeated pending SIGINTs coalesce into one delivery.
    """
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    previous = signal.getsignal(signal.SIGINT)
    if not callable(previous):
        yield
        return
    pending = None

    def remember(signum, frame):
        nonlocal pending
        pending = (signum, frame)

    signal.signal(signal.SIGINT, remember)
    try:
        yield
    finally:
        signal.signal(signal.SIGINT, previous)
        if pending is not None:
            previous(*pending)


def run_command(command, label, output, report, env, timeout, deadline=None):
    """Write raw logs directly to files; inherited child handles cannot block EOF."""
    command = [str(value) for value in command]
    prefix = f"{len(report['commands']):03d}-{label}"
    stdout_path, stderr_path = output / (prefix + ".stdout.txt"), output / (prefix + ".stderr.txt")
    event = {"label": label, "argv": command, "shell": False, "cwd": str(output),
             "timeout_seconds": timeout, "stdout": stdout_path.name, "stderr": stderr_path.name,
             "timeout_cap_seconds": timeout, "process_started": False, "budget_exhausted": False,
             "returncode": None, "timed_out": False, "interrupted": False}
    started = time.perf_counter_ns()
    process = None
    try:
        options = {"creationflags": subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP} \
            if os.name == "nt" else {"start_new_session": True}
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            try:
                deadline = command_deadline(deadline, env)
            except ValueError as error:
                event["error"] = str(error)
                raise TrialError(str(error)) from error
            event["test_deadline_inherited"] = TEST_DEADLINE_ENV in env
            if deadline is not None:
                remaining = deadline - time.monotonic()
                event["budget_remaining_seconds"] = max(0.0, remaining)
                event["timeout_seconds"] = timeout = max(0.0, min(timeout, remaining))
                if remaining <= 0:
                    event["budget_exhausted"] = True
                    event["error"] = "overall tool wall budget exhausted before launch"
                    raise BudgetExhausted(label + ": " + event["error"])
            with defer_sigint_during_creation():
                process = subprocess.Popen(command, cwd=output, env=env, shell=False,
                                           stdout=stdout, stderr=stderr, **options)
                event["pid"] = process.pid
                event["process_started"] = True
            # Popen itself may consume time; it is not a cancellable operation.
            wait_timeout = timeout if deadline is None else max(0.0, min(timeout, deadline - time.monotonic()))
            event["wait_timeout_seconds"] = wait_timeout
            event["returncode"] = process.wait(timeout=wait_timeout)
    except subprocess.TimeoutExpired:
        event["timed_out"] = True
        event["budget_exhausted"] = deadline is not None and time.monotonic() >= deadline
        event["error"] = "command timed out"
        if process is not None:
            event["termination"] = terminate_tree(process, output, prefix)
    except (KeyboardInterrupt, SystemExit):
        event["interrupted"] = True
        event["error"] = "command interrupted"
        if process is not None:
            event["termination"] = terminate_tree(process, output, prefix)
        raise
    except OSError as error:
        event["error"] = f"{type(error).__name__}: {error}"
        if process is not None:
            event["termination"] = terminate_tree(process, output, prefix)
    except BaseException as error:
        # A caller's custom SIGINT handler may raise a different exception.
        # Preserve that exception after cleaning up the now-owned child.
        event.setdefault("error", f"{type(error).__name__}: {error}")
        if process is not None:
            event["termination"] = terminate_tree(process, output, prefix)
        raise
    finally:
        event["duration_ns"] = time.perf_counter_ns() - started
        stdout_text, event["stdout_bytes"], event["stdout_preview_truncated"] = log_preview(stdout_path)
        stderr_text, event["stderr_bytes"], event["stderr_preview_truncated"] = log_preview(stderr_path)
        report["commands"].append(event)
        (output / (prefix + ".json")).write_text(json.dumps(event, indent=2) + "\n", encoding="utf-8")
    return event, stdout_text, stderr_text


def require_success(event):
    if event.get("budget_exhausted"):
        raise BudgetExhausted(f"{event['label']}: overall tool wall budget exhausted; see {event['stderr']}")
    if event["timed_out"] or event["returncode"] != 0:
        reason = event.get("error", f"exit status {event['returncode']}")
        raise TrialError(f"{event['label']}: {reason}; see {event['stderr']}")


def tools_in(directory, system):
    directory = Path(directory).resolve()
    suffix = ".exe" if system == "Windows" else ""
    paths = {name: directory / (name + suffix) for name in ("metal", "metallib")}
    return paths if all(path.is_file() for path in paths.values()) else None


def windows_candidates(env):
    """Only known product roots and absolute PATH entries; no drive-wide search."""
    seen = set()
    candidates = []

    def add(path):
        key = str(path).casefold()
        if key not in seen and len(candidates) < 256:
            seen.add(key)
            candidates.append(path)

    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        base = env.get(variable)
        if not base:
            continue
        for pattern in ("Metal Developer Tools*", "Apple/Metal Developer Tools*"):
            for root in sorted(Path(base).glob(pattern), reverse=True)[:16]:
                if not root.is_dir():
                    continue
                add(root)
                for bin_pattern in ("bin", "*/bin", "*/*/bin", "*/*/*/bin"):
                    for directory in sorted(root.glob(bin_pattern))[:32]:
                        add(directory)
    for entry in env.get("PATH", "").split(";")[:128]:
        path = Path(entry.strip().strip('"'))
        if entry and path.is_absolute():
            add(path)
    return candidates


def resolve_tools(explicit, target, system, output, report, env, timeout, deadline=None):
    sdk = "iphoneos" if target == "ios" else "macosx"
    sdk_path = None
    if system == "Darwin":
        xcrun = shutil.which("xcrun", path=env.get("PATH"))
        if not xcrun:
            raise TrialError("xcrun is missing; install/select the Apple command-line tools and Metal toolchain")

        def query(*args):
            event, stdout, _ = run_command([xcrun, "--sdk", sdk, *args], "resolve-" + args[-1].lstrip("-"),
                                          output, report, env, timeout, deadline)
            require_success(event)
            value = stdout.strip()
            if not value or "\n" in value:
                raise TrialError("xcrun returned an empty or ambiguous path")
            path = Path(value)
            if not path.is_absolute():
                raise TrialError("xcrun returned a non-absolute path")
            # Driver selection can depend on argv[0]: metallib may be a
            # symlink to air-lld. Preserve xcrun's spelling when launching it.
            return path

        sdk_path = query("--show-sdk-path").resolve()
        if not sdk_path.is_dir():
            raise TrialError(f"Apple SDK directory is missing: {sdk_path}")
        if explicit is None:
            paths = {name: query("--find", name) for name in ("metal", "metallib")}
            if not all(path.is_file() for path in paths.values()):
                raise TrialError("xcrun did not resolve installed metal and metallib executables")
        else:
            paths = tools_in(explicit, system)
    elif system == "Windows":
        candidates = [Path(explicit)] if explicit is not None else windows_candidates(env)
        report["tool_search_directories"] = [str(path) for path in candidates]
        paths = next((found for path in candidates if (found := tools_in(path, system))), None)
    else:
        raise TrialError(f"Unsupported host {system}; this runner requires Windows or macOS Apple tools")
    if not paths:
        raise TrialError("Apple metal and metallib are missing from the same bin directory; supply --tools")
    report["sdk"] = {"name": sdk, "path": str(sdk_path) if sdk_path else None,
                     "selection": "xcrun" if system == "Darwin" else "bundled Windows toolchain"}
    return paths, sdk_path


def compile_command(metal, source, air, target, deployment, language, sdk_path=None):
    os_name = "ios" if target == "ios" else "macosx"
    # Apple metal 32023.850 requires platform-qualified standards for MSL 1/2;
    # MSL 3 and later use the unified spelling on both target platforms.
    standard = f"{target}-metal{language}" if int(language.split(".")[0]) < 3 else f"metal{language}"
    command = [str(metal), "-c", "-target", f"air64-apple-{os_name}{deployment}",
               f"-std={standard}", "-DPROBE_BIAS=3", "-fno-fast-math"]
    if sdk_path is not None:
        command += ["-isysroot", str(sdk_path)]
    return command + [str(source), "-o", str(air)]


def validate_artifact(path, metallib=False):
    if not path.is_file():
        raise TrialError(f"Expected output was not created: {path.name}")
    if path.stat().st_size == 0:
        raise TrialError(f"Empty output: {path.name}")
    if metallib:
        with path.open("rb") as stream:
            if stream.read(4) != b"MTLB" or path.stat().st_size <= 4:
                raise TrialError(f"Invalid metallib header or missing payload: {path.name}")


def run_experiment(args):
    # mkdir is the ownership boundary: even an existing empty directory is refused.
    output = args.output.expanduser().absolute()
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    system = platform.system()
    env, removed = clean_environment()
    report = {"schema": 1, "status": "running", "host": system, "commands": [], "trials": [],
              "options": {"target": args.target, "deployment": args.deployment, "language": args.language,
                          "repeat": args.repeat, "timeout_seconds": args.timeout, "define": "PROBE_BIAS=3",
                          "budget_seconds": args.budget,
                          "fast_math": False, "tools": str(args.tools) if args.tools else None},
              "removed_environment_options": removed,
              "offline_compile_verified": False, "gpu_execution_verified": False,
              "physical_iphone_verified": False, "game_rendering_verified": False,
              "performance_improvement_verified": False,
              "scope": "Authored fixture offline compilation only; not production compile-setting parity.",
              "timing_scope": "Host subprocess wall time; repeats share uncontrolled OS/tool caches. Not cold/warm proof.",
              "budget_scope": "Elapsed wall time from runner start caps admission and waits for all tools. Input I/O, process startup, cleanup and reporting are not hard-deadline bounded; external compiler services are not cancelled."}
    started = time.perf_counter_ns()
    deadline = None if args.budget is None else time.monotonic() + args.budget
    try:
        # Use one byte snapshot for all repeats and retain it with the evidence.
        source = output / "fixture.metal"
        source.write_bytes(SOURCE.read_bytes())
        report["source"] = file_info(source)
        report["source"]["original_path"] = str(SOURCE)
        paths, sdk_path = resolve_tools(args.tools, args.target, system, output, report, env, args.timeout, deadline)
        report["tools"] = {}
        for name, tool in paths.items():
            resolved_tool = tool.resolve(strict=True)
            info = file_info(resolved_tool)
            info.update(path=str(tool), resolved_path=str(resolved_tool))
            event, stdout, stderr = run_command([tool, "--version"], name + "-version", output, report, env, args.timeout, deadline)
            info["version_returncode"] = event["returncode"]
            info["version_timed_out"] = event["timed_out"]
            info["version_output"] = stdout + stderr
            info["version_output_truncated"] = event["stdout_preview_truncated"] or event["stderr_preview_truncated"]
            info["version_available"] = event["returncode"] == 0 and bool((stdout + stderr).strip())
            report["tools"][name] = info
            # A missing/broken executable is a failure. Some metallib versions do
            # not implement --version; retain that exit and the executable hash.
            if event["returncode"] is None:
                require_success(event)
        for index in range(args.repeat):
            directory = output / f"trial-{index + 1:02d}"
            directory.mkdir()
            air, library = directory / "fixture.air", directory / "fixture.metallib"
            trial = {"index": index + 1, "status": "running", "artifacts": []}
            report["trials"].append(trial)
            try:
                command = compile_command(paths["metal"], source, air, args.target, args.deployment,
                                          args.language, sdk_path)
                compile_event, _, _ = run_command(command, f"compile-{index + 1:02d}", output, report, env, args.timeout, deadline)
                trial["compile_duration_ns"] = compile_event["duration_ns"]
                require_success(compile_event)
                validate_artifact(air)
                link_event, _, _ = run_command([paths["metallib"], air, "-o", library],
                                               f"link-{index + 1:02d}", output, report, env, args.timeout, deadline)
                trial["link_duration_ns"] = link_event["duration_ns"]
                require_success(link_event)
                validate_artifact(library, metallib=True)
                trial["status"] = "passed"
            finally:
                for path in (air, library):
                    if path.is_file():
                        trial["artifacts"].append(file_info(path))
                if trial["status"] == "running":
                    trial["status"] = "failed"
        report["offline_compile_verified"] = True
        report["status"] = "passed"
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["error"] = "KeyboardInterrupt: execution interrupted; see command termination evidence"
        raise
    except BudgetExhausted as error:
        report["status"] = "budget_exhausted"
        report["error"] = f"{type(error).__name__}: {error}"
    except (OSError, TrialError) as error:
        report["status"] = "failed"
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        report["duration_ns"] = time.perf_counter_ns() - started
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def version(value):
    if not re.fullmatch(r"[1-9][0-9]?\.[0-9]{1,2}(?:\.[0-9]{1,2})?", value):
        raise argparse.ArgumentTypeError("expected a numeric version such as 18.0")
    return value


def repeat_count(value):
    value = int(value)
    if not 1 <= value <= MAX_REPEAT:
        raise argparse.ArgumentTypeError(f"repeat must be between 1 and {MAX_REPEAT}")
    return value


def timeout_seconds(value):
    value = float(value)
    if not math.isfinite(value) or not 0 < value <= 600:
        raise argparse.ArgumentTypeError("timeout must be greater than zero and at most 600 seconds")
    return value


def budget_seconds(value):
    value = float(value)
    if not math.isfinite(value) or not 0 < value <= 3600:
        raise argparse.ArgumentTypeError("budget must be greater than zero and at most 3600 seconds")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="fresh evidence directory; existing paths are refused")
    parser.add_argument("--tools", type=Path, help="directory containing Apple's metal and metallib executables")
    parser.add_argument("--target", choices=("ios", "macos"), default="ios")
    parser.add_argument("--deployment", type=version)
    parser.add_argument("--language", type=version, default="2.4")
    parser.add_argument("--repeat", type=repeat_count, default=3)
    parser.add_argument("--timeout", type=timeout_seconds, default=DEFAULT_TIMEOUT, help="per-command timeout in seconds")
    parser.add_argument("--budget", type=budget_seconds,
                        help="optional overall tool wall budget in seconds; cleanup/reporting can extend elapsed time")
    args = parser.parse_args(argv)
    args.deployment = args.deployment or ("18.0" if args.target == "ios" else "15.0")
    if args.language.count(".") != 1:
        parser.error("--language requires a major.minor version")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        report = run_experiment(args)
    except OSError as error:
        print(f"Cannot create a fresh output directory: {error}", file=sys.stderr)
        return 2
    print(f"offline compilation {report['status']}; evidence: {args.output.absolute() / 'result.json'}")
    if report["status"] != "passed":
        print(report["error"], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
