#!/usr/bin/env python3
"""Bounded authored Foundation/MRC fixture; no Metal device, app, or game inputs."""
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


control = load("mrc_lifetime_control", "offline.py")
batch = load("mrc_lifetime_io", "batch.py")
SCOPE = "authored-foundation-mrc-owner-retirement"
FALSE_EVIDENCE = ("metal_api_verified", "metal_task_semantics_verified", "gpu_execution_verified", "production_adapter_verified")
CASES = ("early_success", "late_after_destroy_request", "early_warning", "nil_before_callback",
         "throw_before_callback", "nil_after_success", "throw_after_success")
VARIANTS = {"normal": [], "asan-ubsan": ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
            "tsan": ["-fsanitize=thread", "-fno-omit-frame-pointer"]}


def require(value, message):
    if not value:
        raise ValueError(message)


def validate_native(native):
    require(type(native) is dict and type(native.get("schema")) is int and native["schema"] == 1
            and native.get("scope") == SCOPE and native.get("status") == "passed", "native fixture did not pass")
    require(native.get("manual_reference_counting") is True and native.get("foundation_ownership_verified") is True,
            "MRC/Foundation verification missing")
    for name in FALSE_EVIDENCE:
        require(native.get(name) is False, "unsupported evidence claim: " + name)
    cases = native.get("cases")
    require(type(cases) is list and len(cases) == len(CASES), "incomplete authored cases")
    for row, name in zip(cases, CASES):
        require(type(row) is dict and row.get("case") == name, "authored case order or identity changed")
        failure = name.startswith(("nil_", "throw_"))
        late = name == "late_after_destroy_request"
        after = name.endswith("after_success")
        code = 7 if late or name == "early_warning" else (12 if name.startswith("throw_") else 11) if failure and not after else 0
        expected = {"notifications": 1, "retirements": 1, "callback_attempts": 2 if after else 1,
                    "faults": int(failure), "error_code": code}
        for key, value in expected.items():
            require(type(row.get(key)) is int and row[key] == value, "count/payload mismatch: " + name + "/" + key)
        expected_flags = {"snapshot_survived_caller_pool": True, "first_payload_preserved": True,
                          "notification_before_launch_exit": not late, "retirement_after_launch_exit": True,
                          "retirement_inside_notification": False, "timeout_did_not_cancel": late,
                          "objc_exception_observed": name.startswith("throw_"), "tracked_objects_balanced": True}
        for key, value in expected_flags.items():
            require(row.get(key) is value, "ownership/order mismatch: " + name + "/" + key)
        objects = row.get("objects_created")
        require(type(objects) is dict and set(objects) == {"owner", "task", "token"}
                and all(type(v) is int for v in objects.values())
                and objects == {"owner": 1, "task": 0 if failure else 1, "token": 2}, "tracked object coverage changed")


def write_report(output, report):
    pending = output / "result.tmp"
    pending.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    pending.replace(output / "result.json")


def run(args):
    output = batch.fresh_directory(args.output)
    started = time.monotonic()
    deadline = started + args.budget
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [], "inputs": {}, "variants": [],
              "foundation_ownership_verified": False, **{key: False for key in FALSE_EVIDENCE},
              "budget_seconds": args.budget, "requested_variants": args.variants,
              "snapshot_scope": "Authored NSString/NSDictionary copies only; no MTLCompileOptions copy/production parity claim.",
              "tracked_object_scope": "LifetimeOwner/LifetimeTask/LifetimeToken only; not a Foundation-wide leak proof",
              "budget_scope": "Absolute tool admission/wait budget; process cleanup/reporting can extend wall time."}
    write_report(output, report)
    try:
        for filename in ("mrc_lifetime.mm", "mrc_lifetime.py", "owner_retirement.h", "offline.py", "batch.py"):
            data = (HERE / filename).read_bytes()
            (output / filename).write_bytes(data)
            report["inputs"][filename] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if platform.system() != "Darwin":
            report.update(status="unsupported_host", reason="The MRC/Foundation fixture requires macOS; no Metal device is required")
            return report
        env = dict(os.environ)
        for key in control.ENV_OPTIONS:
            env.pop(key, None)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        env["TSAN_OPTIONS"] = "halt_on_error=1"

        def command(argv, label):
            require(time.monotonic() < deadline, "fixture budget exhausted before " + label)
            event, stdout, stderr = control.run_command(argv, label, output, report, env, args.timeout, deadline=deadline)
            control.require_success(event)
            return stdout, stderr

        xcrun = "/usr/bin/xcrun"
        version, _ = command([xcrun, "--sdk", "macosx", "--show-sdk-version"], "sdk-version")
        sdk_text, _ = command([xcrun, "--sdk", "macosx", "--show-sdk-path"], "sdk-path")
        sdk = Path(sdk_text.strip())
        require(sdk.is_absolute() and sdk.is_dir(), "SDK path unavailable")
        report["sdk"] = {"version": version.strip(), "path": str(sdk)}
        command([xcrun, "--sdk", "macosx", "clang++", "--version"], "clangxx-version")
        for variant in args.variants:
            row = {"variant": variant, "status": "running", "native_compilation_verified": False,
                   "foundation_ownership_verified": False}
            report["variants"].append(row)
            try:
                binary = output / ("mrc-lifetime-" + variant)
                command([xcrun, "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
                         "-fno-objc-arc", "-fblocks", "-fexceptions", "-fobjc-exceptions", "-pthread",
                         "-Wall", "-Wextra", "-Werror", "-mmacosx-version-min=13.3", "-isysroot", sdk,
                         *VARIANTS[variant], "-framework", "Foundation", output / "mrc_lifetime.mm", "-o", binary],
                        variant + "-compile")
                control.validate_artifact(binary)
                row.update(native_compilation_verified=True, binary=control.file_info(binary))
                native_path = output / (variant + "-native.json")
                # Inspect process failure first; a timeout need not create JSON.
                _, runtime_stderr = command([binary, "--report", native_path], variant + "-run")
                require(not runtime_stderr.strip(), "native runtime emitted diagnostics; inspect retained stderr")
                native, _ = batch.read_json(native_path)
                row["native"] = native
                validate_native(native)
                row.update(status="passed", foundation_ownership_verified=True)
            except (KeyboardInterrupt, SystemExit):
                row["status"] = "interrupted"
                raise
            except Exception as error:
                row.update(status="failed", reason=type(error).__name__ + ": " + str(error))
                raise
        report.update(status="passed", foundation_ownership_verified=True)
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
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=["normal", "asan-ubsan", "tsan"])
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
    print("Foundation MRC lifetime: " + report["status"] + "; no Metal task/production claim")
    return 0 if report["status"] == "passed" else 78 if report["status"] == "unsupported_host" else 1


if __name__ == "__main__":
    raise SystemExit(main())
