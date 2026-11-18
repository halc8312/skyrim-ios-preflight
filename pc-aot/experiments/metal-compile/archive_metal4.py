#!/usr/bin/env python3
"""Authored direct Metal 4 archive lookup, in isolated Mac processes.

The default mode needs macOS 26 and a Metal 4 device. It creates one recorded
kernel using the installed offline tools, then observes supplied-library and
nil-library lookups without a compiler fallback. --compile-only builds the
native probe for Mac and the iPhone SDK, but executes neither it nor GPU work.
No game input, production integration, or performance claim is supported.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path
import platform
import re
import shutil
import sys
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("metal4_archive_control", HERE / "offline.py")
control = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(control)
SCOPE = "authored-metal4-archive-boundary-suite"
NATIVE_SCOPE = "authored-metal4-direct-archive-lookup"
CASES = ("recorded-supplied", "recorded-nil", "unrecorded-supplied", "unrecorded-nil")
FALSE_EVIDENCE = ("physical_iphone_verified", "full_moltenvk_verified", "game_rendering_verified",
                  "production_compile_options_parity_verified", "performance_improvement_verified",
                  "zero_internal_compilation_verified")
WORDS = [3, 7, 10, 0x12345678, 4, 14, 10, 0x12345678,
         5, 21, 10, 0x12345678, 6, 28, 10, 0x12345678]
TARGET = "air64-apple-macosx26.0"
SNAPSHOTS = ("archive_metal4.py", "archive_metal4.mm", "archive_fixture.metal", "offline.py")


def budget_seconds(value):
    value = float(value)
    if not math.isfinite(value) or not 0 < value <= 240:
        raise argparse.ArgumentTypeError("budget must be positive, finite, and at most 240 seconds")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="new evidence directory")
    parser.add_argument("--compile-only", action="store_true", help="compile Mac and iPhone SDK probes; do not execute")
    parser.add_argument("--budget", type=budget_seconds, default=240.0)
    return parser.parse_args(argv)


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def error_valid(value):
    return isinstance(value, dict) and value.get("present") is True \
        and isinstance(value.get("domain"), str) and bool(value["domain"]) \
        and type(value.get("code")) is int and isinstance(value.get("description"), str)


def no_error(value):
    return isinstance(value, dict) and value == {"present": False} and value.get("present") is False


def validate_native(report, case, returncode):
    """Reject false success/unsupported claims independently of native assertions."""
    if not isinstance(report, dict) or type(report.get("schema")) is not int or report["schema"] != 1 \
            or report.get("scope") != NATIVE_SCOPE or report.get("case") != case:
        raise ValueError("wrong native report identity")
    for field in FALSE_EVIDENCE:
        if report.get(field) is not False:
            raise ValueError("unsupported evidence claim: " + field)
    for field in ("source_library_compiles", "compiler_factory_calls"):
        if type(report.get(field)) is not int or report[field] != 0:
            raise ValueError("unexpected compiler or source library call")
    if type(report.get("process_id")) is not int or report["process_id"] <= 0:
        raise ValueError("missing process identity")
    version = report.get("os_version")
    if not isinstance(version, list) or len(version) != 3 or any(type(v) is not int or v < 0 for v in version):
        raise ValueError("missing OS identity")
    device, counters, gpu = report.get("device"), report.get("counters"), report.get("gpu")
    if not all(isinstance(value, dict) for value in (device, counters, gpu)):
        raise ValueError("missing device, counters or GPU observations")
    for field in ("library_load_attempts", "archive_load_attempts", "direct_archive_lookup_attempts"):
        if type(counters.get(field)) is not int or counters[field] < 0:
            raise ValueError("invalid attempt counter")
    status = report.get("status")
    if status == "unsupported" or case == "probe-device":
        if any(counters.values()) or gpu.get("attempted") is not False \
                or gpu.get("readback_matches") is not False:
            raise ValueError("device probe performed archive or GPU work")
        for field in ("library", "archive", "lookup"):
            if not isinstance(report.get(field), dict) or report[field].get("attempted") is not False:
                raise ValueError("device probe attempted a runtime operation")
        if status == "unsupported":
            if returncode != 78 or not isinstance(report.get("unsupported_reason"), str) \
                    or not report["unsupported_reason"]:
                raise ValueError("unsupported report lacks its non-success exit/reason")
            if device.get("available") is True and device.get("supports_metal4") is True \
                    and device.get("archive_selector_available") is True and version[0] >= 26 \
                    and device.get("architecture_query_status") == "observed":
                raise ValueError("unsupported report contradicts capability evidence")
        elif status != "device_supported" or returncode != 0 or device.get("available") is not True \
                or device.get("supports_metal4") is not True or device.get("archive_selector_available") is not True \
                or version[0] < 26 or device.get("architecture_query_status") != "observed":
            raise ValueError("device probe did not establish support")
        return
    if case not in CASES or version[0] < 26 or device.get("available") is not True \
            or device.get("supports_metal4") is not True or device.get("archive_selector_available") is not True:
        raise ValueError("invalid runtime capability or case")
    supplied, recorded = case.endswith("-supplied"), case.startswith("recorded-")
    if report.get("library_mode") != ("supplied" if supplied else "nil") \
            or report.get("kernel") != ("archive_recorded" if recorded else "archive_unrecorded"):
        raise ValueError("wrong lookup mode or kernel")
    expected = {"library_load_attempts": int(supplied), "archive_load_attempts": 1,
                "direct_archive_lookup_attempts": 1}
    if counters != expected:
        raise ValueError("lookup was not exactly one direct archive call")
    library, archive, lookup = (report.get(key) for key in ("library", "archive", "lookup"))
    if not all(isinstance(row, dict) for row in (library, archive, lookup)):
        raise ValueError("missing factory observations")
    if library.get("attempted") is not supplied or library.get("created") is not supplied:
        raise ValueError("library-free lookup loaded a library or supplied library failed")
    if supplied:
        if sorted(library.get("function_names", [])) != ["archive_recorded", "archive_unrecorded"]:
            raise ValueError("the recorded and unrecorded kernels must both exist in the IR")
        if not no_error(library.get("error")) and not error_valid(library.get("error")):
            raise ValueError("invalid library warning")
    elif library.get("function_names") != [] or not no_error(library.get("error")):
        raise ValueError("nil-library case contains library state")
    if archive.get("attempted") is not True or archive.get("created") is not True \
            or (not no_error(archive.get("error")) and not error_valid(archive.get("error"))) \
            or lookup.get("attempted") is not True:
        raise ValueError("archive or direct lookup was not established")
    created = lookup.get("pipeline_created")
    if created is True:
        if not recorded or status != "recorded_readback_matched" or returncode != 0:
            raise ValueError("unrecorded lookup unexpectedly succeeded or success is inconsistent")
        if not no_error(lookup.get("error")) and not error_valid(lookup.get("error")):
            raise ValueError("invalid lookup warning")
        if gpu.get("attempted") is not True or gpu.get("command_status") != "completed" \
                or not no_error(gpu.get("error")) or gpu.get("readback_matches") is not True \
                or gpu.get("words_u32") != WORDS or any(type(word) is not int for word in gpu["words_u32"]) \
                or gpu.get("expected_u32") != WORDS:
            raise ValueError("exact GPU readback was not established")
    else:
        if created is not False or not error_valid(lookup.get("error")) or gpu.get("attempted") is not False \
                or gpu.get("readback_matches") is not False or gpu.get("words_u32") != []:
            raise ValueError("archive rejection lacks explicit nil/error evidence")
        if recorded:
            if supplied or status != "boundary_rejected" or returncode != 3:
                raise ValueError("recorded lookup failed or nil-library rejection was hidden")
        elif status != "unrecorded_rejected" or returncode != 0:
            raise ValueError("unrecorded rejection was not recorded")


def aggregate_native(reports):
    if len(reports) != 5 or [row.get("case") for row in reports] != ["probe-device", *CASES]:
        raise ValueError("missing, duplicate or reordered native cases")
    if len({row["process_id"] for row in reports}) != len(reports):
        raise ValueError("each case must use a separate process")
    device, version = reports[0]["device"], reports[0]["os_version"]
    if any(row["device"] != device or row["os_version"] != version for row in reports[1:]):
        raise ValueError("device or OS changed between cases")
    nil_success = reports[2]["status"] == "recorded_readback_matched"
    return {"status": "passed" if nil_success else "boundary_rejected",
            "direct_archive_control_verified": True, "library_free_lookup_verified": nil_success,
            "gpu_execution_verified": True}


def pipeline_script():
    return {"libraries": [{"label": "authored", "path": "authored.metallib"}],
            "pipeline_descriptors": {"compute_pipeline_descriptors": [
                {"compute_function_descriptor": "lib:authored#archive_recorded"}]}}


def run(args):
    output = args.output.expanduser().absolute()
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    started, code = time.monotonic(), 1
    deadline = started + args.budget
    env, removed = control.clean_environment()
    report = {"schema": 1, "scope": SCOPE, "status": "running", "compile_only": bool(args.compile_only),
              "commands": [], "native_reports": [], "inputs": {}, "environment_removed": removed,
              "overall_tool_budget_seconds": args.budget, "command_timeout_cap_seconds": 65,
              "budget_scope": "Admission/waits only; startup, I/O, bounded cleanup and external compiler services can extend elapsed time.",
              "requested": {"target": TARGET, "language": "3.1", "math_mode": "safe",
                            "math_floating_point_functions": "precise", "preprocessor_macros": {},
                            "function_constants": {}, "archive_route": "direct_factory_no_compiler_fallback"},
              "mac_native_compile_verified": False, "ios_object_compile_verified": False,
              "offline_archive_generated": False, "direct_archive_control_verified": False,
              "library_free_lookup_verified": False, "gpu_execution_verified": False,
              **{key: False for key in FALSE_EVIDENCE}}
    write_report(output, report)

    def command(label, argv, success=True):
        report["active_command"] = label
        write_report(output, report)
        event, stdout, _ = control.run_command(argv, label, output, report, env, 65, deadline=deadline)
        write_report(output, report)
        if success:
            control.require_success(event)
        return event, stdout.strip()

    def query_path(label, argv, directory=False):
        event, value = command(label, argv)
        path = Path(value)
        if event.get("stdout_preview_truncated") or not value or "\n" in value or not path.is_absolute() \
                or not (path.is_dir() if directory else path.is_file()):
            raise ValueError("missing or ambiguous tool/SDK path: " + label)
        return path  # Preserve clang++/metallib driver aliases, not resolved targets.

    def native(case, extra):
        path = output / (case + ".json")
        event, _ = command("native-" + case, [binary, "--case", case, *extra, "--report", path], success=False)
        result, parse_error = None, None
        try:
            if not path.is_file() or path.stat().st_size > 1024 * 1024:
                raise ValueError("missing or oversized native report")
            result = json.loads(path.read_text(encoding="utf-8"))
            report["native_reports"].append(result)
        except (OSError, ValueError) as error:
            parse_error = str(error)
            report.setdefault("native_report_errors", []).append({"case": case, "error": parse_error})
        # A killed process need not publish JSON. Preserve that fact but keep
        # the original timeout/budget/tool failure as the principal diagnosis.
        if event.get("timed_out") or event.get("budget_exhausted") or event.get("returncode") not in (0, 3, 78):
            control.require_success(event)
        if parse_error:
            raise ValueError(parse_error)
        validate_native(result, case, event["returncode"])
        return result

    try:
        for name in SNAPSHOTS:
            shutil.copyfile(HERE / name, output / name)
            report["inputs"][name] = control.file_info(output / name)
        if platform.system() != "Darwin":
            report.update(status="unsupported", unsupported_reason="macos_host_required")
            return report, 78
        xcrun = shutil.which("xcrun", path=env.get("PATH"))
        if not xcrun:
            raise ValueError("xcrun is unavailable")
        clang = query_path("resolve-clangxx", [xcrun, "--sdk", "macosx", "--find", "clang++"])
        sdk = query_path("resolve-macos-sdk", [xcrun, "--sdk", "macosx", "--show-sdk-path"], directory=True)
        _, version = command("macos-sdk-version", [xcrun, "--sdk", "macosx", "--show-sdk-version"])
        report["sdk_version"] = version
        if not version.split(".")[0].isdigit() or int(version.split(".")[0]) < 26:
            report.update(status="unsupported", unsupported_reason="sdk_before_26")
            return report, 78
        report["tools"] = {"clang++": control.file_info(clang)}
        command("clangxx-version", [clang, "--version"])
        binary = output / "metal4-archive-probe"
        flags = ["-std=c++17", "-O2", "-g", "-fobjc-arc", "-Wall", "-Wextra", "-Werror"]
        command("compile-macos-native", [clang, *flags, "-mmacosx-version-min=26.0", "-isysroot", sdk,
                                         output / "archive_metal4.mm", "-framework", "Foundation", "-framework", "Metal", "-o", binary])
        control.validate_artifact(binary)
        report.update(mac_native_compile_verified=True, native_binary=control.file_info(binary))
        if args.compile_only:
            ios_sdk = query_path("resolve-ios-sdk", [xcrun, "--sdk", "iphoneos", "--show-sdk-path"], directory=True)
            ios_clang = query_path("resolve-ios-clangxx", [xcrun, "--sdk", "iphoneos", "--find", "clang++"])
            obj = output / "metal4-archive-ios26.o"
            report["tools"]["ios-clang++"] = control.file_info(ios_clang)
            command("compile-ios-object", [ios_clang, *flags, "-target", "arm64-apple-ios26.0", "-isysroot", ios_sdk,
                                             "-c", output / "archive_metal4.mm", "-o", obj])
            control.validate_artifact(obj)
            report.update(status="sdk_compilation_passed", ios_object_compile_verified=True, ios_object=control.file_info(obj))
            return report, 0
        host_version = platform.mac_ver()[0]
        report["host_os_version"] = host_version
        if not host_version.split(".")[0].isdigit() or int(host_version.split(".")[0]) < 26:
            report.update(status="unsupported", unsupported_reason="macos_runtime_before_26_or_unobserved")
            return report, 78
        probe = native("probe-device", [])
        if probe["status"] == "unsupported":
            report.update(status="unsupported", unsupported_reason=probe["unsupported_reason"])
            return report, 78
        arch = probe["device"].get("architecture_name")
        if probe["device"].get("architecture_query_status") != "observed" \
                or not isinstance(arch, str) or not re.fullmatch(r"[A-Za-z0-9_]+", arch):
            raise ValueError("device architecture could not be selected explicitly")
        tools = {}
        for name in ("metal", "metallib", "metal-tt", "metal-nt", "metal-config", "metal-lipo"):
            tools[name] = query_path("resolve-" + name, [xcrun, "--sdk", "macosx", "--find", name])
            report["tools"][name] = control.file_info(tools[name])
        event, architectures = command("query-supported-architectures", [tools["metal-config"], "--native-archs-supported", "macos", "26.0"])
        report["tool_architectures"] = architectures.split()
        if event.get("stdout_preview_truncated") or arch not in report["tool_architectures"]:
            report.update(status="unsupported", unsupported_reason="device_architecture_absent_from_tool_query")
            return report, 78
        report["selected_architecture"] = arch
        library, air = output / "authored.metallib", output / "authored.air"
        command("compile-authored-ir", [tools["metal"], "-c", "-target", TARGET, "-std=metal3.1", "-fmetal-math-mode=safe",
                                        "-fmetal-math-fp32-functions=precise", "-isysroot", sdk, output / "archive_fixture.metal", "-o", air])
        control.validate_artifact(air)
        command("link-authored-ir", [tools["metallib"], air, "-o", library])
        control.validate_artifact(library, metallib=True)
        script, archive = output / "recorded.mtl4-json", output / "recorded.mtl4archive"
        script.write_text(json.dumps(pipeline_script(), indent=2) + "\n", encoding="utf-8")
        report["pipeline_script"] = control.file_info(script)
        command("generate-recorded-archive", [tools["metal-tt"], "-target", TARGET, "-arch", arch,
                                               "-disable-disk-cache", "-verbose", "-L", output, library, script, "-o", archive])
        control.validate_artifact(archive)
        event, archived_arch = command("inspect-archive-architecture", [tools["metal-lipo"], "-archs", archive])
        if event.get("stdout_preview_truncated") or archived_arch.split() != [arch]:
            raise ValueError("generated archive architecture mismatch")
        original_archive, original_library = control.file_info(archive), control.file_info(library)
        if original_archive["changed_during_hash"] or original_library["changed_during_hash"]:
            raise ValueError("generated archive or library changed while hashing")
        report.update(offline_archive_generated=True, archive=original_archive, library=original_library)
        for case in CASES:
            extra = ["--archive", archive]
            if case.endswith("-supplied"):
                extra.extend(["--library", library])
            result = native(case, extra)
            if control.file_info(archive) != original_archive or control.file_info(library) != original_library:
                raise ValueError("lookup changed original archive or library bytes")
            if result["status"] == "unsupported":
                report.update(status="unsupported", unsupported_reason="capability_changed_between_processes")
                return report, 78
        report.update(aggregate_native(report["native_reports"]))
        code = 0 if report["status"] == "passed" else 3
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", error=type(error).__name__)
        raise
    except Exception as error:
        report.update(status="budget_exhausted" if isinstance(error, control.BudgetExhausted) else "failed",
                      error=type(error).__name__ + ": " + str(error))
    finally:
        report.pop("active_command", None)
        report["elapsed_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report, code


def main(argv=None):
    args = parse_args(argv)
    try:
        report, code = run(args)
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps({"status": report["status"], "result": str(args.output / "result.json"), "error": report.get("error")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
