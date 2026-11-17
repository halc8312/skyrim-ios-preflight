#!/usr/bin/env python3
"""Run the authored synchronous library comparison on a Metal 4 capable Mac.

Portable control tests are not native compilation or GPU execution evidence.
The optional library must target macOS; its original compile flags are not inferred.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SCOPE = "authored-metal-library-api-and-readback"
SOURCE_OPTIONS = {
    "language": "2.4", "fast_math": False, "preprocessor_macros": {"PROBE_BIAS": 3},
    "function_constant": {"index": 0, "type": "uint", "value": 7},
}
FALSE_EVIDENCE = (
    "full_moltenvk_verified", "physical_iphone_verified", "game_rendering_verified",
    "performance_improvement_verified", "cold_os_cache_verified", "offline_compile_options_verified",
)


class Unsupported(RuntimeError):
    pass


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="fresh evidence directory; never overwritten")
    parser.add_argument("--repeat", type=int, choices=range(1, 6), default=3)
    parser.add_argument("--library", type=Path, help="optional macOS-targeted authored fixture.metallib")
    return parser.parse_args(argv)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_report(output, report):
    # Each run owns a fresh directory. A replace keeps interruptions from leaving half JSON.
    temporary = output / "result.tmp"
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(output / "result.json")


def stop_process(process, event):
    """Bound cleanup; never wait for pipe EOF or claim compiler-service cancellation."""
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
            event["cleanup_scope"] = "spawned process group; external Metal services are outside this group"
        else:
            # The actual runner refuses non-Mac hosts. This path supports portable child-control tests.
            process.kill()
            event["cleanup_scope"] = "direct synthetic child on unsupported host"
    except ProcessLookupError:
        pass
    except OSError as error:
        event["cleanup_error"] = str(error)
        try:
            process.kill()
        except OSError:
            pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        event["cleanup_error"] = "child did not exit within the bounded cleanup wait"


def run_command(command, label, output, report, env, timeout):
    """Raw streams go to files so a descendant cannot hold PIPE EOF open."""
    event = {"label": label, "argv": [str(part) for part in command], "status": "running",
             "stdout": label + ".stdout.log", "stderr": label + ".stderr.log", "timeout_seconds": timeout}
    report["commands"].append(event)
    start = time.perf_counter_ns()
    process = None
    try:
        with (output / event["stdout"]).open("wb") as stdout, (output / event["stderr"]).open("wb") as stderr:
            process = subprocess.Popen(event["argv"], cwd=output, env=env, stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, shell=False,
                                       start_new_session=(os.name == "posix"))
            try:
                event["returncode"] = process.wait(timeout=timeout)
                event["status"] = "completed"
            except subprocess.TimeoutExpired:
                event["status"] = "timed_out"
                stop_process(process, event)
                event["returncode"] = process.returncode
                raise RuntimeError(label + " exceeded its supervisor deadline")
            except BaseException:
                event["status"] = "interrupted"
                stop_process(process, event)
                event["returncode"] = process.returncode
                raise
    except OSError as error:
        event["status"] = "launch_failed"
        event["error"] = str(error)
        raise
    finally:
        event["duration_ns"] = time.perf_counter_ns() - start
    return event


def probe(command, label, output, report, env):
    event = run_command(command, label, output, report, env, 30)
    if event["returncode"] != 0:
        raise RuntimeError(label + " failed; see retained command logs")
    return (output / event["stdout"]).read_text(encoding="utf-8", errors="replace").strip()


def version_major(value):
    prefix = value.split(".", 1)[0]
    if not prefix.isdecimal():
        raise ValueError("unrecognized version: " + value)
    return int(prefix)


def expected_render_bits():
    return [struct.unpack("<I", struct.pack("<f", value))[0]
            for value in (3 / 16, 7 / 16, 0.5, 1.0)] * 16


def expected_compute_words():
    return [value for index in range(4) for value in (index + 3, (index + 1) * 7, 10, 0x12345678)]


def validate_native(report, repeats, with_library):
    """Independently check every authored output, not just the binary's pass label."""
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def integer(value, label, minimum=0):
        require(type(value) is int and value >= minimum, label + " is not a valid integer")

    require(isinstance(report, dict), "native report is not an object")
    require(type(report.get("schema")) is int and report["schema"] == 1, "unsupported native schema")
    require(report.get("status") == "passed" and report.get("scope") == SCOPE, "native run did not pass the expected scope")
    require(report.get("gpu_execution_verified") is True, "GPU execution was not verified")
    for key in FALSE_EVIDENCE:
        require(report.get(key) is False, "unsupported evidence claim: " + key)
    # JSON canonicalization distinguishes booleans from integers (Python equality does not).
    require(json.dumps(report.get("source_options"), sort_keys=True) == json.dumps(SOURCE_OPTIONS, sort_keys=True),
            "source options differ")
    require(type(report.get("repeat")) is int and report["repeat"] == repeats, "repeat count differs")
    require(report.get("offline_library_requested") is with_library, "offline route differs")
    require(report.get("specialization_scope") == "vertex lookup plus fragment and compute specialization",
            "specialization timing scope differs")
    os_version = report.get("os_version")
    require(isinstance(os_version, list) and len(os_version) == 3, "OS version missing")
    for component in os_version:
        integer(component, "OS version")
    require(os_version[0] >= 26, "unsupported native OS")
    device = report.get("device")
    require(isinstance(device, dict) and device.get("supports_metal4") is True
            and isinstance(device.get("name"), str) and bool(device["name"]), "Metal 4 device missing")
    integer(report.get("compiler_setup_ns"), "compiler setup duration")
    expected_order = []
    for iteration in range(1, repeats + 1):
        modes = ["legacy_source", "metal4_source"] if iteration % 2 else ["metal4_source", "legacy_source"]
        if with_library:
            modes.append("offline_metallib")
        expected_order.extend((iteration, api) for api in modes)
    samples = report.get("samples")
    require(isinstance(samples, list) and len(samples) == len(expected_order), "missing or extra samples")
    for sample, (iteration, api) in zip(samples, expected_order):
        require(isinstance(sample, dict) and sample.get("api") == api
                and type(sample.get("iteration")) is int and sample["iteration"] == iteration,
                "sample route/order differs")
        require(sample.get("library_operation") == ("load" if api == "offline_metallib" else "compile"),
                "library load must not be labeled compile")
        for key in ("library_ns", "specialization_ns", "render_pso_ns", "compute_pso_ns"):
            integer(sample.get(key), key)
        require(sample.get("command_status") == "completed", "GPU command incomplete")
        for key, expected in (("render_bits", expected_render_bits()), ("compute_words", expected_compute_words())):
            values = sample.get(key)
            require(isinstance(values, list) and len(values) == len(expected), "invalid readback length: " + key)
            require(all(type(value) is int and 0 <= value <= 0xffffffff for value in values),
                    "invalid readback word: " + key)
            require(values == expected, "authored readback mismatch: " + key)
    rejections = report.get("rejections")
    require(isinstance(rejections, list) and len(rejections) == 2, "both real source rejections are required")
    for rejection, api in zip(rejections, ("legacy_source", "metal4_source")):
        require(isinstance(rejection, dict) and rejection.get("api") == api
                and rejection.get("returned_nil") is True and rejection.get("error_present") is True,
                "invalid source did not return nil plus NSError")
        integer(rejection.get("library_ns"), "rejection duration")
        error = rejection.get("error")
        require(isinstance(error, dict) and isinstance(error.get("domain"), str) and bool(error["domain"])
                and type(error.get("code")) is int, "rejection diagnostic missing")


def run_experiment(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": 1, "status": "running", "scope": SCOPE, "source_options": SOURCE_OPTIONS,
        "repeat": args.repeat, "commands": [], "inputs": {},
        "host": {"system": platform.system(), "release": platform.release(), "macos": platform.mac_ver()[0]},
        "native_compilation_verified": False, "gpu_execution_verified": False,
        **{key: False for key in FALSE_EVIDENCE},
        "timing_limits": "synchronous host intervals; OS/compiler caches uncontrolled; repeated source may be warm; no Windows elapsed-time comparison",
        "deadline_limits": "supervisor process deadline; not Metal compiler-service cancellation or production async validation",
        "offline_library_scope": "requires macOS target; original offline target/options are not established by this runner",
    }
    write_report(output, report)
    try:
        for name in ("fixture.metal", "runtime.mm", "runtime.py"):
            target = output / name
            target.write_bytes((HERE / name).read_bytes())
            report["inputs"][name] = {"path": name, "sha256": sha256(target)}
        if args.library:
            target = output / "offline.metallib"
            target.write_bytes(args.library.resolve().read_bytes())
            if target.stat().st_size == 0:
                raise ValueError("optional library is empty")
            report["inputs"]["offline.metallib"] = {"path": target.name, "sha256": sha256(target),
                                                      "original_path": str(args.library.resolve())}
        if report["host"]["system"] != "Darwin":
            raise Unsupported("this comparison requires a Mac; native compilation and GPU execution were not attempted")
        if version_major(report["host"]["macos"]) < 26:
            raise Unsupported("macOS 26 or later is required; native compilation and GPU execution were not attempted")
        env = os.environ.copy()
        removed = ("SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET")
        for name in removed:
            env.pop(name, None)
        report["removed_environment_overrides"] = list(removed)
        report["sdk_version"] = probe(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-version"], "sdk-version", output, report, env)
        if version_major(report["sdk_version"]) < 26:
            raise Unsupported("macOS SDK 26 or later is required")
        report["sdk_path"] = probe(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"], "sdk-path", output, report, env)
        report["xcode_version"] = probe(["/usr/bin/xcodebuild", "-version"], "xcode-version", output, report, env)
        report["clang_version"] = probe(["/usr/bin/xcrun", "--sdk", "macosx", "clang++", "--version"], "clang-version", output, report, env)
        command = ["/usr/bin/xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O2", "-g", "-fobjc-arc",
                   "-mmacosx-version-min=26.0", "-isysroot", report["sdk_path"], "-Wall", "-Wextra", "-Werror",
                   "-framework", "Foundation", "-framework", "Metal", str(output / "runtime.mm"), "-o", str(output / "runtime")]
        event = run_command(command, "native-compile", output, report, env, 120)
        if event["returncode"] != 0 or not (output / "runtime").is_file():
            raise RuntimeError("native compilation failed or produced no executable; see retained logs")
        report["native_compilation_verified"] = True
        report["binary_sha256"] = sha256(output / "runtime")
        command = [str(output / "runtime"), "--source", str(output / "fixture.metal"),
                   "--report", str(output / "native.json"), "--repeat", str(args.repeat)]
        if args.library:
            command.extend(("--library", str(output / "offline.metallib")))
        write_report(output, report)
        event = run_command(command, "native-run", output, report, env, 180)
        native_path = output / "native.json"
        if native_path.is_file():
            report["native"] = json.loads(native_path.read_text(encoding="utf-8"))
        if event["returncode"] == 78 and report.get("native", {}).get("status") == "unsupported":
            raise Unsupported(report["native"].get("failure", "unsupported native device"))
        if event["returncode"] != 0:
            raise RuntimeError("native comparison failed; see native.json and retained logs")
        validate_native(report.get("native"), args.repeat, args.library is not None)
        report["gpu_execution_verified"] = True
        report["status"] = "passed"
    except Unsupported as error:
        report["status"] = "unsupported"
        report["failure"] = str(error)
    except (KeyboardInterrupt, SystemExit) as error:
        report["status"] = "interrupted"
        report["failure"] = type(error).__name__
        raise
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = type(error).__name__ + ": " + str(error)
    finally:
        write_report(output, report)
    return report


def main(argv=None):
    try:
        report = run_experiment(parse_args(argv))
    except FileExistsError:
        print("output already exists; choose a fresh evidence directory", file=sys.stderr)
        return 2
    print(report["status"] + ": " + report.get("failure", "authored GPU readbacks verified"))
    return 0 if report["status"] == "passed" else 78 if report["status"] == "unsupported" else 1


if __name__ == "__main__":
    raise SystemExit(main())
