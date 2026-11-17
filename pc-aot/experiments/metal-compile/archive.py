#!/usr/bin/env python3
"""Save/reload an authored legacy Metal archive in separate Mac processes.

No Metal 4 family condition, game shaders, production settings, or performance
claim. This fixture creates its archive on the selected Mac GPU; it does not
validate the separate Windows-generated archive. Portable tests are controls,
not Apple API or GPU execution evidence.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import platform
import shutil
import sys
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("metal_archive_offline", HERE / "offline.py")
offline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(offline)
SCOPE = "authored-legacy-metal-binary-archive"
FALSE_EVIDENCE = (
    "metal4_verified", "windows_archive_runtime_verified", "nil_library_lookup_verified",
    "physical_iphone_verified", "game_rendering_verified", "performance_improvement_verified",
    "cold_os_cache_verified", "production_compile_settings_parity_verified",
)
OPTIONS = {"target": "air64-apple-macosx13.0", "language": "macos-metal2.4",
           "fast_math": False, "preprocessor_macros": {}, "function_constants": {}}
RECORDED_WORDS = [3, 7, 10, 0x12345678, 4, 14, 10, 0x12345678,
                  5, 21, 10, 0x12345678, 6, 28, 10, 0x12345678]
UNRECORDED_WORDS = [11, 20, 29, 0x87654321, 16, 19, 28, 0x87654321,
                    21, 18, 27, 0x87654321, 26, 17, 26, 0x87654321]


def budget_seconds(value):
    value = float(value)
    if not math.isfinite(value) or not 0 < value <= 180:
        raise argparse.ArgumentTypeError("budget must be greater than zero and at most 180 seconds")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="fresh evidence directory")
    parser.add_argument("--budget", type=budget_seconds, default=180.0,
                        help="overall tool wall budget; cleanup and reporting can extend elapsed time")
    return parser.parse_args(argv)


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def valid_error(value):
    return (isinstance(value, dict) and isinstance(value.get("domain"), str)
            and bool(value["domain"]) and type(value.get("code")) is int
            and isinstance(value.get("description"), str))


def successful(operation, field):
    if not isinstance(operation, dict) or operation.get(field) is not True \
            or operation.get("error_present") is not False or operation.get("error") != {}:
        raise ValueError("operation was not successful: " + field)


def validate_native(report, mode):
    if not isinstance(report, dict) or type(report.get("schema")) is not int or report.get("schema") != 1 or report.get("scope") != SCOPE \
            or report.get("mode") != mode:
        raise ValueError("wrong native archive report identity")
    for key in FALSE_EVIDENCE:
        if report.get(key) is not False:
            raise ValueError("unsupported evidence claim: " + key)
    if type(report.get("process_id")) is not int or report["process_id"] <= 0:
        raise ValueError("missing native process identity")
    if type(report.get("source_library_compiles")) is not int or report.get("source_library_compiles") != 0:
        raise ValueError("native process compiled a source library")
    device = report.get("device", {})
    version = report.get("os_version")
    if not isinstance(device, dict) or not isinstance(version, list) or len(version) != 3 \
            or any(type(part) is not int or part < 0 for part in version):
        raise ValueError("missing device or OS observation")
    if report.get("status") == "unsupported":
        if device.get("available") is not False or report.get("unsupported_reason") != "no_default_metal_device" \
                or report.get("gpu_execution_verified") is not False or type(report.get("url_library_loads")) is not int \
                or report.get("url_library_loads") != 0 or report.get("harvested_kernels") != [] \
                or report.get("pipelines") != [] or report.get("archive_rejections") != []:
            raise ValueError("invalid unsupported report")
        return
    if report.get("status") != "passed" or report.get("gpu_execution_verified") is not True \
            or device.get("available") is not True or not isinstance(device.get("name"), str):
        raise ValueError("native archive run did not pass")
    if type(report.get("url_library_loads")) is not int or report.get("url_library_loads") != 1:
        raise ValueError("expected exactly one offline library load")
    successful(report.get("library_load"), "created")
    successful(report.get("archive_factory"), "created")
    if report["archive_factory"].get("kind") != ("fresh" if mode == "harvest" else "saved"):
        raise ValueError("wrong archive factory route")
    if report.get("harvested_kernels") != (["archive_recorded"] if mode == "harvest" else []):
        raise ValueError("wrong harvested kernel set")
    expected = [("harvest_recorded_strict", "archive_recorded", True, True, True, RECORDED_WORDS)] \
        if mode == "harvest" else [
            ("restore_recorded_strict", "archive_recorded", True, True, True, RECORDED_WORDS),
            ("restore_unrecorded_strict", "archive_unrecorded", True, True, False, None),
            ("restore_unrecorded_fallback", "archive_unrecorded", False, False, True, UNRECORDED_WORDS),
        ]
    rows = report.get("pipelines")
    if not isinstance(rows, list) or len(rows) != len(expected):
        raise ValueError("missing or duplicate pipeline observations")
    for row, (case, kernel, strict, attached, created, words) in zip(rows, expected):
        if not isinstance(row, dict) or row.get("case") != case or row.get("kernel") != kernel \
                or row.get("strict") is not strict or row.get("archive_attached") is not attached \
                or row.get("created") is not created:
            raise ValueError("wrong pipeline route: " + case)
        if created:
            successful(row, "created")
            if row.get("command_status") != "completed" or row.get("command_error") != {} \
                    or row.get("compute_words") != words \
                    or any(type(word) is not int for word in row["compute_words"]):
                raise ValueError("compute readback or command completion mismatch: " + case)
        elif row.get("error_present") is not True or not valid_error(row.get("error")) \
                or "compute_words" in row:
            raise ValueError("strict miss lacks an explicit error")
    rejections = report.get("archive_rejections")
    if mode == "harvest":
        successful(report.get("archive_add"), "succeeded")
        successful(report.get("archive_serialize"), "succeeded")
        if rejections != []:
            raise ValueError("unexpected harvest rejection")
    else:
        if not isinstance(rejections, list) or any(not isinstance(row, dict) for row in rejections) \
                or [row.get("case") for row in rejections] != ["empty_file", "malformed_file"]:
            raise ValueError("missing invalid archive controls")
        for row in rejections:
            if row.get("returned_nil") is not True or row.get("error_present") is not True or not valid_error(row.get("error")):
                raise ValueError("invalid archive was not explicitly rejected")


def load_native(path):
    if not path.is_file() or path.stat().st_size > 1024 * 1024:
        raise ValueError("missing or oversized native archive report")
    return json.loads(path.read_text(encoding="utf-8"))


def run(args):
    started = time.monotonic()
    deadline = started + args.budget
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "scope": SCOPE, "status": "running", "commands": [],
              "host": platform.system(), "source_options": OPTIONS.copy(),
              "overall_tool_wall_budget_seconds": args.budget, "command_timeout_cap_seconds": 60,
              "budget_scope": "Admission/waits share one deadline. Input I/O, process startup, cleanup, reporting and external Metal services are not hard-deadline bounded.",
              "native_reports": [], "offline_compile_verified": False,
              "native_compile_verified": False, "gpu_execution_verified": False,
              "legacy_archive_save_reload_verified": False,
              **{key: False for key in FALSE_EVIDENCE}}
    code = 1
    env, removed = offline.clean_environment()
    report["environment_removed"] = removed

    def command(label, argv, success=True):
        report["active_command"] = label
        write_report(output, report)
        event, stdout, _ = offline.run_command(argv, label, output, report, env, 60, deadline=deadline)
        write_report(output, report)
        if success:
            offline.require_success(event)
        return event, stdout.strip()

    def query(label, argv):
        _, value = command(label, argv)
        if not value or "\n" in value:
            raise ValueError("empty or ambiguous tool path: " + label)
        path = Path(value).resolve()
        if not path.is_file():
            raise ValueError("missing resolved tool: " + label)
        return path

    try:
        if platform.system() != "Darwin":
            report.update(status="unsupported", unsupported_reason="macos_host_required")
            code = 78
            return report, code
        for name in ("archive_fixture.metal", "archive_runtime.mm"):
            shutil.copyfile(HERE / name, output / name)
        report["inputs"] = [offline.file_info(output / name) for name in ("archive_fixture.metal", "archive_runtime.mm")]
        report["runner"] = offline.file_info(Path(__file__))
        report["command_helper"] = offline.file_info(HERE / "offline.py")
        tools, sdk = offline.resolve_tools(None, "macos", "Darwin", output, report, env, 60, deadline)
        xcrun = shutil.which("xcrun", path=env.get("PATH"))
        clang = query("resolve-clangxx", [xcrun, "--sdk", "macosx", "--find", "clang++"])
        tools["clang++"] = clang
        report["tools"] = {name: offline.file_info(path) for name, path in tools.items()}
        report["versions"] = {}
        for name, path in tools.items():
            event, version = command("version-" + name.replace("++", "xx"), [path, "--version"], success=False)
            stderr, _, truncated = offline.log_preview(output / event["stderr"])
            report["versions"][name] = {"returncode": event["returncode"],
                "available": event["returncode"] == 0 and bool((version + stderr).strip()),
                "text": version + "\n" + stderr,
                "stderr_preview_truncated": truncated}
            # Some metallib releases do not implement --version. Keep that
            # failed probe and the executable hash; actual linking must succeed.
            if name != "metallib" or event["returncode"] is None or event.get("timed_out") or event.get("budget_exhausted"):
                offline.require_success(event)
        _, report["sdk_version"] = command("sdk-version", [xcrun, "--sdk", "macosx", "--show-sdk-version"])
        library = output / "archive_fixture.metallib"
        command("compile-metal", [tools["metal"], "-c", "-target", OPTIONS["target"],
                                  "-std=" + OPTIONS["language"], "-fno-fast-math", "-isysroot", sdk,
                                  output / "archive_fixture.metal", "-o", output / "archive_fixture.air"])
        offline.validate_artifact(output / "archive_fixture.air")
        command("link-metallib", [tools["metallib"], output / "archive_fixture.air", "-o", library])
        offline.validate_artifact(library, metallib=True)
        report["offline_compile_verified"] = True
        report["ir_library"] = offline.file_info(library)
        binary = output / "archive-runtime"
        command("compile-native", [clang, "-std=c++17", "-O2", "-g", "-fobjc-arc",
                                   "-Wall", "-Wextra", "-Werror", "-mmacosx-version-min=13.0", "-isysroot", sdk,
                                   output / "archive_runtime.mm", "-framework", "Foundation", "-framework", "Metal",
                                   "-o", binary])
        offline.validate_artifact(binary)
        report["native_compile_verified"] = True
        report["native_binary"] = offline.file_info(binary)
        archive = output / "device.binary.metallib"
        empty, malformed = output / "empty.binary.metallib", output / "malformed.binary.metallib"
        empty.write_bytes(b"")
        malformed.write_bytes(b"Authored negative control: not a Metal archive.\n")
        report["negative_inputs"] = [offline.file_info(path) for path in (empty, malformed)]
        saved_archive = None
        for mode in ("harvest", "restore"):
            native_path = output / (mode + ".json")
            event, _ = command("native-" + mode, [binary, "--mode", mode, "--library", library,
                                                 "--archive", archive, "--empty", empty, "--malformed", malformed,
                                                 "--report", native_path], success=False)
            if native_path.is_file():
                native = load_native(native_path)
                report["native_reports"].append(native)
            else:
                native = None
            if event.get("returncode") == 78 and not event.get("timed_out"):
                validate_native(native, mode)
                if native["status"] != "unsupported":
                    raise ValueError("exit 78 without unsupported evidence")
                report.update(status="unsupported", unsupported_reason=native["unsupported_reason"])
                code = 78
                return report, code
            offline.require_success(event)
            validate_native(native, mode)
            if native["status"] != "passed":
                raise ValueError("native run returned success without passed evidence")
            offline.validate_artifact(archive)
            current = offline.file_info(archive)
            if mode == "harvest":
                saved_archive = current
                report["archive"] = current
            elif current != saved_archive:
                raise ValueError("restore modified the saved archive")
        producer, consumer = report["native_reports"]
        if producer["process_id"] == consumer["process_id"]:
            raise ValueError("harvest and restore were not separate processes")
        if producer["device"] != consumer["device"] or producer.get("os_version") != consumer.get("os_version"):
            raise ValueError("harvest/restore device or OS changed")
        report.update(status="passed", gpu_execution_verified=True, legacy_archive_save_reload_verified=True)
        code = 0
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", error=type(error).__name__)
        raise
    except offline.BudgetExhausted as error:
        report.update(status="budget_exhausted", error=str(error))
    except (OSError, ValueError, RuntimeError) as error:
        report.update(status="failed", error=type(error).__name__ + ": " + str(error))
    finally:
        report.pop("active_command", None)
        report["elapsed_wall_seconds"] = time.monotonic() - started
        write_report(output, report)
    return report, code


def main(argv=None):
    args = parse_args(argv)
    try:
        report, code = run(args)
    except KeyboardInterrupt:
        return 130
    except OSError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps({"status": report["status"], "result": str(args.output.resolve() / "result.json"),
                      "error": report.get("error")}, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
