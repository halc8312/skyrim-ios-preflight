#!/usr/bin/env python3
"""Compare authored fixtures with pinned DXSO; never accept game bytecode.

Linux/WSL host only. Keeps a fresh private evidence directory, compiler errors,
sanitizer output and per-command records. A timeout kills the process group.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC = importlib.util.spec_from_file_location("shader_inputs_native_study", HERE / "study.py")
study = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = study
SPEC.loader.exec_module(study)

UNITS = (
    "dxso/dxso_reader.cpp", "dxso/dxso_header.cpp", "dxso/dxso_code.cpp",
    "dxso/dxso_decoder.cpp", "dxso/dxso_tables.cpp", "dxso/dxso_analysis.cpp",
    "dxso/dxso_common.cpp", "dxso/dxso_module.cpp", "dxvk/dxvk_shader_key.cpp",
    "util/log/log.cpp", "util/util_env.cpp", "util/sha1/sha1_util.cpp", "util/sha1/sha1.c",
)


def authored_cases():
    version, end, dst, src, rel = 0xFFFE0300, 0xFFFF, 0x800F0000, 0x90E40000, 0xB0000000
    bodies = {
        "mov": [0x02000001, dst, src],
        "nop": [0],
        "comment": [0x0002FFFE, 0x12345678, 0x0000FFFF, 0x02000001, dst, src],
        "relative_source": [0x03000001, dst, src | 0x2000, rel],
        "relative_destination": [0x03000001, dst | 0x2000, rel, src],
        "predicate": [0x13000001, dst, 0xB0001000, src],
        "predicate_relative_source": [0x14000001, dst, 0xB0001000, src | 0x2000, rel],
        "declaration": [0x0200001F, 0x80000000, dst],
        "def_float": [0x05000051, 0xA00F0000, 1, 2, 3, 4],
        "def_int": [0x05000030, 0xF00F0000, 0, 0xFFFFFFFF, 0xFFFF, 0],
        "def_bool": [0x0200002F, 0xE00F0800, 1],
        "loop": [0x0200001B, src, src, 0x0000001D],
        "repeat": [0x01000026, src, 0x00000027],
        "if_else": [0x01000028, src, 0x0000002A, 0x0000002B],
        "if_compare": [0x02010029, src, src, 0x0000002B],
        "break_compare": [0x0201002D, src, src],
        "break_predicate": [0x01000060, 0xB0001000],
        "texldd": [0x0500005D, dst, src, src, src, src],
        "sincos_short": [0x02000025, dst, src],
        "sincos_long": [0x04000025, dst, src, src, src],
    }
    cases = [(name, b"BSVS", [version, *body, end]) for name, body in bodies.items()]
    cases.append(("pixel_mov", b"BSPS", [0xFFFF0300, 0x02000001, dst, src, end]))
    return cases


def run_command(command, label, output, report, deadline, timeout=45):
    remaining = min(timeout, deadline - time.monotonic())
    if remaining <= 0:
        raise RuntimeError("oracle_total_deadline")
    event = {"argv": [str(v) for v in command], "label": label, "timeout_seconds": remaining,
             "returncode": None, "timed_out": False}
    report["commands"].append(event)
    started = time.monotonic_ns()
    process = None
    with (output / (label + ".stdout")).open("xb") as stdout, (output / (label + ".stderr")).open("xb") as stderr:
        try:
            process = subprocess.Popen(event["argv"], cwd=output, stdout=stdout, stderr=stderr,
                                       env=dict(os.environ, DXVK_LOG_PATH="none", DXVK_LOG_LEVEL="error",
                                                ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                                                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"),
                                       start_new_session=True)
            try:
                event["returncode"] = process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                event["timed_out"] = True
                os.killpg(process.pid, signal.SIGKILL)
                event["returncode"] = process.wait(timeout=5)
        except BaseException:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            raise
        finally:
            event["duration_ns"] = time.monotonic_ns() - started
            (output / (label + ".json")).write_text(json.dumps(event, indent=2) + "\n")
    if event["timed_out"] or event["returncode"] != 0:
        raise RuntimeError(label + " failed; inspect preserved streams")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args(argv)
    if sys.platform != "linux":
        parser.error("run with the existing Linux/WSL compiler")
    output = study.private_output(args.output)
    report = {"schema": 1, "status": "running", "commands": [], "synthetic_only": True,
              "game_bytecode_executed": False, "sanitizers": bool(args.sanitizers),
              "upstream_revision": study.UPSTREAM}
    try:
        compiler = shutil.which("g++")
        if not compiler:
            raise RuntimeError("existing g++ is required")
        snapshots = {"native_check-source.py": Path(__file__), "study-source.py": HERE / "study.py",
                     "oracle-source.cpp": HERE / "native_oracle.cpp"}
        report["tool_snapshots"] = {}
        for name, origin in snapshots.items():
            content = origin.read_bytes()
            (output / name).write_bytes(content)
            report["tool_snapshots"][name] = hashlib.sha256(content).hexdigest()
        cases = authored_cases()
        expected, header = {}, ["struct Fixture { const char* name; const uint32_t* words; size_t count; };"]
        for index, (name, marker, values) in enumerate(cases):
            code = struct.pack("<" + "I" * len(values), *values)
            parsed = study.validate_sm3(code, marker)
            expected[name] = {key: parsed[key] for key in ("stage_vk", "analyzed_bytes", "dxvk_key")}
            header.append(f"const uint32_t words_{index}[] = {{" + ",".join(f"0x{v:08x}u" for v in values) + "};")
        # A malformed relative operand is explicitly rejected before native code.
        try:
            study.validate_sm3(struct.pack("<5I", 0xFFFE0300, 0x02000001, 0x800F0000, 0x90E42000, 0xFFFF), b"BSVS")
        except study.Invalid:
            report["malformed_preflight_rejected"] = True
        else:
            raise RuntimeError("malformed input reached native eligibility")
        header.append("const Fixture fixtures[] = {")
        for index, (name, _, values) in enumerate(cases):
            header.append(f'{{"{name}", words_{index}, {len(values)}}},')
        header.append("};")
        (output / "synthetic_cases.h").write_text("\n".join(header) + "\n")
        (output / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
        source = ROOT / "vendor/skydream/third_party/dxvk"
        report["key_algorithm_sources"] = study.verify_key_sources(source)
        report["source_sha256"] = {name: hashlib.sha256((source / "src" / name).read_bytes()).hexdigest() for name in UNITS}
        flags = [compiler, "-std=c++17", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                 "-Wno-deprecated-declarations", "-include", "cstdint", "-include", "cstring",
                 "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00"]
        if args.sanitizers:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        for folder in ("src", "src/util", "include", "include/native", "include/native/windows", "include/native/directx"):
            flags += ["-I", str(source / folder)]
        flags += ["-I", str(output)]
        deadline = time.monotonic() + 300
        objects = []
        for index, name in enumerate(UNITS):
            obj = output / (Path(name).stem + ".o")
            run_command(flags + ["-x", "c++", "-c", source / "src" / name, "-o", obj],
                        f"compile-{index:02}", output, report, deadline)
            objects.append(obj)
        binary = output / "authored-oracle"
        run_command(flags + [output / "oracle-source.cpp", *objects, "-Wl,--gc-sections", "-pthread", "-ldl", "-o", binary],
                    "link", output, report, deadline)
        run_command([binary], "oracle", output, report, deadline, timeout=10)
        rows = [json.loads(line) for line in (output / "oracle.stdout").read_text().splitlines()]
        actual = {}
        for row in rows:
            name = row.pop("case")
            if name in actual:
                raise RuntimeError("duplicate oracle case")
            actual[name] = row
        if actual != expected:
            raise RuntimeError("pinned decoder consumption/stage/hash mismatch")
        report.update(status="passed", matched_cases=len(expected))
    except Exception as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}")
    except BaseException:
        report.update(status="interrupted", error="interrupted_before_completion")
        raise
    finally:
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "matched_cases": report.get("matched_cases"),
                      "error": report.get("error"), "game_bytecode_executed": False}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
