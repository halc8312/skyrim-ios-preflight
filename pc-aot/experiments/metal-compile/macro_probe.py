#!/usr/bin/env python3
"""Observe authored macro tokens through deliberate, strictly parsed diagnostics.

No successful shader compilation, expression type/value equivalence, production
parity, GPU execution or Metal 4 support is established by this experiment.
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
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value

control = module("macro_offline_control", HERE / "offline.py")
batch = module("macro_batch_control", HERE / "batch.py")
options = module("macro_options_validation", HERE / "options_probe.py")
SCOPE = "authored-static-assert-token-diagnostics"
TOKENS = {"U64_MAX": "18446744073709551615ull", "U64_HIGH": "9223372036854775808ull",
          "I64_MIN": "-9223372036854775808ll", "U32_MAX": "4294967295u", "F32_NEG_ZERO": "-0.0f",
          "F32_NEXT": "1.00000011920928955078125f", "BOOL_VALUE": "true", "TOKEN_STRING": "0x1234u"}
PROFILE = {"target": "macos", "deployment": "15.0", "language": "3.1", "math_mode": "safe",
           "math_functions": "precise", "preserve_invariance": False, "optimization": "default", "macros": TOKENS}
FALSE_EVIDENCE = ("shader_compilation_success_verified", "gpu_execution_verified", "metal4_used",
                  "production_parity_verified", "typed_value_equivalence_verified",
                  "original_macro_type_recovery_verified", "runtime_assignment_provenance_verified")
SETTERS = ["languageVersion", "mathMode", "mathFloatingPointFunctions", "preserveInvariance", "optimizationLevel", "preprocessorMacros"]

class UnsupportedDiagnostics(ValueError):
    pass

def require(condition, reason):
    if not condition:
        raise ValueError(reason)

def source_lines(source):
    expected = {}
    for line, text in enumerate(source.splitlines(), 1):
        match = re.fullmatch(r'static_assert\(false, "TOKEN_BEGIN_([A-Z0-9_]+)__" TOKEN_STRINGIFY\(\1\) "__TOKEN_END"\);', text)
        if match:
            name = match[1]
            require(name not in expected, "duplicate authored assertion")
            expected[name] = line
    require(set(expected) == set(TOKENS), "authored assertion set differs")
    return expected

def parse_diagnostics(text, expected_lines, source_path=None):
    """Only the actual observed Apple diagnostic format is accepted, fail closed.

    Source echoes lack the path:line:column:error prefix and cannot supply tokens.
    Different compiler/OS formatting, extra errors or duplicate markers are not
    guessed at; raw evidence remains available with unsupported status.
    """
    if not isinstance(text, str) or len(text.encode("utf-8")) > 65536:
        raise UnsupportedDiagnostics("missing or oversized diagnostic text")
    observed = {}
    for line in text.splitlines():
        if not re.search(r"\berror:", line):
            continue
        entry = re.fullmatch(r'(.+):(\d+):(\d+): error: static_assert failed "TOKEN_BEGIN_([A-Z0-9_]+)__(.*)__TOKEN_END"', line)
        if not entry:
            raise UnsupportedDiagnostics("unrecognized or additional compiler error")
        path, number, column, name, token = entry.groups()
        # Windows Metal 6.2 doubles backslashes in diagnostic paths. Accept
        # only spellings of the exact submitted path, never a basename match.
        spellings = {"program_source"}
        if source_path is not None:
            exact = str(source_path)
            spellings.update((exact, exact.replace("\\", "\\\\"), exact.replace("\\", "/")))
        accepted_path = path in spellings
        if not accepted_path or name not in expected_lines or int(number) != expected_lines[name] or int(column) != 1:
            raise UnsupportedDiagnostics("diagnostic source/line/case differs")
        if name in observed or not re.fullmatch(r"[A-Za-z0-9+_.() -]{1,128}", token) or "TOKEN_" in token:
            raise UnsupportedDiagnostics("duplicate, ambiguous or unsupported token")
        observed[name] = token
    if set(observed) != set(expected_lines):
        raise UnsupportedDiagnostics("not all authored tokens were observed")
    return observed

def declarations():
    result = {}
    for name, factory, width, bits in (
        ("U64_MAX", "numberWithUnsignedLongLong", 64, "ffffffffffffffff"),
        ("U64_HIGH", "numberWithUnsignedLongLong", 64, "8000000000000000"),
        ("I64_MIN", "numberWithLongLong", 64, "8000000000000000"),
        ("U32_MAX", "numberWithUnsignedInt", 32, "ffffffff"),
        ("F32_NEG_ZERO", "numberWithFloat", 32, "80000000"),
        ("F32_NEXT", "numberWithFloat", 32, "3f800001")):
        result[name] = {"factory": factory, "input_width_bits": width, "input_bits_hex": bits}
    return {**result, "BOOL_VALUE": {"factory": "boolean", "input_value": True},
            "TOKEN_STRING": {"factory": "NSString", "input_text": "0x1234u"}}

def validate_native(report, lines):
    require(type(report) is dict and report.get("schema") == 1 and report.get("scope") == SCOPE, "native report identity differs")
    require(report.get("expected_compilation_failure") is True and all(report.get(k) is False for k in FALSE_EVIDENCE), "unsupported evidence claim")
    require(report.get("requested_string_macros") == TOKENS and batch.canonical(report.get("authored_constructor_arguments")) == batch.canonical(declarations()), "authored declarations differ")
    require(report.get("status") == "deliberate_rejections_observed" and report.get("device_present") is True, "native observation incomplete")
    rows = report.get("cases")
    require(type(rows) is list and [row.get("case") for row in rows] == ["runtime_number", "runtime_string"], "native cases differ")
    captured = {}
    for row in rows:
        require(row.get("status") == "deliberately_rejected" and row.get("library_created") is False
                and row.get("error_present") is True and row.get("requested_options_getters_match") is True, "expected native rejection missing")
        require(row.get("diagnostics_truncated") is False, "truncated native diagnostics")
        snapshot = row.get("effective_options", {})
        require(snapshot.get("explicit_fixture_setters") == SETTERS and snapshot.get("fresh_unmodified_object") is False
                and snapshot.get("runtime_assignment_provenance_verified") is False, "option provenance differs")
        fields = snapshot.get("fields", {})
        for field in SETTERS:
            require(fields.get(field, {}).get("availability") == "supported"
                    and fields[field].get("origin") == "explicit_fixture_assignment", "required option unavailable")
        require(fields["mathMode"]["value"].get("known_name") == "safe"
                and fields["mathFloatingPointFunctions"]["value"].get("known_name") == "precise"
                and fields["preserveInvariance"]["value"] is False
                and fields["optimizationLevel"]["value"].get("is_default_enum") is True, "authored math profile differs")
        macros = fields["preprocessorMacros"]["value"]
        require(macros.get("status") == "captured" and macros.get("is_nil") is False, "macro getter snapshot incomplete")
        entries = macros.get("entries", [])
        require([entry.get("name") for entry in entries] == sorted(TOKENS), "macro getter set differs")
        for entry in entries:
            value = entry["value"]
            if row["case"] == "runtime_string" or entry["name"] == "TOKEN_STRING":
                require(value == {"status": "captured", "kind": "NSString", "text": TOKENS[entry["name"]]}, "string macro getter differs")
            else:
                options.numeric_value(value) # Validate observed representation, not original declaration width.
        captured[row["case"]] = parse_diagnostics(row.get("diagnostics"), lines)
    return captured

def write_report(output, report):
    # Each fresh run owns this temporary path. Preserve the last complete JSON
    # if interruption occurs while writing or before publishing the replacement.
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")

def run(args):
    output = batch.fresh_directory(args.output)
    started = time.monotonic()
    deadline = started + args.budget
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [], "inputs": {},
              "profile": PROFILE, "expected_compilation_failure": True,
              **{name: False for name in FALSE_EVIDENCE}, "native_tokens_observed": False,
              "offline_tokens_observed": False, "budget_seconds": args.budget,
              "budget_scope": "Tool admission/waits are bounded; startup, cleanup, reporting and external compiler services are not hard-deadline bounded."}
    write_report(output, report)
    try:
        for name in ("macro_probe.py", "macro_probe.mm", "macro_diagnostics.metal", "options_snapshot.h", "options_probe.py", "offline.py", "batch.py"):
            data = (HERE / name).read_bytes()
            (output / name).write_bytes(data)
            report["inputs"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        system = platform.system()
        require(system in ("Windows", "Darwin"), "This experiment requires installed Apple Windows tools or macOS")
        source = output / "macro_diagnostics.metal"
        lines = source_lines(source.read_text(encoding="utf-8"))
        env = dict(os.environ)
        for key in ("SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET"):
            env.pop(key, None)
        def command(argv, label, success=True):
            require(time.monotonic() < deadline, "probe budget exhausted before " + label)
            event, stdout, stderr = control.run_command(argv, label, output, report, env, args.timeout, deadline=deadline)
            if success:
                control.require_success(event)
            return event, stdout, stderr
        tools, sdk_path = control.resolve_tools(args.tools, "macos", system, output, report, env, args.timeout, deadline)
        report["tools"] = {name: control.file_info(path) for name, path in tools.items()}
        command([tools["metal"], "--version"], "metal-version")
        rejected_air = output / "deliberately-rejected.air"
        event, _, _ = command(batch.compile_command(tools["metal"], source, rejected_air, PROFILE, sdk_path), "deliberate-rejection", False)
        require(event.get("returncode") == 1 and not event.get("timed_out") and not event.get("interrupted") and not rejected_air.exists(), "expected offline rejection did not occur")
        diagnostics = batch.read_bounded(output / event["stderr"], 65536).decode("utf-8")
        offline_tokens = parse_diagnostics(diagnostics, lines, source)
        require(offline_tokens == TOKENS, "offline tokens differ from authored -D arguments")
        report.update(offline_tokens_observed=True, offline_tokens=offline_tokens, status="offline_tokens_observed")
        write_report(output, report)
        if system == "Darwin":
            _, sdk_version, _ = command(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-version"], "sdk-version")
            if not re.fullmatch(r"(?:1[5-9]|[2-9][0-9])\.[0-9]+(?:\.[0-9]+)?", sdk_version.strip()):
                report.update(status="unsupported_native", reason="macOS SDK 15 or later is required")
                return report
            binary = output / "macro-probe"
            command(["/usr/bin/xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g", "-fobjc-arc",
                     "-mmacosx-version-min=13.3", "-isysroot", sdk_path, "-Wall", "-Wextra", "-Werror",
                     "-framework", "Foundation", "-framework", "Metal", "-framework", "CoreFoundation",
                     output / "macro_probe.mm", "-o", binary], "native-compile")
            report["native_binary"] = control.file_info(binary)
            event, _, _ = command([binary, "--source", source, "--report", output / "native.json"], "native-observe", False)
            native, _ = batch.read_json(output / "native.json")
            report["native"] = native
            if event.get("returncode") == 78 and not event.get("timed_out") and native.get("status") == "unsupported":
                report.update(status="unsupported_native", reason=native.get("reason"))
                return report
            control.require_success(event)
            captured = validate_native(native, lines)
            report.update(status="diagnostic_tokens_observed", native_tokens_observed=True, observed_tokens=captured,
                          number_vs_string_token_equal={key: captured["runtime_number"][key] == captured["runtime_string"][key] for key in TOKENS},
                          string_vs_cli_token_equal={key: captured["runtime_string"][key] == offline_tokens[key] for key in TOKENS})
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", reason=type(error).__name__)
        raise
    except UnsupportedDiagnostics as error:
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
    parser.add_argument("--tools", type=Path)
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
    print("macro diagnostic observation: " + report["status"])
    if report["status"] in ("diagnostic_tokens_observed", "offline_tokens_observed"):
        return 0
    return 78 if report["status"].startswith("unsupported_") else 1

if __name__ == "__main__":
    raise SystemExit(main())
