#!/usr/bin/env python3
"""Build/run the isolated Mac MTLCompileOptions getter probe; no GPU required."""
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
SPEC = importlib.util.spec_from_file_location("options_probe_process_control", HERE / "offline.py")
control = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(control)
SCOPE = "authored-mtlcompileoptions-getters"
FALSE_EVIDENCE = ("gpu_execution_verified", "shader_compilation_verified", "source_included", "production_parity_verified",
                  "batch_profile_parity_verified", "runtime_assignment_provenance_verified", "original_macro_type_recovery_verified",
                  "macro_preprocessor_text_equivalence_verified")
FIELDS = {"languageVersion", "fastMathEnabled", "preserveInvariance", "optimizationLevel", "preprocessorMacros", "libraryType",
          "compileSymbolVisibility", "allowReferencingUndefinedSymbols", "maxTotalThreadsPerThreadgroup", "installName", "libraries",
          "mathMode", "mathFloatingPointFunctions", "enableLogging", "requiredThreadsPerThreadgroup", "floatingPointConversionRoundingMode"}
LATE_FIELDS = {"mathMode": (150000, (15, 0)), "mathFloatingPointFunctions": (150000, (15, 0)),
               "enableLogging": (150000, (15, 0)), "requiredThreadsPerThreadgroup": (260000, (26, 0)),
               "floatingPointConversionRoundingMode": (270000, (27, 0))}
MODERN_SETTERS = ["languageVersion", "mathMode", "mathFloatingPointFunctions", "preserveInvariance", "optimizationLevel"]


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def uint(value, maximum=(1 << 64) - 1):
    return type(value) is int and 0 <= value <= maximum


def expected_macros():
    values = {"I8": ("signed_integer", 8, -7), "I16": ("signed_integer", 16, -300),
              "I32": ("signed_integer", 32, -70000), "I64_MIN": ("signed_integer", 64, -(1 << 63)),
              "U8": ("unsigned_integer", 8, 255), "U16": ("unsigned_integer", 16, 65535),
              "U32": ("unsigned_integer", 32, (1 << 32) - 1), "U64_MAX": ("unsigned_integer", 64, (1 << 64) - 1),
              "F32": ("binary_float", 32, 1.25), "F32_NEG_ZERO": ("binary_float", 32, -0.0),
              "F64": ("binary_float", 64, 1.25), "F64_NEG_ZERO": ("binary_float", 64, -0.0)}
    declarations = {}
    for name, (kind, width, value) in values.items():
        bits = struct.pack(">f" if width == 32 else ">d", value).hex() if kind == "binary_float" \
            else f"{value & ((1 << width) - 1):0{width // 4}x}"
        declarations[name] = {"factory_kind": kind, "declared_width_bits": width, "declared_bits_hex": bits}
    declarations["BOOL"] = {"factory_kind": "boolean", "value": True}
    declarations["TOKEN_STRING"] = {"factory_kind": "NSString", "text": "0x1234u"}
    return declarations


def numeric_value(value):
    require(type(value) is dict and value.get("kind") == "NSNumber" and value.get("status") == "captured", "numeric snapshot missing")
    require(value.get("original_declaration_recovered") is False, "NSNumber cannot recover original declaration")
    encoding = value.get("objc_type")
    require(type(encoding) is str and encoding in "cCsSiIlLqQfdB" and len(encoding) == 1, "NSNumber encoding unsupported")
    category = value.get("category")
    if category == "boolean":
        require(type(value.get("value")) is bool, "boolean snapshot malformed")
        return value["value"]
    width = value.get("observed_width_bits")
    require(type(width) is int and width in (8, 16, 32, 64), "observed numeric width malformed")
    bits = value.get("bits_hex")
    require(type(bits) is str and re.fullmatch(r"[0-9a-f]{" + str(width // 4) + "}", bits), "numeric bits malformed")
    if category == "binary_float":
        require((encoding, width) in (("f", 32), ("d", 64)), "float encoding/width mismatch")
        actual = struct.unpack(">f" if width == 32 else ">d", bytes.fromhex(bits))[0]
        classification = "nan" if math.isnan(actual) else "infinity" if math.isinf(actual) else "finite"
        require(value.get("classification") == classification, "float classification mismatch")
        negative_zero = actual == 0 and bool(int(bits, 16) >> (width - 1))
        require(type(value.get("negative_zero")) is bool and value["negative_zero"] == negative_zero, "negative zero lost")
        return actual
    signed = category == "signed_integer"
    require(category in ("signed_integer", "unsigned_integer"), "numeric category unsupported")
    require(encoding in ("csilq" if signed else "CSILQB"), "integer encoding/sign mismatch")
    possible = {"c": (8,), "s": (16,), "i": (32,), "l": (32, 64), "q": (64,), "b": (8,)}
    require(width in possible[encoding.lower()], "integer encoding/width mismatch")
    decimal = value.get("decimal")
    require(type(decimal) is str and len(decimal) <= 21 and re.fullmatch(r"-?(?:0|[1-9][0-9]*)", decimal), "integer decimal malformed")
    actual = int(decimal)
    low, high = (-(1 << (width - 1)), (1 << (width - 1)) - 1) if signed else (0, (1 << width) - 1)
    require(low <= actual <= high and (actual & ((1 << width) - 1)) == int(bits, 16), "integer bits/decimal mismatch")
    return actual


def validate_native(report):
    require(type(report) is dict and type(report.get("schema")) is int and report["schema"] == 1, "unsupported snapshot schema")
    require(report.get("status") == "captured" and report.get("scope") == SCOPE, "option probe did not capture expected scope")
    require(report.get("native_option_getters_verified") is True and report.get("unsupported_macro_value_rejected") is True,
            "native getter/value checks incomplete")
    for name in FALSE_EVIDENCE:
        require(report.get(name) is False, "unsupported evidence claim: " + name)
    sdk, os_version = report.get("sdk_max_allowed"), report.get("os_version")
    require(uint(sdk) and sdk >= 150000, "unsupported SDK")
    require(type(os_version) is list and len(os_version) == 3 and all(uint(v, 999) for v in os_version), "OS version missing")
    require(tuple(os_version[:2]) >= (13, 3), "unsupported OS")
    expected = [("fresh_defaults", []), ("legacy_safe", ["fastMathEnabled"]), ("legacy_fast", ["fastMathEnabled"])]
    if os_version[0] >= 15:
        expected += [("modern_" + mode, MODERN_SETTERS) for mode in ("safe", "relaxed", "fast")]
    else:
        require(report.get("modern_authored_cases") == "unsupported_os", "modern cases must remain unsupported")
    expected += [("typed_macros", ["preprocessorMacros"]), ("explicit_empty_and_output", ["preprocessorMacros", "libraries", "libraryType", "installName"])]
    cases = report.get("cases")
    require(type(cases) is list and len(cases) == len(expected), "missing or extra authored cases")
    for case, (name, setters) in zip(cases, expected):
        require(type(case) is dict and case.get("case") == name and case.get("explicit_fixture_setters") == setters, "authored provenance differs")
        require(case.get("fresh_unmodified_object") is (name == "fresh_defaults")
                and case.get("runtime_assignment_provenance_verified") is False, "runtime provenance cannot be inferred")
        fields = case.get("fields")
        require(type(fields) is dict and set(fields) == FIELDS, "snapshot field set differs")
        for field, observation in fields.items():
            require(type(observation) is dict, "field observation malformed")
            minimum_sdk, minimum_os = LATE_FIELDS.get(field, (0, (0, 0)))
            availability = "unsupported_sdk" if sdk < minimum_sdk else "unsupported_os" if tuple(os_version[:2]) < minimum_os else "supported"
            require(observation.get("availability") == availability, "availability differs: " + field)
            if availability != "supported":
                require(observation.get("origin") == "unobserved" and "value" not in observation, "unsupported field has invented value")
                continue
            origin = "explicit_fixture_assignment" if field in setters else "default_observed" if name == "fresh_defaults" \
                else "affected_by_legacy_math_assignment" if field in ("mathMode", "mathFloatingPointFunctions") and "fastMathEnabled" in setters \
                else "observed_after_modern_math_assignment" if field == "fastMathEnabled" and "mathMode" in setters \
                else "not_explicitly_assigned_by_fixture"
            require(observation.get("origin") == origin and "value" in observation, "field provenance differs: " + field)
            value = observation["value"]
            if field in ("fastMathEnabled", "preserveInvariance", "allowReferencingUndefinedSymbols", "enableLogging"):
                require(type(value) is bool, "boolean option malformed")
            elif field in ("libraryType", "compileSymbolVisibility", "maxTotalThreadsPerThreadgroup", "floatingPointConversionRoundingMode"):
                require(uint(value), "integer option malformed")
            elif field == "requiredThreadsPerThreadgroup":
                require(type(value) is list and len(value) == 3 and all(uint(v) for v in value), "thread dimensions malformed")
            elif field == "installName":
                require(value is None or type(value) is str and len(value) <= 1024, "install name malformed")
            elif field == "libraries":
                require(type(value) is dict and type(value.get("is_nil")) is bool and uint(value.get("count"))
                        and value.get("nonempty_library_identity_verified") is False, "linked library identity cannot be inferred")
                require(value.get("status") == ("empty_observed" if value["count"] == 0 else "nonempty_library_identity_unverified"), "library status differs")
            elif field in ("languageVersion", "optimizationLevel", "mathMode", "mathFloatingPointFunctions"):
                require(type(value) is dict and uint(value.get("raw")), "enum option malformed")
            elif field == "preprocessorMacros":
                require(type(value) is dict and type(value.get("is_nil")) is bool and value.get("status") == "captured"
                        and type(value.get("entries")) is list and len(value["entries"]) <= 64, "macro snapshot malformed")
                names = []
                for entry in value["entries"]:
                    require(type(entry) is dict and type(entry.get("name")) is str, "macro entry malformed")
                    names.append(entry["name"])
                    macro = entry.get("value")
                    require(type(macro) is dict, "macro value missing")
                    if macro.get("kind") == "NSNumber":
                        numeric_value(macro)
                    else:
                        require(macro.get("kind") == "NSString" and macro.get("status") == "captured"
                                and type(macro.get("text")) is str and len(macro["text"]) <= 256, "unsupported macro accepted")
                require(names == sorted(set(names)) and (not value["is_nil"] or not names), "macro order/nil mismatch")
        if name.startswith("legacy_"):
            enabled = name == "legacy_fast"
            require(fields["fastMathEnabled"]["value"] is enabled, "legacy setter/getter mismatch")
            if os_version[0] >= 15:
                require(fields["mathMode"]["value"].get("known_name") == ("fast" if enabled else "safe")
                        and fields["mathFloatingPointFunctions"]["value"].get("known_name") == ("fast" if enabled else "precise"), "legacy math mapping differs")
        if name.startswith("modern_"):
            mode = name.removeprefix("modern_")
            require(fields["languageVersion"]["value"].get("known_name") == "2.4"
                    and fields["mathMode"]["value"].get("known_name") == mode
                    and fields["mathFloatingPointFunctions"]["value"].get("known_name") == ("fast" if mode == "fast" else "precise")
                    and fields["preserveInvariance"]["value"] is True
                    and fields["optimizationLevel"]["value"].get("is_default_enum") is True, "modern setter/getter mismatch")
        if name == "explicit_empty_and_output":
            require(fields["preprocessorMacros"]["value"] == {"is_nil": False, "status": "captured", "entries": []}
                    and fields["libraries"]["value"]["is_nil"] is False and fields["libraries"]["value"]["count"] == 0
                    and fields["installName"]["value"] == "authored-option-snapshot", "explicit empty/output setter differs")
        if name != "typed_macros":
            require(case.get("authored_macro_declarations") == {}, "unexpected macro declarations")
    typed = next(case for case in cases if case["case"] == "typed_macros")
    declarations = expected_macros()
    require(json.dumps(typed.get("authored_macro_declarations"), sort_keys=True) == json.dumps(declarations, sort_keys=True), "authored macro declarations differ")
    observed = {entry["name"]: entry["value"] for entry in typed["fields"]["preprocessorMacros"]["value"]["entries"]}
    require(set(observed) == set(declarations), "authored typed macros missing")
    preservation = {}
    for name, declaration in declarations.items():
        value = observed[name]
        kind = declaration["factory_kind"]
        if kind == "NSString":
            preservation[name] = value.get("kind") == "NSString" and value.get("text") == declaration["text"]
        elif kind == "boolean":
            preservation[name] = value.get("category") == "boolean" and value.get("value") is True
        else:
            actual = numeric_value(value)
            width, bits = declaration["declared_width_bits"], declaration["declared_bits_hex"]
            if kind == "binary_float":
                try:
                    preservation[name] = struct.pack(">f" if width == 32 else ">d", actual).hex() == bits
                except (OverflowError, struct.error):
                    preservation[name] = False
            else:
                expected = int(bits, 16)
                if kind == "signed_integer" and expected >> (width - 1):
                    expected -= 1 << width
                preservation[name] = type(actual) is int and actual == expected
    return {"authored_macro_getter_value_preserved": preservation,
            "all_authored_macro_values_preserved": all(preservation.values()),
            "original_declaration_recovered": False, "metal_preprocessor_text_equivalence_verified": False}


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def run(args):
    output = args.output.expanduser().absolute()
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    started, deadline = time.monotonic(), time.monotonic() + args.budget
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [], "inputs": {},
              "native_compilation_verified": False, "native_option_getters_verified": False,
              **{key: False for key in FALSE_EVIDENCE}, "budget_seconds": args.budget}
    write_report(output, report)
    try:
        for name in ("options_snapshot.h", "options_probe.mm", "options_probe.py", "offline.py"):
            raw = (HERE / name).read_bytes()
            (output / name).write_bytes(raw)
            report["inputs"][name] = hashlib.sha256(raw).hexdigest()
        if platform.system() != "Darwin":
            report["status"] = "unsupported"
            report["reason"] = "Mac required; no native compilation or option getter execution was attempted"
            return report
        env = os.environ.copy()
        for name in ("SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET"):
            env.pop(name, None)

        def command(argv, label, per_command=30):
            remaining = deadline - time.monotonic()
            require(remaining > 0, "option probe budget exhausted before " + label)
            event, stdout, _ = control.run_command(argv, label, output, report, env,
                                                  min(per_command, remaining), deadline=deadline)
            control.require_success(event)
            return stdout.strip()

        report["sdk_version"] = command(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-version"], "sdk-version")
        require(re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", report["sdk_version"]), "unrecognized SDK version")
        require(int(report["sdk_version"].split(".")[0]) >= 15, "macOS SDK 15 or later required")
        sdk = command(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"], "sdk-path")
        require(sdk and "\n" not in sdk and Path(sdk).is_dir(), "SDK path unavailable")
        report["sdk_path"] = sdk
        command(["/usr/bin/xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g", "-fobjc-arc",
                 "-mmacosx-version-min=13.3", "-isysroot", sdk, "-Wall", "-Wextra", "-Werror",
                 "-framework", "Foundation", "-framework", "Metal", "-framework", "CoreFoundation",
                 str(output / "options_probe.mm"), "-o", str(output / "options-probe")], "native-compile", 90)
        require((output / "options-probe").is_file(), "option probe binary missing")
        report["native_compilation_verified"] = True
        report["binary_sha256"] = hashlib.sha256((output / "options-probe").read_bytes()).hexdigest()
        write_report(output, report)
        command([str(output / "options-probe"), "--report", str(output / "native.json")], "native-run", 30)
        raw = (output / "native.json").read_bytes()
        require(0 < len(raw) <= 1 << 20, "native option snapshot size invalid")
        report["native"] = json.loads(raw)
        report["typed_value_observations"] = validate_native(report["native"])
        report["native_option_getters_verified"] = True
        report["status"] = "captured"
    except (KeyboardInterrupt, SystemExit):
        report["status"], report["reason"] = "interrupted", "option probe interrupted"
        raise
    except Exception as error:
        report["status"], report["reason"] = "failed", type(error).__name__ + ": " + str(error)
    finally:
        report["elapsed_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--budget", type=float, default=180)
    args = parser.parse_args(argv)
    if not math.isfinite(args.budget) or not 0 < args.budget <= 720:
        parser.error("budget must be finite, positive and at most 720 seconds")
    return args


def main(argv=None):
    try:
        report = run(parse_args(argv))
    except FileExistsError:
        print("choose a new output directory", file=sys.stderr)
        return 2
    print("option snapshot " + report["status"] + "; no shader/GPU/production parity claimed")
    return 0 if report["status"] == "captured" else 78 if report["status"] == "unsupported" else 1


if __name__ == "__main__":
    raise SystemExit(main())
