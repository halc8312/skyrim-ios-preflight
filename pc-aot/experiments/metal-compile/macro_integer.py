#!/usr/bin/env python3
"""Observe one authored integer macro through three legacy Metal API inputs.

The unsuffixed string is a fixed, previously observed token, not a claim about
the current NSNumber expansion. GPU expression observations are separate from
explicit uint controls. No game input, Metal 4, or production parity is tested.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import re
import sys
import time

HERE = Path(__file__).resolve().parent


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


control = module("macro_integer_process_control", "offline.py")
batch = module("macro_integer_file_control", "batch.py")
options = module("macro_integer_options_validation", "options_probe.py")
SCOPE = "authored-u32-macro-gpu-expressions"
CASES = ["runtime_number", "runtime_token_string", "runtime_uint_string"]
STRINGS = {"runtime_token_string": "4294967295", "runtime_uint_string": "4294967295u"}
DECLARATION = {"factory": "numberWithUnsignedInt", "input_width_bits": 32, "input_bits_hex": "ffffffff"}
SETTERS = ["languageVersion", "mathMode", "mathFloatingPointFunctions",
           "preserveInvariance", "optimizationLevel", "preprocessorMacros"]
FALSE_EVIDENCE = ("metal4_used", "production_parity_verified", "game_impact_verified", "iphone_gpu_verified",
                  "all_macro_types_equivalent_verified", "original_macro_type_recovery_verified",
                  "runtime_assignment_provenance_verified", "runtime_macro_token_capture_verified")
UINT_EXPECTATION = [0xffffffff, 4, 0]
CONTROL_WORDS = UINT_EXPECTATION + UINT_EXPECTATION + [0x13579bdf ^ 0x2468ace0, 0x4d434933] + [0xcdcdcdcd] * 5
GPU_SOURCE = """// Authored integer expression observation. No game source.
#include <metal_stdlib>
using namespace metal;
kernel void macro_integer_values(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]]) {
    out[0] = uint(U32_MAX);
    out[1] = uint(sizeof(U32_MAX));
    out[2] = uint(-1 < U32_MAX);
    out[3] = 4294967295u;
    out[4] = uint(sizeof(4294967295u));
    out[5] = uint(-1 < 4294967295u);
    out[6] = in[0];
    out[7] = uint(sizeof(in[0]));
    out[8] = uint(-1 < in[0]);
    out[9] = in[1] ^ 0x2468ace0u;
    out[10] = 0x4d434933u;
    // out[11:16] retain the host sentinel as unwritten guards.
}
"""


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def validate_options(row):
    snapshot = row.get("effective_options", {})
    require(snapshot.get("explicit_fixture_setters") == SETTERS
            and snapshot.get("fresh_unmodified_object") is False
            and snapshot.get("runtime_assignment_provenance_verified") is False, "option provenance differs")
    fields = snapshot.get("fields", {})
    for name in SETTERS:
        require(fields.get(name, {}).get("availability") == "supported"
                and fields[name].get("origin") == "explicit_fixture_assignment", "required option unavailable")
    require(type(fields["languageVersion"]["value"].get("raw")) is int
            and fields["languageVersion"]["value"]["raw"] == 0x30001
            and fields["mathMode"]["value"].get("known_name") == "safe"
            and fields["mathFloatingPointFunctions"]["value"].get("known_name") == "precise"
            and fields["preserveInvariance"]["value"] is False
            and fields["optimizationLevel"]["value"].get("is_default_enum") is True, "authored profile differs")
    macros = fields["preprocessorMacros"]["value"]
    require(macros.get("status") == "captured" and macros.get("is_nil") is False, "macro getter missing")
    entries = macros.get("entries")
    require(type(entries) is list and len(entries) == 1 and entries[0].get("name") == "U32_MAX", "macro getter names differ")
    value = entries[0]["value"]
    if row["case"] == "runtime_number":
        require(options.numeric_value(value) == 0xffffffff
                and value.get("category") in ("signed_integer", "unsigned_integer"), "NSNumber getter changed authored input")
        require(row.get("integer_getter") == {"method": "NSNumber.unsignedIntValue", "bits_hex": "ffffffff"},
                "unsignedIntValue getter differs")
    else:
        require(value == {"status": "captured", "kind": "NSString", "text": STRINGS[row["case"]]}, "string getter differs")
        require(row.get("integer_getter") == {"status": "not_applicable_to_NSString"}, "string route invented a numeric getter")


def validate_native(report):
    require(type(report) is dict and type(report.get("schema")) is int and report["schema"] == 1
            and report.get("scope") == SCOPE, "native report identity differs")
    require(all(report.get(name) is False for name in FALSE_EVIDENCE), "unsupported evidence claim")
    require(report.get("status") == "authored_expressions_observed" and report.get("device_present") is True
            and report.get("authored_legacy_gpu_execution_verified") is True, "native observation incomplete")
    require(report.get("number_constructor") == DECLARATION and report.get("requested_string_macros") == STRINGS,
            "authored input declarations differ")
    rows = report.get("cases")
    require(type(rows) is list and len(rows) == 3 and all(type(row) is dict for row in rows)
            and [row.get("case") for row in rows] == CASES, "native routes differ")
    observations = {}
    for row in rows:
        require(row.get("status") == "readback_observed", "route did not finish")
        validate_options(row)
        library = row.get("value_library", {})
        require(library.get("created") is True and type(library.get("error")) is dict
                and type(library["error"].get("present")) is bool, "value library observation absent")
        # A nonnil library may include warnings. Do not turn NSError alone into rejection.
        gpu = row.get("gpu", {})
        require(gpu.get("pipeline_created") is True and gpu.get("pipeline_error") == {"present": False}
                and gpu.get("command_status") == "completed" and gpu.get("command_error") == {"present": False},
                "GPU completion absent")
        words = gpu.get("words_u32")
        require(type(words) is list and len(words) == 16
                and all(type(value) is int and 0 <= value <= 0xffffffff for value in words), "GPU words malformed")
        require(words[3:] == CONTROL_WORDS and gpu.get("authored_controls_match") is True
                and gpu.get("input_words") == [0xffffffff, 0x13579bdf]
                and all(type(value) is int for value in gpu["input_words"])
                and type(gpu.get("dispatch_threads")) is int and gpu["dispatch_threads"] == 1
                and type(gpu.get("initial_output_word")) is int and gpu["initial_output_word"] == 0xcdcdcdcd,
                "literal, input, dispatch or guard control differs")
        if row["case"] == "runtime_uint_string":
            require(words[:3] == UINT_EXPECTATION, "explicit uint macro expectation failed")
        observations[row["case"]] = dict(zip(("uint_value", "sizeof_bytes", "minus_one_less_than_value"), words[:3]))
    return {"observed_expressions": observations,
            "explicit_uint_expectation": dict(zip(("uint_value", "sizeof_bytes", "minus_one_less_than_value"), UINT_EXPECTATION)),
            "matches_explicit_uint_expectation": {name: list(values.values()) == UINT_EXPECTATION for name, values in observations.items()},
            "number_vs_token_string_expressions_equal": observations[CASES[0]] == observations[CASES[1]],
            "comparison_scope": "These three expressions only; no inferred original type, current macro token, general equivalence, or production fix."}


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def run(args):
    output = batch.fresh_directory(args.output)
    started = time.monotonic()
    deadline = started + args.budget
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [], "inputs": {},
              **{name: False for name in FALSE_EVIDENCE}, "authored_legacy_gpu_execution_verified": False,
              "budget_seconds": args.budget, "command_timeout_cap_seconds": args.timeout,
              "budget_scope": "Tool admission/waits share a deadline; input I/O, startup, cleanup, reporting and external compiler services are not hard-deadline bounded."}
    native_path = output / "native.json"
    write_report(output, report)
    try:
        for name in ("macro_integer.py", "macro_integer.mm", "options_snapshot.h", "options_probe.py", "offline.py", "batch.py"):
            data = (HERE / name).read_bytes()
            (output / name).write_bytes(data)
            report["inputs"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        data = GPU_SOURCE.encode("utf-8")
        source = output / "authored-macro-integer.metal"
        source.write_bytes(data)
        report["inputs"][source.name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if platform.system() != "Darwin":
            report.update(status="unsupported_host", reason="macOS with a legacy Metal device is required; Metal 4 is unnecessary")
            return report
        env, report["removed_environment_options"] = control.clean_environment()

        def command(argv, label, required=True):
            report["active_command"] = label
            write_report(output, report)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise control.BudgetExhausted("budget exhausted before " + label)
            event, stdout, _ = control.run_command(argv, label, output, report, env, min(args.timeout, remaining), deadline=deadline)
            if required:
                control.require_success(event)
            return event, stdout

        xcrun = "/usr/bin/xcrun"
        _, version = command([xcrun, "--sdk", "macosx", "--show-sdk-version"], "sdk-version")
        if not re.fullmatch(r"(?:1[5-9]|[2-9][0-9])\.[0-9]+(?:\.[0-9]+)?", version.strip()):
            report.update(status="unsupported_sdk", reason="macOS SDK 15 or later is required")
            return report
        _, path = command([xcrun, "--sdk", "macosx", "--show-sdk-path"], "sdk-path")
        sdk = Path(path.strip())
        require(sdk.is_absolute() and sdk.is_dir(), "SDK path is unavailable")
        report["sdk"] = {"version": version.strip(), "path": str(sdk)}
        command([xcrun, "--sdk", "macosx", "clang++", "--version"], "clangxx-version")
        binary = output / "macro-integer"
        command([xcrun, "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g", "-fobjc-arc",
                 "-mmacosx-version-min=15.0", "-isysroot", sdk, "-Wall", "-Wextra", "-Werror",
                 "-framework", "Foundation", "-framework", "Metal", "-framework", "CoreFoundation",
                 output / "macro_integer.mm", "-o", binary], "native-compile")
        control.validate_artifact(binary)
        report["native_binary"] = control.file_info(binary)
        event, _ = command([binary, "--gpu-source", source, "--report", native_path], "native-observe", False)
        native, _ = batch.read_json(native_path)
        report["native"] = native
        if event.get("returncode") == 78 and not event.get("timed_out") and not event.get("interrupted"):
            require(type(native) is dict and type(native.get("schema")) is int and native["schema"] == 1
                    and native.get("scope") == SCOPE and native.get("status") == "unsupported"
                    and all(native.get(name) is False for name in FALSE_EVIDENCE)
                    and native.get("authored_legacy_gpu_execution_verified") is False and native.get("cases") == []
                    and type(native.get("reason")) is str and 0 < len(native["reason"]) <= 1024,
                    "invalid unsupported evidence")
            report.update(status="unsupported_native", reason=native["reason"])
            return report
        control.require_success(event)
        report.update(validate_native(native))
        report.update(status="authored_expressions_observed", authored_legacy_gpu_execution_verified=True)
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", reason=type(error).__name__)
        raise
    except Exception as error:
        report.update(status="failed", reason=type(error).__name__ + ": " + str(error))
    finally:
        if "native" not in report and native_path.is_file():
            try:
                report["native"], _ = batch.read_json(native_path)
            except (OSError, ValueError, batch.BatchError) as error:
                report["native_report_error"] = str(error)
        report.pop("active_command", None)
        report["elapsed_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--budget", type=float, default=90)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 60 or not math.isfinite(args.budget) or not 0 < args.budget <= 180:
        parser.error("timeout must be in (0,60] and budget in (0,180], both finite seconds")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        report = run(args)
    except (OSError, ValueError, batch.BatchError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print("macro integer observation: " + report["status"])
    return 0 if report["status"] == "authored_expressions_observed" else 78 if report["status"].startswith("unsupported_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
