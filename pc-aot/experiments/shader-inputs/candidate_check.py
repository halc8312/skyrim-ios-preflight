#!/usr/bin/env python3
"""Private bounded DXSO analyze/key sample; never compile SPIR-V or use a GPU."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC = importlib.util.spec_from_file_location("candidate_authored_oracle", HERE / "native_check.py")
native = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(native)
study = native.study
CONTROL_SPEC = importlib.util.spec_from_file_location("candidate_process_control", ROOT / "experiments/metal-compile/offline.py")
control = importlib.util.module_from_spec(CONTROL_SPEC)
CONTROL_SPEC.loader.exec_module(control)
MAX_SELECTED_BYTES = 16 << 20
ISOLATION = {"kind": "isolation", "address_space_bytes": 512 << 20,
             "cpu_soft_seconds": 2, "cpu_hard_seconds": 3,
             "file_size_bytes": 1 << 20, "core_bytes": 0, "open_files": 32, "dumpable": False}
SCOPE = {"spirv_compilation_performed": False, "gpu_execution_verified": False,
         "full_fxp_grammar_verified": False, "native_compiler_safety_verified": False,
         "runtime_coverage_verified": False, "production_option_parity_verified": False,
         "vertex_pixel_linkage_verified": False, "performance_improvement_verified": False,
         "actual_inputs_asan_checked": False}


def write_json(path, document):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def child_environment():
    env = os.environ.copy()
    for name in ("LD_PRELOAD", "LD_LIBRARY_PATH", "DXVK_SHADER_DUMP_PATH", "ASAN_OPTIONS"):
        env.pop(name, None)
    env.update(DXVK_LOG_PATH="none", DXVK_LOG_LEVEL="warn",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    return env


def run_command(command, label, output, report, deadline, timeout=5):
    event = {"label": label, "argv": list(map(str, command)), "status": "pending",
             "returncode": None, "process_started": False, "timed_out": False,
             "stdout": label + ".stdout.log", "stderr": label + ".stderr.log"}
    report["commands"].append(event)
    path = output / (label + ".json")
    process = None
    started = time.monotonic()

    def stop():
        cleanup = event["cleanup"] = {"group_kill_sent": False, "parent_reaped": False}
        if process is not None and process.returncode is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                cleanup["group_kill_sent"] = True
            except OSError as error:
                cleanup["signal_error"] = str(error)
            try:
                process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup["reap_error"] = type(error).__name__ + ": " + str(error)
        if process is not None:
            event["returncode"] = process.returncode
            cleanup["parent_reaped"] = process.returncode is not None
        return cleanup["parent_reaped"]

    write_json(path, event)
    try:
        with (output / event["stdout"]).open("xb") as stdout, (output / event["stderr"]).open("xb") as stderr:
            remaining = min(timeout, deadline - time.monotonic())
            if remaining <= 0:
                event["status"] = "budget_exhausted"
                raise RuntimeError("aggregate deadline exhausted before " + label)
            command_deadline = min(deadline, time.monotonic() + remaining)
            with control.defer_sigint_during_creation():
                process = subprocess.Popen(event["argv"], cwd=output, env=child_environment(),
                                           stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                           start_new_session=True)
                event.update(process_started=True, pid=process.pid, timeout_seconds=remaining)
            write_json(path, event)
            try:
                event["returncode"] = process.wait(timeout=max(0, command_deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                event.update(timed_out=True, status="timeout")
                if not stop():
                    raise RuntimeError("timed-out child could not be reaped")
            else:
                event["status"] = "exited"
    except BaseException as error:
        if event["status"] not in ("budget_exhausted", "timeout"):
            event["status"] = "interrupted" if isinstance(error, (KeyboardInterrupt, SystemExit)) else "failed"
        event["error"] = type(error).__name__ + ": " + str(error)
        try:
            if "cleanup" not in event:
                stop()
        except BaseException as cleanup_error:
            event["cleanup_error"] = type(cleanup_error).__name__ + ": " + str(cleanup_error)
        raise
    finally:
        event["elapsed_seconds"] = max(0, time.monotonic() - started)
        write_json(path, event)
    return event


def read_rows(event, output):
    for name in ("stdout", "stderr"):
        if (output / event[name]).stat().st_size > ISOLATION["file_size_bytes"]:
            raise RuntimeError("native output exceeded hard file limit")
    rows = [json.loads(line) for line in (output / event["stdout"]).read_text().splitlines()]
    if not rows or rows[0] != ISOLATION:
        raise RuntimeError("native isolation was not established")
    return rows


def check_analysis(event, output, expected):
    rows = read_rows(event, output)
    if event["returncode"] != 0 or event["timed_out"] or (output / event["stderr"]).stat().st_size:
        raise RuntimeError("native analysis failed or emitted a diagnostic")
    required = {"kind": "analysis", "read_only_input": True, "end_guard": True,
                **{name: expected[name] for name in ("stage_vk", "analyzed_bytes", "dxvk_key")}}
    if rows != [ISOLATION, required]:
        raise RuntimeError("native consumption/stage/key mismatch")


def source_hashes(source):
    return {name: study.digest((source / "src" / name).read_bytes()) for name in native.UNITS}


def build(output, report):
    compiler, prlimit = shutil.which("g++"), shutil.which("prlimit")
    if not compiler or not prlimit:
        raise RuntimeError("existing Linux g++ and prlimit are required")
    source = ROOT / "vendor/skydream/third_party/dxvk"
    report["key_sources"] = study.verify_key_sources(source)
    report["native_sources"] = source_hashes(source)
    report["tool_snapshots"] = {}
    for name in ("candidate_check.py", "candidate_check.cpp", "native_check.py", "study.py"):
        raw = (HERE / name).read_bytes()
        (output / name).write_bytes(raw)
        report["tool_snapshots"][name] = study.digest(raw)
    raw = Path(control.__file__).read_bytes()
    (output / "command-control-source.py").write_bytes(raw)
    report["tool_snapshots"]["command-control-source.py"] = study.digest(raw)
    flags = [compiler, "-std=c++17", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
             "-fsanitize=undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
             "-Wno-deprecated-declarations", "-include", "cstdint", "-include", "cstring",
             "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00"]
    for folder in ("src", "src/util", "include", "include/native", "include/native/windows", "include/native/directx"):
        flags += ["-I", str(source / folder)]
    prefix = [prlimit, "--as=2147483648:2147483648", "--cpu=45:46", "--fsize=67108864:67108864", "--core=0:0", "--"]
    deadline = time.monotonic() + 300
    version = run_command(prefix + [compiler, "--version"], "compiler-version", output, report, deadline, 10)
    if version["returncode"] != 0:
        raise RuntimeError("compiler version probe failed")
    objects = []
    for index, name in enumerate(native.UNITS):
        obj = output / (Path(name).stem + ".o")
        event = run_command(prefix + flags + ["-x", "c++", "-c", source / "src" / name, "-o", obj],
                            f"compile-{index:02}", output, report, deadline, 45)
        if event["returncode"] != 0 or event["timed_out"]:
            raise RuntimeError("pinned native unit compilation failed: " + name)
        objects.append(obj)
    binary = output / "candidate-probe"
    event = run_command(prefix + flags + [output / "candidate_check.cpp", *objects,
                        "-Wl,--gc-sections", "-pthread", "-ldl", "-o", binary],
                        "link", output, report, deadline, 45)
    if event["returncode"] != 0 or event["timed_out"] or not binary.is_file():
        raise RuntimeError("native probe link failed")
    if source_hashes(source) != report["native_sources"]:
        raise RuntimeError("native sources changed during build")
    report["binary_sha256"] = study.digest(binary.read_bytes())
    return binary


def authored_checks(binary, output, report):
    checks = report["authored_checks"] = []
    deadline = time.monotonic() + 60
    for index, (name, marker, words) in enumerate(native.authored_cases()):
        code = struct.pack("<" + "I" * len(words), *words)
        expected = study.validate_sm3(code, marker)
        path = output / f"authored-{index:02}.dxso"
        path.write_bytes(code)
        event = run_command([binary, path], f"authored-{index:02}", output, report, deadline)
        check_analysis(event, output, expected)
        checks.append({"case": name, "status": "passed"})
    for name, raw, expected_signal in (
        ("bad_header", struct.pack("<2I", 0, 0xffff), None),
        ("bad_size", b"1234567", None),
        ("missing_end_guard", struct.pack("<2I", 0xfffe0300, 0), signal.SIGSEGV),
    ):
        path = output / (name + ".dxso")
        path.write_bytes(raw)
        event = run_command([binary, path], name, output, report, deadline)
        read_rows(event, output)
        if event["timed_out"] or event["returncode"] != (-expected_signal if expected_signal else 3):
            raise RuntimeError("authored malformed/boundary control failed: " + name)
        checks.append({"case": name, "status": "passed", "returncode": event["returncode"]})
    for name in ("memory", "cpu", "output", "wall"):
        event = run_command([binary, "--authored-" + name], "limit-" + name, output, report, deadline,
                            1 if name == "wall" else 5)
        rows = read_rows(event, output)
        if name == "memory":
            valid = event["returncode"] == 0 and rows[1:] == [{"kind": "control", "memory_limit_observed": True}]
        elif name == "cpu":
            valid = not event["timed_out"] and event["returncode"] in (-signal.SIGXCPU, -signal.SIGKILL)
        elif name == "output":
            valid = not event["timed_out"] and event["returncode"] == -signal.SIGXFSZ
        else:
            valid = event["timed_out"] and event["returncode"] == -signal.SIGKILL
        if not valid:
            raise RuntimeError("authored resource control failed: " + name)
        checks.append({"case": name, "status": "passed", "returncode": event["returncode"]})
    report["authored_checks_passed"] = True


def collect_candidates(archive):
    limits = study.Limits()
    catalog = study.read_bsa(archive, limits)
    budget = study.ValidationBudget()
    unique, decoded, markers, accepted = {}, 0, 0, 0
    for entry in catalog["entries"]:
        payload = study.decode_member(archive, entry, limits)
        decoded += len(payload)
        study.need(decoded <= limits.decoded_bytes, "total_decoded_limit")
        if not entry["name"].lower().endswith(".fxp"):
            continue
        records = study.scan_member(payload, limits, budget)
        markers += len(records)
        study.need(markers <= limits.candidates, "total_candidate_limit")
        for row in records:
            if row["status"] != "key_candidate":
                continue
            accepted += 1
            key = row["dxvk_key"]
            study.need(key not in unique or unique[key]["sha256"] == row["sha256"], "sha1_collision")
            unique.setdefault(key, {**row, "entry": entry})
    summary = {"archive_entries": len(catalog["entries"]), "candidate_records": accepted,
               "unique_keys": len(unique), "validation_bytes": budget.used}
    return list(unique.values()), summary


def select_candidates(candidates):
    selected = []
    for stage in (1, 16):
        ordered = sorted((row for row in candidates if row["stage_vk"] == stage),
                         key=lambda row: (row["analyzed_bytes"], row["dxvk_key"]))
        if len(ordered) < 16:
            raise ValueError("selection requires at least 16 unique candidates in each stage")
        selected.extend({**row, "selection_band": band} for band, rows in
                        (("smallest_eight", ordered[:8]), ("largest_eight", ordered[-8:])) for row in rows)
    if sum(row["analyzed_bytes"] for row in selected) > MAX_SELECTED_BYTES:
        raise ValueError("selected input byte budget exceeded")
    return selected


def run(args):
    if sys.platform != "linux":
        raise RuntimeError("Linux/WSL isolation is required; no unbounded fallback")
    output = study.private_output(args.output)
    report = {"schema": 1, "status": "running", "scope": "sampled native DXSO consumption/stage/key only",
              **SCOPE, "upstream_revision": study.UPSTREAM, "commands": [], "actual_results": [],
              "actual_candidates_submitted": 0, "authored_checks_passed": False,
              "budgets_seconds": {"build_tools": 300, "authored_controls": 60,
                                  "sample": 180, "per_native_command": 5, "cleanup_wait": 5},
              "budget_scope": "tool admission/waits; bounded input processing, startup, cleanup and report I/O can extend elapsed time",
              "input_limits": {"per_candidate_bytes": 1 << 20, "selected_bytes": MAX_SELECTED_BYTES,
                               "candidate_count": 32, "native": ISOLATION},
              "selection": "For each VS/PS stage: eight smallest and eight largest unique keys; sort (bytes,key)."}
    archive = identity = None
    try:
        write_json(output / "result.json", report)
        binary = build(output, report)
        authored_checks(binary, output, report)
        write_json(output / "result.json", report)
        if args.archive is None:
            report["status"] = "authored_checks_passed"
            return report
        archive, identity = study.read_snapshot(args.archive, study.Limits())
        report["archive"] = {"path": str(Path(args.archive).resolve()), "bytes": len(archive),
                             "sha256_before": study.digest(archive)}
        candidates, report["inventory_summary"] = collect_candidates(archive)
        selected = select_candidates(candidates)
        report["selected"] = selected
        deadline = time.monotonic() + 180
        for index, candidate in enumerate(selected):
            payload = study.decode_member(archive, candidate["entry"])
            begin, size = candidate["bytecode_offset"], candidate["analyzed_bytes"]
            code = payload[begin:begin + size]
            expected = study.validate_sm3(code, candidate["marker"].encode("ascii"))
            if any(expected[name] != candidate[name] for name in ("stage_vk", "analyzed_bytes", "dxvk_key", "sha256")):
                raise RuntimeError("selected candidate changed during extraction")
            path = output / f"candidate-{index + 1:02}.dxso"
            path.write_bytes(code)
            event = run_command([binary, path], f"candidate-{index + 1:02}", output, report, deadline)
            report["actual_candidates_submitted"] += int(event["process_started"])
            result = {"ordinal": index + 1, "key": candidate["dxvk_key"], "status": "failed",
                      "returncode": event["returncode"], "timed_out": event["timed_out"]}
            report["actual_results"].append(result)
            # A missing isolation prelude stops the sample, even if the child exited zero.
            read_rows(event, output)
            try:
                check_analysis(event, output, expected)
                if study.digest(path.read_bytes()) != expected["sha256"]:
                    raise RuntimeError("candidate snapshot changed during native analysis")
                result["status"] = "matched"
            except (RuntimeError, ValueError) as error:
                result["failure"] = str(error)
            write_json(output / "result.json", report)
        report["matched_candidates"] = sum(row["status"] == "matched" for row in report["actual_results"])
        report["status"] = "matched" if report["matched_candidates"] == 32 else "failed"
    except Exception as error:
        report.update(status="failed", failure=type(error).__name__ + ": " + str(error))
    except BaseException as error:
        report.update(status="interrupted", failure=type(error).__name__)
        raise
    finally:
        report["actual_candidates_submitted"] = sum(event["process_started"] for event in report["commands"]
                                                     if event["label"].startswith("candidate-"))
        if archive is not None:
            try:
                after, after_identity = study.read_snapshot(args.archive, study.Limits())
                original = report.setdefault("archive", {"path": str(Path(args.archive).resolve()),
                                                          "sha256_before": study.digest(archive)})
                original["sha256_after"] = study.digest(after)
                if identity != after_identity or archive != after:
                    raise RuntimeError("original archive changed during probe")
                original["unchanged"] = True
            except Exception as error:
                report["archive_verification_error"] = type(error).__name__ + ": " + str(error)
                if report["status"] != "interrupted":
                    report["status"] = "failed"
        write_json(output / "result.json", report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--archive", type=Path)
    mode.add_argument("--authored-only", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = run(args)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(2, type(error).__name__ + ": " + str(error) + "\n")
    print(json.dumps({key: report.get(key) for key in
                     ("status", "authored_checks_passed", "actual_candidates_submitted", "matched_candidates", "failure")}))
    return 0 if report["status"] in ("matched", "authored_checks_passed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
