#!/usr/bin/env python3
"""Bounded authored legacy Metal/MRC probe; no game inputs or Metal 4 tasks."""
import argparse
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


control = load("mrc_metal_control", "offline.py")
batch = load("mrc_metal_io", "batch.py")
SCOPE = "authored-legacy-metal-mrc-owner-retirement"
FALSE_EVIDENCE = ("metal4_task_semantics_verified", "real_compile_timeout_verified", "compiler_cancellation_verified",
                  "production_options_parity_verified", "production_adapter_verified", "physical_iphone_verified",
                  "game_rendering_verified", "performance_improvement_verified", "internal_compiler_parallelism_verified")
CASES = ("success", "invalid_source", "controlled_publication")
WARNING_MARKER = "authored_nonfatal_diagnostic"
VARIANTS = {"normal": [], "asan-ubsan": ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]}
EXPECTED_WORDS = [value for index in range(4) for value in (index + 3, (index + 1) * 7, 10, 0x12345678)]


def expected_bias(name, parallel):
    return {"controlled_publication": 13, "warning_source": 23}.get(name, 3) if parallel else 3


def expected_words(bias):
    return [value for index in range(4) for value in (index + bias, (index + 1) * 7, 10, 0x12345678)]


def require(value, message):
    if not value:
        raise ValueError(message)


def error_info(value, *, required=False):
    require(type(value) is dict and type(value.get("present")) is bool, "NSError observation missing")
    if required:
        require(value["present"], "expected source rejection has no NSError")
    if value["present"]:
        require(type(value.get("code")) is int and type(value.get("domain")) is str and 0 < len(value["domain"]) <= 1024,
                "NSError domain/code malformed")
        for key in ("description", "full_description", "user_info_description"):
            require(type(value.get(key)) is str and len(value[key]) <= 262144, "NSError diagnostic missing or oversized")


def validate_native(native, unsupported=False, *, parallel=False, warning_source=False):
    require(type(native) is dict and type(native.get("schema")) is int and native["schema"] == 1
            and native.get("scope") == SCOPE and native.get("manual_reference_counting") is True, "native scope/MRC mismatch")
    for key in FALSE_EVIDENCE:
        require(native.get(key) is False, "unsupported evidence claim: " + key)
    require(native.get("execution_mode") == ("parallel" if parallel else "sequential")
            and native.get("warning_source_requested") is warning_source, "execution mode/optional warning differs")
    if unsupported:
        require(native.get("status") == "unsupported" and native.get("reason") == "no_default_metal_device"
                and native.get("cases") == [] and native.get("legacy_async_library_verified") is False
                and native.get("exact_compute_readback_verified") is False, "unsupported result promoted evidence")
        return
    require(native.get("status") == "passed" and native.get("legacy_async_library_verified") is True
            and native.get("exact_compute_readback_verified") is True, "native Metal/MRC probe did not pass")
    device = native.get("device")
    require(type(device) is dict and set(device) == {"name"} and type(device.get("name")) is str,
            "native device observation missing")
    cases = native.get("cases")
    names = CASES + (("warning_source",) if warning_source else ())
    require(type(cases) is list and len(cases) == len(names), "native cases incomplete")
    for row, name in zip(cases, names):
        require(type(row) is dict and row.get("case") == name and row.get("status") == "passed", "case identity/status mismatch")
        controlled, invalid = name == "controlled_publication", name == "invalid_source"
        bias = expected_bias(name, parallel)
        require(type(row.get("bias")) is int and row["bias"] == bias and row.get("macros") == {
            "AUTHORED_BIAS": str(bias) + "u", "AUTHORED_WARNING": "1" if name == "warning_source" else "0"},
            "per-request macro/bias differs")
        for key in ("library_compile_attempts", "callbacks", "notifications", "retirements"):
            require(type(row.get(key)) is int and row[key] == 1, "callback/retirement count mismatch: " + key)
        for key in ("caller_pool_expired_before_launch", "retirement_after_launch_exit", "tracked_owner_retired",
                    "callback_pool_drained_before_observation", "original_callback_objects_preserved"):
            require(row.get(key) is True, "ownership boundary unverified: " + key)
        require(type(row.get("notification_before_launch_exit")) is bool, "notification ordering missing")
        require(row.get("controlled_publication") is controlled and
                row.get("destroy_requested_before_controlled_publication") is controlled, "controlled publication mislabeled")
        if controlled:
            require(row["notification_before_launch_exit"] is False, "controlled publication preceded launch exit")
        require(row.get("library_created") is (not invalid), "source result incorrectly classified")
        error_info(row.get("library_error"), required=invalid or name == "warning_source")
        if name == "warning_source":
            diagnostic = row["library_error"]
            require(row.get("warning_marker_present") is True and any(
                WARNING_MARKER in diagnostic.get(key, "") for key in ("description", "full_description", "user_info_description")),
                "real marked NSError warning was not observed")
        require(row.get("exact_compute_readback_verified") is (not invalid), "GPU evidence differs from case")
        if invalid:
            require("compute_words" not in row and "command_completed" not in row, "invalid library executed GPU work")
        else:
            require(row.get("command_completed") is True and row.get("compute_words") == expected_words(bias)
                    and all(type(word) is int for word in row["compute_words"]), "exact GPU words differ")
            error_info(row.get("pipeline_error"))
            error_info(row.get("command_error"))
            require(row["command_error"]["present"] is False, "failed GPU command promoted to success")


def write_report(output, report):
    pending = output / "result.tmp"
    pending.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    pending.replace(output / "result.json")


def run(args):
    output = batch.fresh_directory(args.output)
    started = time.monotonic()
    deadline = started + args.budget
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [], "inputs": {}, "variants": [],
              "legacy_async_library_verified": False, "exact_compute_readback_verified": False,
              **{key: False for key in FALSE_EVIDENCE}, "budget_seconds": args.budget,
              "requested_variants": args.variants,
              "execution_mode": "parallel" if args.parallel_cases else "sequential",
              "warning_source_requested": args.warning_source,
              "prior_warning_boundary": "Mac run 9 created a library but no NSError for #warning; its strict failed expectation is retained, not reclassified.",
              "control_scope": "Controlled publication after actual completion; not a compiler timeout/cancel test.",
              "ownership_scope": "One authored owner plus explicit retained library/NSError; not Foundation/Metal-wide leak proof.",
              "runtime_stderr_policy": "Any nonempty stderr requires review and prevents pass, even if it duplicates the expected source NSError.",
              "budget_scope": "Tool admission/waits bounded; process cleanup and compiler services can outlive the budget."}
    write_report(output, report)
    try:
        for filename in ("mrc_metal.mm", "mrc_metal.py", "owner_retirement.h", "offline.py", "batch.py"):
            data = (HERE / filename).read_bytes()
            (output / filename).write_bytes(data)
            report["inputs"][filename] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if platform.system() != "Darwin":
            report.update(status="unsupported_host", reason="Legacy Metal/MRC probe requires macOS")
            return report
        env = dict(os.environ)
        for key in control.ENV_OPTIONS:
            env.pop(key, None)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        def command(argv, label, required=True):
            require(time.monotonic() < deadline, "probe budget exhausted before " + label)
            event, stdout, stderr = control.run_command(argv, label, output, report, env, args.timeout, deadline=deadline)
            if required:
                control.require_success(event)
            return event, stdout, stderr

        xcrun = "/usr/bin/xcrun"
        _, version, _ = command([xcrun, "--sdk", "macosx", "--show-sdk-version"], "sdk-version")
        if not version.strip().split(".")[0].isdigit() or int(version.strip().split(".")[0]) < 15:
            report.update(status="unsupported_sdk", reason="macOS SDK 15+ is required for explicitly constructed math options")
            return report
        _, sdk_text, _ = command([xcrun, "--sdk", "macosx", "--show-sdk-path"], "sdk-path")
        sdk = Path(sdk_text.strip())
        require(sdk.is_absolute() and sdk.is_dir(), "SDK path unavailable")
        report["sdk"] = {"version": version.strip(), "path": str(sdk)}
        command([xcrun, "--sdk", "macosx", "clang++", "--version"], "clangxx-version")
        for variant in args.variants:
            row = {"variant": variant, "status": "running", "native_compilation_verified": False,
                   "legacy_async_library_verified": False, "exact_compute_readback_verified": False}
            report["variants"].append(row)
            try:
                binary = output / ("mrc-metal-" + variant)
                command([xcrun, "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
                         "-fno-objc-arc", "-fblocks", "-fexceptions", "-fobjc-exceptions", "-pthread",
                         "-Wall", "-Wextra", "-Werror", "-mmacosx-version-min=13.3", "-isysroot", sdk,
                         *VARIANTS[variant], "-framework", "Foundation", "-framework", "Metal",
                         output / "mrc_metal.mm", "-o", binary], variant + "-compile")
                control.validate_artifact(binary)
                row.update(native_compilation_verified=True, binary=control.file_info(binary))
                native_path = output / (variant + "-native.json")
                native_args = [binary, "--report", native_path]
                if args.parallel_cases:
                    native_args.append("--parallel-cases")
                if args.warning_source:
                    native_args.append("--warning-source")
                event, _, runtime_stderr = command(native_args, variant + "-run", False)
                if event.get("returncode") == 78 and not event.get("timed_out") and not event.get("interrupted"):
                    native, _ = batch.read_json(native_path)
                    row["native"] = native
                    validate_native(native, unsupported=True, parallel=args.parallel_cases, warning_source=args.warning_source)
                    require(not runtime_stderr.strip(), "unsupported runtime emitted diagnostics; inspect stderr")
                    row.update(status="unsupported")
                    report.update(status="unsupported_device", reason=native["reason"])
                    return report
                control.require_success(event)
                native, _ = batch.read_json(native_path)
                row["native"] = native
                require(not runtime_stderr.strip(), "native runtime emitted diagnostics; inspect retained stderr")
                validate_native(native, parallel=args.parallel_cases, warning_source=args.warning_source)
                row.update(status="passed", legacy_async_library_verified=True, exact_compute_readback_verified=True)
            except (KeyboardInterrupt, SystemExit):
                row["status"] = "interrupted"
                raise
            except Exception as error:
                row.update(status="failed", reason=type(error).__name__ + ": " + str(error))
                raise
        report.update(status="passed", legacy_async_library_verified=True, exact_compute_readback_verified=True)
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", reason=type(error).__name__)
        raise
    except Exception as error:
        report.update(status="failed", reason=type(error).__name__ + ": " + str(error))
    finally:
        report["elapsed_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=["normal", "asan-ubsan"])
    parser.add_argument("--parallel-cases", action="store_true", help="Start independent authored requests on separate threads; no compiler parallelism claim")
    parser.add_argument("--warning-source", action="store_true", help="Also require a real marked NSError warning; prior Mac observation failed this expectation")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--budget", type=float, default=180)
    args = parser.parse_args(argv)
    if len(set(args.variants)) != len(args.variants):
        parser.error("each variant may be requested only once")
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 90 or not math.isfinite(args.budget) or not 0 < args.budget <= 720:
        parser.error("timeout must be in (0,90], budget in (0,720], both finite")
    return args


def main(argv=None):
    try:
        report = run(parse_args(argv))
    except (OSError, ValueError, batch.BatchError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print("legacy Metal MRC: " + report["status"] + "; no Metal 4 task/production claim")
    return 0 if report["status"] == "passed" else 78 if report["status"].startswith("unsupported_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
