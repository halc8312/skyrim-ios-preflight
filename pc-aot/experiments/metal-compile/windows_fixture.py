#!/usr/bin/env python3
"""Verify one pinned, authored Windows metallib with the existing Mac fixture.

The private build input is never printed or forwarded to child processes. This
is a fixed artifact check, not a general transfer facility or signature check.
Metal 4, physical iPhone, production rendering and performance are out of scope.
"""
import argparse
import base64
import binascii
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import sys
import time

HERE = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


batch = load("windows_fixture_io", "batch.py")
control = batch.offline
runtime = load("windows_fixture_runtime", "runtime.py")
INPUT_ENV = "SKYRIM_AUTHORED_WINDOWS_LIBRARY"
SCOPE = "pinned-authored-windows-metallib-mac-readback"
LIBRARY_BYTES = 11352
LIBRARY_SHA256 = "86dc2baa8ab47c4523318570c6eaed9053ed0b46200a311afd5b43cf2d9bfc0c"
SOURCE_BYTES = 1024
SOURCE_SHA256 = "e45755523aabf64eb236c01a77ae220e271b4ae6d0eabcf120d9a80cabbada87"
BASE64_BYTES = 4 * ((LIBRARY_BYTES + 2) // 3)
CLEANUP_RESERVE_SECONDS = 10.0
FALSE_EVIDENCE = ("metal4_runtime_verified", "physical_iphone_verified", "game_rendering_verified",
                  "production_adapter_verified", "production_compile_options_parity_verified",
                  "performance_improvement_verified", "signature_verified")
SNAPSHOTS = ("windows_fixture.py", "runtime.py", "runtime.mm", "offline.py", "batch.py")
# Only the child receives the earlier absolute deadline. Passing it in the
# parent's env would also shorten the parent's helper wait and erase its cleanup
# reserve. No payload or user-selected code is interpolated into this bootstrap.
RUNTIME_BOOTSTRAP = (
    "import os,runpy,sys\n"
    "os.environ['SKYRIM_METAL_TEST_DEADLINE']=sys.argv[1]\n"
    "sys.argv=sys.argv[2:]\n"
    "runpy.run_path(sys.argv[0],run_name='__main__')\n"
)


def require(value, message):
    if not value:
        raise ValueError(message)


def pinned_bytes(data, expected_size, expected_hash, label):
    require(len(data) == expected_size, label + " size differs from the fixed authored input")
    require(hashlib.sha256(data).hexdigest() == expected_hash, label + " SHA-256 differs from the fixed authored input")
    return data


def decode_library(value):
    require(isinstance(value, str) and bool(value), INPUT_ENV + " is required")
    require(len(value) == BASE64_BYTES, "Private artifact base64 length differs from the fixed input")
    try:
        data = base64.b64decode(value.encode("ascii"), validate=True)
    except (UnicodeError, binascii.Error, ValueError):
        raise ValueError("Private artifact is not strict ASCII base64") from None
    require(base64.b64encode(data).decode("ascii") == value, "Private artifact base64 is not canonical")
    require(data.startswith(b"MTLB"), "Private artifact is not a metallib")
    return pinned_bytes(data, LIBRARY_BYTES, LIBRARY_SHA256, "Library")


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def validate_runtime(report, native, output):
    require(type(report) is dict and type(report.get("schema")) is int and report["schema"] == 1
            and report.get("scope") == runtime.SCOPE and report.get("status") == "passed",
            "Runtime comparison did not pass")
    for key in ("native_compilation_verified", "gpu_execution_verified", "legacy_only_requested",
                "async_library_requested", "async_adapter_verified"):
        require(report.get(key) is True, "Runtime verification missing: " + key)
    for key in (*runtime.FALSE_EVIDENCE, "metal4_library_verified", "metal4_async_adapter_verified",
                "library_factory_probe_requested"):
        require(report.get(key) is False, "Unexpected runtime evidence claim: " + key)
    require(report.get("comparison_mode") == "legacy-only" and type(report.get("repeat")) is int
            and report["repeat"] == 3 and report.get("async_apis") == ["legacy_async_source"],
            "Runtime comparison mode/repetition differs")
    require(json.dumps(report.get("native"), sort_keys=True) == json.dumps(native, sort_keys=True),
            "Embedded and retained native reports differ")
    runtime.validate_native(native, 3, True, async_library=True, legacy_only=True)
    inputs = report.get("inputs")
    require(type(inputs) is dict, "Runtime input identities are missing")
    for name, size, digest in (("fixture.metal", SOURCE_BYTES, SOURCE_SHA256),
                               ("offline.metallib", LIBRARY_BYTES, LIBRARY_SHA256)):
        require(type(inputs.get(name)) is dict and inputs[name].get("sha256") == digest,
                "Runtime input identity differs: " + name)
        pinned_bytes(batch.read_bounded(output / name, size), size, digest, name)
    # validate_native also verifies all nine ordered samples, real source
    # rejection observations, and the controlled legacy async retention cases.
    require([sample["iteration"] for sample in native["samples"] if sample["api"] == "offline_metallib"] == [1, 2, 3],
            "The offline library route was not exercised three times")


def run(args):
    require(math.isfinite(args.budget) and 0 < args.budget <= 3600, "budget must be positive, finite, and at most 3600 seconds")
    output = batch.fresh_directory(args.output)
    started = time.monotonic()
    deadline = started + args.budget
    child_output = output / "runtime-evidence"
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [], "inputs": {},
              "input_artifact_verified": False, "cross_host_gpu_verified": False,
              **{key: False for key in FALSE_EVIDENCE},
              "expected_library": {"bytes": LIBRARY_BYTES, "sha256": LIBRARY_SHA256,
                                   "origin_record": "windows-macos-6.2-01/trial-01", "declared_tool_release": "Apple Windows Metal tools 6.2"},
              "expected_source": {"bytes": SOURCE_BYTES, "sha256": SOURCE_SHA256},
              "declared_offline_options": {"target": "macos", "deployment": "15.0", "language": "2.4",
                                           "preprocessor_macros": {"PROBE_BIAS": 3}, "fast_math": False},
              "identity_scope": "SHA-256 fixes the authored bytes; it does not verify a signature or independently reconstruct compiler options.",
              "runtime_mode": "legacy-only + async-library", "repeat": 3,
              "budget_seconds": args.budget, "child_cleanup_reserve_seconds": CLEANUP_RESERVE_SECONDS,
              "budget_scope": "Shared absolute tool deadlines; child tools stop admission/waits 10 seconds before the outer supervisor. Startup, cleanup, reporting and external services are not hard-deadline bounded."}
    write_report(output, report)
    try:
        source = pinned_bytes(batch.read_bounded(HERE / "fixture.metal", SOURCE_BYTES), SOURCE_BYTES, SOURCE_SHA256, "Source")
        library = decode_library(os.environ.get(INPUT_ENV))
        (output / "fixture.metal").write_bytes(source)
        library_path = output / "windows-fixture.metallib"
        library_path.write_bytes(library)
        report["input_artifact_verified"] = True
        report["inputs"]["fixture.metal"] = control.file_info(output / "fixture.metal")
        report["inputs"][library_path.name] = control.file_info(library_path)
        for name in SNAPSHOTS:
            (output / name).write_bytes((HERE / name).read_bytes())
            report["inputs"][name] = control.file_info(output / name)
        if platform.system() != "Darwin":
            report.update(status="unsupported_host", reason="The cross-host GPU check requires macOS 26 and a Metal device")
            return report
        env, report["removed_environment_options"] = control.clean_environment()
        env.pop(INPUT_ENV, None)
        deadline = control.command_deadline(deadline, env)
        child_deadline = deadline - CLEANUP_RESERVE_SECONDS
        remaining = child_deadline - time.monotonic()
        if remaining <= 0:
            raise control.BudgetExhausted("No runtime budget remains after reserving child cleanup time")
        report["effective_deadline_monotonic"] = deadline
        report["runtime_tool_deadline_monotonic"] = child_deadline
        report["runtime_result"] = "runtime-evidence/result.json"
        write_report(output, report)
        argv = [sys.executable, "-c", RUNTIME_BOOTSTRAP, str(child_deadline), output / "runtime.py",
                "--output", child_output, "--library", library_path, "--legacy-only", "--async-library",
                "--repeat", "3", "--budget", str(remaining)]
        event, _, _ = control.run_command(argv, "runtime-comparison", output, report, env,
                                          max(0.0, deadline - time.monotonic()), deadline=deadline)
        # Process failure takes precedence over a missing/partial child report.
        control.require_success(event)
        child_report, _ = batch.read_json(child_output / "result.json")
        native, _ = batch.read_json(child_output / "native.json")
        report["runtime"] = child_report
        validate_runtime(child_report, native, child_output)
        pinned_bytes(batch.read_bounded(library_path, LIBRARY_BYTES), LIBRARY_BYTES, LIBRARY_SHA256, "Original library")
        report.update(status="passed", cross_host_gpu_verified=True)
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", reason=type(error).__name__)
        raise
    except Exception as error:
        report.update(status="budget_exhausted" if isinstance(error, control.BudgetExhausted) else "failed",
                      reason=type(error).__name__ + ": " + str(error))
    finally:
        if "runtime" not in report and (child_output / "result.json").is_file():
            try:
                report["runtime"], _ = batch.read_json(child_output / "result.json")
            except (OSError, ValueError, batch.BatchError) as error:
                report["runtime_report_error"] = type(error).__name__ + ": " + str(error)
        report["elapsed_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="fresh ignored evidence directory")
    parser.add_argument("--budget", type=float, default=120)
    args = parser.parse_args(argv)
    if not math.isfinite(args.budget) or not 0 < args.budget <= 3600:
        parser.error("budget must be positive, finite, and at most 3600 seconds")
    return args


def main(argv=None):
    try:
        report = run(parse_args(argv))
    except (OSError, ValueError, batch.BatchError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print("Pinned authored Windows metallib: " + report["status"])
    return 0 if report["status"] == "passed" else 78 if report["status"] == "unsupported_host" else 1


if __name__ == "__main__":
    raise SystemExit(main())
