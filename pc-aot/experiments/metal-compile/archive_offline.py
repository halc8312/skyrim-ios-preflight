#!/usr/bin/env python3
"""Generate two authored, constant-free Metal 4 archives with Windows tools.

This fixed iOS 26 / Apple9 experiment accepts no game or arbitrary shader input.
Successful .mtl4-json translation and architecture inspection do not establish
runtime archive loading, GPU correctness, library-free lookup, or performance.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import math
from pathlib import Path
import platform
import sys
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("archive_offline_batch_support", HERE / "batch.py")
batch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(batch)
control = batch.offline
SCOPE = "authored-constant-free-offline-metal4-archives"
SUCCESS = "offline_mtl4_script_archives_generated"
TARGET, FAMILY, ARCH = "air64-apple-ios26.0", "apple9", "applegpu_g16p"
TOOL_NAMES = ("metal", "metallib", "metal-nt", "metal-tt", "metal-config", "metal-lipo")
SNAPSHOTS = ("archive_offline.py", "archive_offline_fixture.metal", "offline.py", "batch.py")
PRODUCTS = ("authored.air", "authored.metallib", "compute.mtl4archive", "render.mtl4archive")
FALSE_EVIDENCE = ("metal4_runtime_archive_load_verified", "gpu_execution_verified",
                  "library_free_lookup_verified", "physical_iphone_verified", "game_rendering_verified",
                  "production_compile_options_parity_verified", "performance_improvement_verified")
SCHEMA_SOURCE = {
    "document": "metal4-pipelines-script(5)", "extension": ".mtl4-json",
    "provenance": "Installed Xcode 26.6 Metal toolchain manual, captured in Mac run 04; fixed authored subset only",
    "captured_manual_sha256": "8a3a76e52f6264d8ad859d3182d651a692c29729e848e67e980787393c9dd411",
    "fields": "libraries(label,path), lib:<label>#<function>, compute/render_pipeline_descriptors",
    "general_schema_validation_implemented": False,
}


def descriptor(case):
    """Only the two reviewed, constant-free authored descriptors are allowed."""
    if case == "compute":
        pipelines = {"compute_pipeline_descriptors": [
            {"compute_function_descriptor": "lib:authored#archive_recorded"}]}
    elif case == "render":
        pipelines = {"render_pipeline_descriptors": [{
            "vertex_function_descriptor": "lib:authored#archive_vertex",
            "fragment_function_descriptor": "lib:authored#archive_fragment",
            "raster_sample_count": 1, "rasterization_enabled": True,
            "input_primitive_topology": "Triangle",
            "color_attachments": [{"pixel_format": "RGBA8Unorm", "write_mask": "All", "blending_state": "Disabled"}],
        }]}
    else:
        raise ValueError("only the authored compute and render descriptors are supported")
    return {"libraries": [{"label": "authored", "path": "authored.metallib"}],
            "pipeline_descriptors": pipelines}


def write_report(output, report):
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def finite_bounds(timeout, budget):
    if not math.isfinite(timeout) or not 0 < timeout <= 65 or not math.isfinite(budget) or not 0 < budget <= 180:
        raise ValueError("timeout must be in (0,65] and budget in (0,180], both finite seconds")


def run(args):
    finite_bounds(args.timeout, args.budget)
    output = batch.fresh_directory(args.output)
    started = time.monotonic()
    deadline = started + args.budget
    env, removed = control.clean_environment()
    report = {
        "schema": 1, "scope": SCOPE, "status": "running", "host": platform.platform(),
        "commands": [], "inputs": {}, "tools": {}, "schema_source": dict(SCHEMA_SOURCE),
        "cases": [{"case": name, "status": "not_run", "offline_mtl4_script_archive_generation_verified": False}
                  for name in ("compute", "render")],
        "requested": {"target": TARGET, "deployment": "26.0", "gpu_family": FAMILY, "architecture": ARCH,
                      "language": "3.1", "math_mode": "safe", "math_floating_point_functions": "precise",
                      "preserve_invariance": False, "optimization": "default",
                      "preprocessor_macros": {}, "function_constants": {}},
        "removed_environment_options": removed,
        "command_timeout_cap_seconds": args.timeout, "overall_tool_budget_seconds": args.budget,
        "budget_scope": "Tool admission/waits; startup, bounded tree cleanup, reporting and external services can extend elapsed time.",
        "concurrent_supervised_commands": 1, "compiler_memory_limit_enforced": False,
        "offline_ir_library_generation_verified": False, "offline_mtl4_script_archive_generation_verified": False,
        **{name: False for name in FALSE_EVIDENCE},
    }
    write_report(output, report)
    active_case = None

    def command(label, argv):
        report["active_command"] = label
        write_report(output, report)
        event, stdout, stderr = control.run_command(argv, label, output, report, env, args.timeout, deadline=deadline)
        write_report(output, report)
        control.require_success(event)
        return event, stdout, stderr

    try:
        for name in SNAPSHOTS:
            original = HERE / name
            data = original.read_bytes()
            snapshot = output / name
            snapshot.write_bytes(data)
            report["inputs"][name] = {**control.file_info(snapshot), "original": str(original)}
        for name in ("compute", "render"):
            path = output / (name + ".mtl4-json")
            path.write_text(json.dumps(descriptor(name), indent=2) + "\n", encoding="utf-8")
            report["inputs"][path.name] = control.file_info(path)
        if platform.system() != "Windows":
            report.update(status="unsupported_host", reason="This fixed offline verifier currently supports the installed Windows Metal tools only")
            return report
        # Preserve each driver spelling, particularly metallib.exe. Following a
        # tool's symlink to another driver name can change its link behavior.
        directory = args.tools.expanduser().absolute()
        tools = {name: directory / (name + ".exe") for name in TOOL_NAMES}
        missing = [name for name, path in tools.items() if not path.is_file()]
        if missing:
            report.update(status="unsupported_tools", reason="Required tools are missing", missing_tools=missing)
            return report
        report["tools"] = {name: control.file_info(path) for name, path in tools.items()}
        if any(value["changed_during_hash"] for value in report["tools"].values()):
            raise control.TrialError("Tool changed while capturing provenance")
        for name, flag in (("metal", "--version"), ("metallib", "--version"),
                           ("metal-nt", "-version"), ("metal-tt", "-version")):
            event, version, _ = command(name + "-version", [tools[name], flag])
            report["tools"][name]["version_stdout"] = version
            report["tools"][name]["version_command"] = event["label"]
        command("metal-tt-help", [tools["metal-tt"], "-help"])
        event, architectures, _ = command("query-ios26-apple9-architectures", [tools["metal-config"],
            "--native-archs-supported", "ios", "26.0", "--gpu-family=apple9"])
        if event.get("stdout_preview_truncated"):
            raise control.TrialError("Architecture query was truncated")
        report["config_query_architectures"] = architectures.split()
        # This query can return several architectures. It does not assert that
        # every returned entry is an Apple9-only device. Select exactly one.
        if ARCH not in report["config_query_architectures"]:
            report.update(status="unsupported_architecture", reason="The explicit architecture was absent from the target/family query")
            return report
        source, air, library = output / SNAPSHOTS[1], output / "authored.air", output / "authored.metallib"
        command("compile-authored-ios26", [tools["metal"], "-c", "-target", TARGET, "-std=metal3.1",
            "-fmetal-math-mode=safe", "-fmetal-math-fp32-functions=precise", source, "-o", air])
        control.validate_artifact(air)
        command("link-authored-ir-library", [tools["metallib"], air, "-o", library])
        control.validate_artifact(library, metallib=True)
        report["offline_ir_library_generation_verified"] = True
        report["ir_library"] = control.file_info(library)
        for row in report["cases"]:
            active_case = row
            name = row["case"]
            row["status"] = "running"
            archive, script = output / (name + ".mtl4archive"), output / (name + ".mtl4-json")
            command("generate-" + name + "-mtl4-archive", [tools["metal-tt"], "-target", TARGET,
                "-gpu-family", FAMILY, "-arch", ARCH, "-disable-disk-cache", "-verbose", "-L", output,
                library, script, "-o", archive])
            control.validate_artifact(archive)
            row["archive"] = control.file_info(archive)
            with archive.open("rb") as stream:
                row["archive"]["first_16_bytes_hex"] = stream.read(16).hex()
            event, architectures, _ = command("inspect-" + name + "-archive", [tools["metal-lipo"], "-archs", archive])
            row["archive_architectures"] = architectures.split()
            if event.get("stdout_preview_truncated") or row["archive_architectures"] != [ARCH]:
                raise control.TrialError("Archive architecture inspection did not return exactly the selected architecture")
            if row["archive"]["changed_during_hash"]:
                raise control.TrialError("Archive changed while capturing provenance")
            row.update(status="offline_mtl4_script_archive_generated", offline_mtl4_script_archive_generation_verified=True)
            active_case = None
            write_report(output, report)
        report.update(status=SUCCESS, offline_mtl4_script_archive_generation_verified=True)
    except (KeyboardInterrupt, SystemExit) as error:
        report.update(status="interrupted", reason=type(error).__name__)
        if active_case is not None:
            active_case.update(status="interrupted", reason=type(error).__name__)
        raise
    except Exception as error:
        status = "budget_exhausted" if isinstance(error, control.BudgetExhausted) else "failed"
        report.update(status=status, reason=type(error).__name__ + ": " + str(error))
        if active_case is not None:
            active_case.update(status=status, reason=report["reason"])
    finally:
        report.pop("active_command", None)
        report["elapsed_seconds"] = time.monotonic() - started
        report["observed_outputs"] = []
        # Preserve a partial product without allowing a hashing error to hide
        # the original compiler, timeout, or interrupt result.
        for name in PRODUCTS:
            path = output / name
            try:
                if path.is_file():
                    report["observed_outputs"].append(control.file_info(path))
            except OSError as error:
                report.setdefault("artifact_observation_errors", []).append({"name": name, "error": str(error)})
        write_report(output, report)
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, required=True, help="Existing Windows Metal toolchain bin directory")
    parser.add_argument("--output", type=Path, required=True, help="New output directory; inside the checkout use ignored local/ or private/")
    parser.add_argument("--timeout", type=float, default=65)
    parser.add_argument("--budget", type=float, default=180)
    args = parser.parse_args(argv)
    try:
        finite_bounds(args.timeout, args.budget)
    except ValueError as error:
        parser.error(str(error))
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        report = run(args)
    except (OSError, ValueError, batch.BatchError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print("authored offline Metal 4 archives: " + report["status"])
    return 0 if report["status"] == SUCCESS else 78 if report["status"].startswith("unsupported_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
