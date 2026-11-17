#!/usr/bin/env python3
"""Observe two authored NSNumber/NSString float macro expressions on legacy Metal.

The diagnostic token, NSNumber getter and GPU bits are different observations.
A detected difference is not a Skyrim bug or an automatic production fix.
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
import struct
import sys
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("macro_value_diagnostic_control", HERE / "macro_probe.py")
diagnostic = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(diagnostic)
control, batch, options = diagnostic.control, diagnostic.batch, diagnostic.options
SCOPE = "authored-two-float-macro-gpu-values"
MACROS = {"F32_NEXT": "1.00000011920928955078125f", "F32_NEG_ZERO": "-0.0f"}
INPUT_BITS = {"F32_NEXT": "3f800001", "F32_NEG_ZERO": "80000000"}
CONSTRUCTOR = "numberWithFloat from memcpy-reconstructed float32 bits"
FALSE_EVIDENCE = ("metal4_used", "production_parity_verified", "game_impact_verified", "iphone_gpu_verified",
                  "all_macro_types_equivalent_verified", "original_macro_type_recovery_verified",
                  "runtime_assignment_provenance_verified")
GPU_SOURCE = """// Authored two-value observation, no game source.
#include <metal_stdlib>
using namespace metal;
kernel void macro_values(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]]) {
    out[0] = as_type<uint>((float)F32_NEXT);
    out[1] = as_type<uint>((float)F32_NEG_ZERO);
    out[2] = as_type<uint>((float)(1.00000011920928955078125f));
    out[3] = as_type<uint>((float)(-0.0f));
    out[4] = in[0] ^ 0x2468ace0u;
    out[5] = 0x4d435632u;
}
"""
DIAGNOSTIC_SOURCE = """// Deliberate failure exposes token spelling; no GPU work in this source.
#define TOKEN_STRINGIFY_INNER(x) #x
#define TOKEN_STRINGIFY(x) TOKEN_STRINGIFY_INNER(x)
static_assert(false, "TOKEN_BEGIN_F32_NEXT__" TOKEN_STRINGIFY(F32_NEXT) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_F32_NEG_ZERO__" TOKEN_STRINGIFY(F32_NEG_ZERO) "__TOKEN_END");
"""
LINES = {"F32_NEXT": 4, "F32_NEG_ZERO": 5}
CONTROL_WORDS = [0x3f800001, 0x80000000, 0x13579bdf ^ 0x2468ace0, 0x4d435632, 0xcdcdcdcd, 0xcdcdcdcd]


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def validate_options(row):
    snapshot = row.get("effective_options", {})
    require(row.get("requested_options_getters_match") is True and snapshot.get("explicit_fixture_setters") == diagnostic.SETTERS
            and snapshot.get("fresh_unmodified_object") is False
            and snapshot.get("runtime_assignment_provenance_verified") is False, "option provenance differs")
    fields = snapshot.get("fields", {})
    for name in diagnostic.SETTERS:
        require(fields.get(name, {}).get("availability") == "supported"
                and fields[name].get("origin") == "explicit_fixture_assignment", "required option unavailable")
    require(type(fields["languageVersion"]["value"].get("raw")) is int
            and fields["languageVersion"]["value"]["raw"] == 0x30001
            and fields["mathMode"]["value"].get("known_name") == "safe"
            and fields["mathFloatingPointFunctions"]["value"].get("known_name") == "precise"
            and fields["preserveInvariance"]["value"] is False
            and fields["optimizationLevel"]["value"].get("is_default_enum") is True, "authored math profile differs")
    macros = fields["preprocessorMacros"]["value"]
    require(macros.get("status") == "captured" and macros.get("is_nil") is False, "macro getter missing")
    entries = macros.get("entries", [])
    require(type(entries) is list and [entry.get("name") for entry in entries] == sorted(MACROS), "macro getter names differ")
    getter_bits = {}
    for entry in entries:
        name, value = entry["name"], entry["value"]
        if row["case"] == "runtime_string":
            require(value == {"status": "captured", "kind": "NSString", "text": MACROS[name]}, "string getter differs")
        else:
            # A returned double representation is allowed. It is not evidence
            # that the NSNumber was originally constructed from a double.
            observed = options.numeric_value(value)
            require(value.get("category") == "binary_float", "float NSNumber representation missing")
            getter_bits[name] = struct.pack(">f", observed).hex()
            require(getter_bits[name] == INPUT_BITS[name], "NSNumber getter did not preserve the authored float input")
    getters = row.get("float_getters")
    if row["case"] == "runtime_number":
        require(getters == {"status": "captured", "method": "NSNumber.floatValue", "bits_hex": getter_bits}, "separate float getter bits differ")
    else:
        require(getters == {"status": "not_applicable_to_NSString"}, "string route must not invent a numeric getter")


def validate_native(report):
    require(type(report) is dict and type(report.get("schema")) is int and report["schema"] == 1
            and report.get("scope") == SCOPE, "native report identity differs")
    require(all(report.get(name) is False for name in FALSE_EVIDENCE), "unsupported evidence claim")
    require(report.get("status") == "authored_values_observed" and report.get("device_present") is True
            and report.get("authored_legacy_gpu_execution_verified") is True, "native observation incomplete")
    require(report.get("authored_float32_input_bits") == INPUT_BITS and report.get("requested_string_macros") == MACROS
            and report.get("number_constructor") == CONSTRUCTOR, "authored input declarations differ")
    rows = report.get("cases")
    require(type(rows) is list and len(rows) == 2 and all(type(row) is dict for row in rows)
            and [row.get("case") for row in rows] == ["runtime_number", "runtime_string"], "native routes differ")
    values, tokens = {}, {}
    for row in rows:
        require(row.get("status") == "readback_observed", "route did not finish")
        validate_options(row)
        rejection = row.get("token_diagnostic", {})
        error = rejection.get("error", {})
        require(rejection.get("library_created") is False and error.get("present") is True, "diagnostic rejection absent")
        if error.get("diagnostics_truncated") is not False:
            raise diagnostic.UnsupportedDiagnostics("diagnostic token evidence is truncated or absent")
        tokens[row["case"]] = diagnostic.parse_diagnostics(error.get("diagnostics"), LINES)
        require(row.get("value_library", {}).get("created") is True, "value library absent")
        gpu = row.get("gpu", {})
        require(gpu.get("pipeline_created") is True and gpu.get("pipeline_error") == {"present": False}
                and gpu.get("command_status") == "completed" and gpu.get("command_error") == {"present": False}, "GPU completion absent")
        words = gpu.get("words_u32")
        require(type(words) is list and len(words) == 8 and all(type(value) is int and 0 <= value <= 0xffffffff for value in words), "GPU words malformed")
        require(words[2:] == CONTROL_WORDS and gpu.get("authored_controls_match") is True
                and type(gpu.get("input_word")) is int and gpu["input_word"] == 0x13579bdf
                and type(gpu.get("dispatch_threads")) is int and gpu["dispatch_threads"] == 1
                and type(gpu.get("initial_output_word")) is int and gpu["initial_output_word"] == 0xcdcdcdcd,
                "literal, input, dispatch or guard control differs")
        bits = {name: f"{words[index]:08x}" for index, name in enumerate(("F32_NEXT", "F32_NEG_ZERO"))}
        require(gpu.get("macro_bits_hex") == bits, "readback words and macro bits disagree")
        values[row["case"]] = bits
    return {"observed_tokens": tokens, "observed_gpu_bits": values,
            "number_vs_string_token_equal": {name: tokens["runtime_number"][name] == tokens["runtime_string"][name] for name in MACROS},
            "number_vs_string_gpu_bits_equal": {name: values["runtime_number"][name] == values["runtime_string"][name] for name in MACROS},
            "matches_authored_float_input": {route: {name: bits[name] == INPUT_BITS[name] for name in MACROS} for route, bits in values.items()}}


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
              "budget_seconds": args.budget,
              "budget_scope": "Tool admission/waits are bounded; process startup, cleanup, reporting and external compiler services can extend elapsed time."}
    write_report(output, report)
    try:
        for name in ("macro_value.py", "macro_value.mm", "options_snapshot.h", "macro_probe.py", "options_probe.py", "offline.py", "batch.py"):
            data = (HERE / name).read_bytes()
            (output / name).write_bytes(data)
            report["inputs"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, text in (("authored-macro-value.metal", GPU_SOURCE), ("authored-macro-diagnostic.metal", DIAGNOSTIC_SOURCE)):
            data = text.encode("utf-8")
            (output / name).write_bytes(data)
            report["inputs"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if platform.system() != "Darwin":
            report.update(status="unsupported_host", reason="This isolated GPU probe requires macOS with a legacy Metal device; Metal 4 is not required")
            return report
        env = dict(os.environ)
        for name in control.ENV_OPTIONS:
            env.pop(name, None)
        def command(argv, label, required=True):
            require(time.monotonic() < deadline, "probe budget exhausted before " + label)
            event, stdout, stderr = control.run_command(argv, label, output, report, env, args.timeout, deadline=deadline)
            if required:
                control.require_success(event)
            return event, stdout, stderr
        xcrun = "/usr/bin/xcrun"
        _, version, _ = command([xcrun, "--sdk", "macosx", "--show-sdk-version"], "sdk-version")
        if not re.fullmatch(r"(?:1[5-9]|[2-9][0-9])\.[0-9]+(?:\.[0-9]+)?", version.strip()):
            report.update(status="unsupported_sdk", reason="macOS SDK 15 or later is required")
            return report
        _, path, _ = command([xcrun, "--sdk", "macosx", "--show-sdk-path"], "sdk-path")
        sdk = Path(path.strip())
        require(sdk.is_absolute() and sdk.is_dir(), "SDK path is unavailable")
        report["sdk"] = {"version": version.strip(), "path": str(sdk)}
        command([xcrun, "--sdk", "macosx", "clang++", "--version"], "clangxx-version")
        binary = output / "macro-value"
        # Keep the clang++ driver spelling. Resolving its symlink to clang and
        # invoking that path can omit the C++ runtime at link time.
        command([xcrun, "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g", "-fobjc-arc",
                 "-mmacosx-version-min=13.3", "-isysroot", sdk, "-Wall", "-Wextra", "-Werror",
                 "-framework", "Foundation", "-framework", "Metal", "-framework", "CoreFoundation",
                 output / "macro_value.mm", "-o", binary], "native-compile")
        control.validate_artifact(binary)
        report["native_binary"] = control.file_info(binary)
        event, _, _ = command([binary, "--gpu-source", output / "authored-macro-value.metal",
                               "--diagnostic-source", output / "authored-macro-diagnostic.metal",
                               "--report", output / "native.json"], "native-observe", False)
        native, _ = batch.read_json(output / "native.json")
        report["native"] = native
        if event.get("returncode") == 78 and not event.get("timed_out") and not event.get("interrupted") \
                and native.get("status") == "unsupported":
            require(type(native.get("schema")) is int and native["schema"] == 1 and native.get("scope") == SCOPE
                    and all(native.get(name) is False for name in FALSE_EVIDENCE)
                    and native.get("authored_legacy_gpu_execution_verified") is False and native.get("cases") == []
                    and type(native.get("reason")) is str and 0 < len(native["reason"]) <= 1024,
                    "unsupported result promoted evidence or has an invalid boundary")
            report.update(status="unsupported_native", reason=native.get("reason"))
            return report
        control.require_success(event)
        report.update(validate_native(native))
        report.update(status="authored_values_observed", authored_legacy_gpu_execution_verified=True)
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", reason=type(error).__name__)
        raise
    except diagnostic.UnsupportedDiagnostics as error:
        report.update(status="unsupported_diagnostic_format", reason=str(error))
    except Exception as error:
        report.update(status="failed", reason=type(error).__name__ + ": " + str(error))
    finally:
        report["elapsed_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--budget", type=float, default=90)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 90 or not math.isfinite(args.budget) or not 0 < args.budget <= 720:
        parser.error("timeout must be in (0,90] and budget in (0,720], both finite seconds")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        report = run(args)
    except (OSError, ValueError, batch.BatchError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print("macro value observation: " + report["status"])
    return 0 if report["status"] == "authored_values_observed" else 78 if report["status"].startswith("unsupported_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
