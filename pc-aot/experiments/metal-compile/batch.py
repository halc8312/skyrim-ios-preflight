#!/usr/bin/env python3
"""Private generated-MSL batch experiment, independent of the application.

prepare snapshots a declared file set and explicit compile profile into a fresh
private directory. run validates every source/options entry before starting a
serial compiler sequence. This does not establish production option parity,
GPU correctness, runtime cache identity, or an iPhone performance improvement.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path, PurePosixPath
import platform
import re
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC = importlib.util.spec_from_file_location("metal_batch_offline_support", HERE / "offline.py")
offline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(offline)

MAX_CASES = 128
MAX_SOURCE_BYTES = 4 * 1024 * 1024
MAX_TOTAL_SOURCE_BYTES = 32 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
PROFILE_KEYS = {"target", "deployment", "language", "math_mode", "math_functions",
                "preserve_invariance", "optimization", "macros"}
LIMITS = {"max_cases": MAX_CASES, "max_source_bytes": MAX_SOURCE_BYTES,
          "max_total_source_bytes": MAX_TOTAL_SOURCE_BYTES, "max_manifest_bytes": MAX_MANIFEST_BYTES,
          "concurrent_supervised_commands": 1, "child_memory_limit_enforced": False,
          "descendant_process_count_limit_enforced": False,
          "memory_scope": "Bounded manifest/source reads and log previews; no compiler RAM or RSS cap.",
          "process_scope": "One supervised compiler tree at a time; tool-created descendants/services are not count-limited."}


class BatchError(RuntimeError):
    pass


class BudgetExpired(BatchError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def private_destination(path):
    path = Path(path).expanduser().resolve()
    if path.is_relative_to(ROOT) and not any(path.is_relative_to(ROOT / name) for name in ("local", "private")):
        raise BatchError("Generated source, logs and products inside the checkout must stay under local/ or private/")
    return path


def fresh_directory(path):
    path = Path(path).expanduser().absolute()
    # Validate the canonical destination, but create the original named entry:
    # resolving it first would accept an existing symlink to a missing target.
    private_destination(path)
    path.mkdir(parents=True, exist_ok=False)
    return path.resolve()


def read_bounded(path, limit):
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise BatchError(f"Input exceeds the {limit}-byte limit: {Path(path).name}")
    return data


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise BatchError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def read_json(path):
    raw = read_bounded(path, MAX_MANIFEST_BYTES)
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                           parse_constant=lambda value: (_ for _ in ()).throw(BatchError("Non-finite JSON number")))
    except (UnicodeError, ValueError) as error:
        raise BatchError(f"Invalid JSON: {error}") from error
    return value, raw


def validate_profile(profile):
    if not isinstance(profile, dict) or set(profile) != PROFILE_KEYS:
        raise BatchError("A compile profile must explicitly contain exactly: " + ", ".join(sorted(PROFILE_KEYS)))
    if profile["target"] not in ("ios", "macos"):
        raise BatchError("Profile target must be ios or macos")
    for name in ("deployment", "language"):
        pattern = r"[1-9][0-9]?\.[0-9]{1,2}" + (r"(?:\.[0-9]{1,2})?" if name == "deployment" else "")
        if not isinstance(profile[name], str) or not re.fullmatch(pattern, profile[name]):
            raise BatchError(f"Invalid profile {name}")
    if profile["math_mode"] not in ("safe", "relaxed", "fast") or profile["math_functions"] not in ("precise", "fast"):
        raise BatchError("Unknown explicit Metal math options")
    if type(profile["preserve_invariance"]) is not bool or profile["optimization"] != "default":
        raise BatchError("preserve_invariance must be boolean; optimization must explicitly be default")
    macros = profile["macros"]
    if not isinstance(macros, dict) or len(macros) > 32:
        raise BatchError("macros must be an explicit object with at most 32 entries")
    literal = r"(?:true|false|[+-]?(?:0[xX][0-9a-fA-F]+|(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)[uUlLfF]*)"
    for name, value in macros.items():
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,63}", name):
            raise BatchError("Invalid macro name")
        if not isinstance(value, str) or len(value) > 64 or not re.fullmatch(literal, value):
            raise BatchError("Macro values must be explicit numeric/boolean token strings; no inferred specialization values")
    return profile


def include_validation_view(text):
    """A bounded lexical view for include checks, not a C++ preprocessor.

    Compilation still uses the original bytes. Normalize supported line ends
    before phase-2 splicing; comments are then masked without losing newlines.
    Quoted literals keep their text, so comment markers inside them cannot hide
    later directives. Unclassified source forms fail explicitly.
    """
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    if re.search(r"\\[ \t\v\f]+\n", text):
        raise BatchError("Whitespace between backslash and newline is not supported")
    # Trigraph translation precedes line splicing and literal recognition.
    # Do not silently interpret it differently from a compiler extension.
    if re.search(r"\?\?[=/\'()!<>-]", text):
        raise BatchError("Trigraph source forms are not supported")
    text = text.replace("\\\n", "")
    view = list(text)
    size, index = len(text), 0
    while index < size:
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            end = size if end == -1 else end
            view[index:end] = " " * (end - index)
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end == -1:
                raise BatchError("Unterminated block comment in MSL source")
            end += 2
            view[index:end] = ["\n" if char == "\n" else " " for char in text[index:end]]
            index = end
        elif text[index] in "\"'":
            quote = text[index]
            index += 1
            while index < size:
                if text[index] == "\n":
                    raise BatchError("Unspliced newline in a quoted MSL literal")
                if text[index] == "\\":
                    index += 2
                elif text[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            else:
                raise BatchError("Unterminated quoted literal in MSL source")
        elif text[index].isdigit() or (text[index] == "." and index + 1 < size and text[index + 1].isdigit()):
            # Consume preprocessing-number spelling, including C++ digit
            # separators, without mistaking its apostrophes for char literals.
            index += 1
            while index < size:
                char = text[index]
                if char.isalnum() or char in "._":
                    index += 1
                elif char == "'" and index + 1 < size and (text[index + 1].isalnum() or text[index + 1] == "_"):
                    index += 2
                elif char in "+-" and text[index - 1] in "eEpP":
                    index += 1
                else:
                    break
        elif text[index].isalpha() or text[index] == "_":
            start = index
            index += 1
            while index < size and (text[index].isalnum() or text[index] == "_"):
                index += 1
            if text[start:index] in ("R", "u8R", "uR", "UR", "LR") and index < size and text[index] == '"':
                raise BatchError("Raw string literals are not supported by the MSL include checker")
        elif text[index] == "\\" and index + 1 < size and text[index + 1] in "uU":
            raise BatchError("Universal-character escapes outside quoted literals are not supported")
        else:
            index += 1
    return "".join(view)


def validate_source(data):
    if not data or b"\0" in data:
        raise BatchError("MSL source must be nonempty text without NUL bytes")
    try:
        text = data.decode("utf-8")
    except UnicodeError as error:
        raise BatchError("MSL source is not UTF-8") from error
    # The representative generator emits only these SDK headers. This check
    # does not establish semantic equivalence, deterministic preprocessing or
    # a sandbox: source macros, #line and location/time builtins remain allowed.
    without_comments = include_validation_view(text)
    # This deliberately narrow input format does not support the alternative
    # directive introducer. Reject it instead of missing an external include.
    if re.search(r"^[^\S\r\n]*%:", without_comments, re.M):
        raise BatchError("Alternative preprocessor directives are not supported")
    for directive, operand in re.findall(
            r"^[^\S\r\n]*#[^\S\r\n]*(include_next|include|import)\b([^\r\n]*)", without_comments, re.M):
        if directive != "include" or operand.strip() not in ("<metal_stdlib>", "<simd/simd.h>"):
            raise BatchError("Only captured MSL with the declared Metal SDK includes is supported")


def input_key(source_hash, options):
    # An experiment input fingerprint, not a production cache equivalence key.
    return digest(canonical({"source_sha256": source_hash, "options": options}))


def prepare(source_directory, output_directory, profile_path, note):
    output = fresh_directory(output_directory)
    status = {"schema": 1, "status": "failed", "production_parity": False, "limits": LIMITS}
    try:
        if not isinstance(note, str) or not 1 <= len(note) <= 4096:
            raise BatchError("A bounded provenance/selection note is required")
        profile, profile_raw = read_json(profile_path)
        validate_profile(profile)
        source_directory = Path(source_directory).resolve(strict=True)
        if not source_directory.is_dir():
            raise BatchError("Source directory does not exist")
        files = []
        for path in source_directory.glob("*.metal"):
            files.append(path)
            if len(files) > MAX_CASES:
                raise BatchError("Too many source files; select a bounded input set explicitly")
        if not files:
            raise BatchError("No MSL source files were selected")
        files.sort(key=lambda path: path.name)
        sources = output / "sources"
        sources.mkdir()
        cases, total = [], 0
        for index, path in enumerate(files, 1):
            if not path.resolve().is_relative_to(source_directory):
                raise BatchError("Source symlink escapes the selected input directory")
            data = read_bounded(path, MAX_SOURCE_BYTES)
            validate_source(data)
            total += len(data)
            if total > MAX_TOTAL_SOURCE_BYTES:
                raise BatchError("Total source bytes exceed the batch limit")
            case_id = f"case-{index:04d}"
            relative = f"sources/{case_id}.metal"
            (output / relative).write_bytes(data)
            cases.append({"id": case_id, "source": relative, "sha256": digest(data), "bytes": len(data),
                          "original_name": path.name, "options": profile})
        manifest = {"schema": 1, "kind": "private_generated_msl_batch", "production_parity": False,
                    "provenance_note": note, "original_source_directory": str(source_directory), "cases": cases}
        manifest_bytes = json.dumps(manifest, indent=2, ensure_ascii=True).encode("utf-8") + b"\n"
        if len(manifest_bytes) > MAX_MANIFEST_BYTES:
            raise BatchError("Prepared manifest exceeds the byte limit")
        (output / "declared-profile.json").write_bytes(profile_raw)
        (output / "manifest.json").write_bytes(manifest_bytes)
        status.update(status="prepared", cases=len(cases), total_source_bytes=total,
                      unique_source_hashes=len({case["sha256"] for case in cases}),
                      unique_input_keys=len({input_key(case["sha256"], case["options"]) for case in cases}),
                      manifest_sha256=digest(manifest_bytes), profile_sha256=digest(profile_raw))
    except KeyboardInterrupt:
        status.update(status="interrupted", error="KeyboardInterrupt")
        raise
    except (OSError, BatchError) as error:
        status["error"] = f"{type(error).__name__}: {error}"
    finally:
        (output / "prepare-result.json").write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
    return status


def snapshot_manifest(manifest_path, output):
    manifest_path = Path(manifest_path).resolve()
    manifest, raw = read_json(manifest_path)
    (output / "input-manifest.json").write_bytes(raw)
    expected = {"schema", "kind", "production_parity", "provenance_note", "original_source_directory", "cases"}
    if not isinstance(manifest, dict) or set(manifest) != expected or type(manifest["schema"]) is not int or manifest["schema"] != 1 or \
            manifest["kind"] != "private_generated_msl_batch" or manifest["production_parity"] is not False:
        raise BatchError("Unsupported manifest or unjustified production-parity claim")
    if not isinstance(manifest["provenance_note"], str) or not 1 <= len(manifest["provenance_note"]) <= 4096:
        raise BatchError("Missing bounded provenance note")
    cases = manifest["cases"]
    if not isinstance(cases, list) or not 1 <= len(cases) <= MAX_CASES:
        raise BatchError("Manifest case count is outside the bounded range")
    (output / "sources").mkdir()
    ids, total, snapshots = set(), 0, []
    for case in cases:
        if not isinstance(case, dict) or set(case) != {"id", "source", "sha256", "bytes", "original_name", "options"}:
            raise BatchError("Case fields are incomplete or unknown")
        identity = case["id"]
        if not isinstance(identity, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", identity) or identity in ids:
            raise BatchError("Case identities must be bounded and unique")
        ids.add(identity)
        relative = case["source"]
        if not isinstance(relative, str) or len(relative) > 256 or "\\" in relative or ":" in relative:
            raise BatchError("Source path must be a bounded portable relative path")
        relative_path = PurePosixPath(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts or relative_path.suffix != ".metal":
            raise BatchError("Source path escapes the manifest or is not MSL")
        source = (manifest_path.parent / relative_path).resolve(strict=True)
        if not source.is_relative_to(manifest_path.parent):
            raise BatchError("Source symlink escapes the manifest directory")
        validate_profile(case["options"])
        if type(case["bytes"]) is not int or not 0 < case["bytes"] <= MAX_SOURCE_BYTES or \
                not isinstance(case["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", case["sha256"]):
            raise BatchError("Invalid declared source size/hash")
        data = read_bounded(source, MAX_SOURCE_BYTES)
        if len(data) != case["bytes"] or digest(data) != case["sha256"]:
            raise BatchError(f"Source size/hash mismatch for {identity}")
        validate_source(data)
        total += len(data)
        if total > MAX_TOTAL_SOURCE_BYTES:
            raise BatchError("Total source bytes exceed the batch limit")
        snapshot = output / "sources" / (identity + ".metal")
        snapshot.write_bytes(data)
        snapshots.append({**case, "snapshot": str(snapshot), "input_key": input_key(case["sha256"], case["options"])})
    return snapshots, {"sha256": digest(raw), "path": str(manifest_path), "source_bytes": total,
                       "provenance_note": manifest["provenance_note"],
                       "unique_source_hashes": len({case["sha256"] for case in snapshots}),
                       "unique_input_keys": len({case["input_key"] for case in snapshots})}


def compile_command(tool, source, air, options, sdk_path=None):
    validate_profile(options)
    target, language = options["target"], options["language"]
    os_name = "ios" if target == "ios" else "macosx"
    standard = f"{target}-metal{language}" if int(language.split(".")[0]) < 3 else f"metal{language}"
    command = [str(tool), "-c", "-target", f"air64-apple-{os_name}{options['deployment']}", f"-std={standard}",
               "-fmetal-math-mode=" + options["math_mode"], "-fmetal-math-fp32-functions=" + options["math_functions"]]
    if options["preserve_invariance"]:
        command.append("-fpreserve-invariance")
    for name, value in sorted(options["macros"].items()):
        command.append(f"-D{name}={value}")
    if sdk_path is not None:
        command.extend(("-isysroot", str(sdk_path)))
    return command + [str(source), "-o", str(air)]


def run(args):
    output = fresh_directory(args.output)
    env, removed = offline.clean_environment()
    started = time.monotonic()
    deadline = started + args.budget
    report = {"schema": 1, "status": "running", "host": platform.system(), "commands": [], "runs": [],
              "limits": {**LIMITS, "repeat": args.repeat, "command_timeout_seconds": args.timeout,
                         "batch_budget_seconds": args.budget,
                         "budget_scope": "Command admission and waits; bounded cleanup/reporting can extend wall time."},
              "removed_environment_options": removed, "production_parity": False,
              "offline_compile_verified": False, "gpu_execution_verified": False,
              "physical_iphone_verified": False, "game_rendering_verified": False,
              "performance_improvement_verified": False,
              "timing_scope": "Offline host subprocess wall time; OS/tool caches are uncontrolled; no cold/warm or runtime speedup claim."}

    def remaining(divisor=1):
        left = (deadline - time.monotonic()) / divisor
        if left <= 0:
            raise BudgetExpired("Batch wall-time budget exhausted before the next command")
        return min(args.timeout, left)

    def execute(command, label):
        return offline.run_command(command, label, output, report, env, remaining(), deadline)

    try:
        cases, manifest_info = snapshot_manifest(args.manifest, output)
        report.update(manifest=manifest_info, cases=cases)
        report["runs"] = [{"case": case["id"], "repeat": repeat, "status": "not_run", "artifacts": []}
                          for repeat in range(1, args.repeat + 1) for case in cases]
        targets = sorted({case["options"]["target"] for case in cases})
        toolchains = {}
        report["toolchains"] = {}
        for target in targets:
            # Recheck the same absolute deadline before each resolver command,
            # including process startup, instead of dividing a stale allowance.
            metadata = {"commands": report["commands"]}
            paths, sdk_path = offline.resolve_tools(args.tools, target, report["host"], output, metadata, env, remaining(), deadline)
            tools_info = {}
            for name, path in paths.items():
                info = offline.file_info(path)
                if info["changed_during_hash"]:
                    raise BatchError("Tool executable changed while its identity was being recorded")
                event, stdout, stderr = execute([path, "--version"], f"{target}-{name}-version")
                info.update(version_returncode=event["returncode"], version_output=stdout + stderr,
                            version_output_truncated=event["stdout_preview_truncated"] or event["stderr_preview_truncated"])
                if event["returncode"] is None or event["timed_out"]:
                    offline.require_success(event)
                tools_info[name] = info
            public_info = {"sdk": metadata["sdk"], "tools": tools_info}
            report["toolchains"][target] = public_info
            toolchains[target] = (paths, sdk_path, digest(canonical(public_info)))
        stop = False
        for row in report["runs"]:
            case = next(case for case in cases if case["id"] == row["case"])
            paths, sdk_path, toolchain_key = toolchains[case["options"]["target"]]
            remaining()
            directory = output / f"repeat-{row['repeat']:02d}" / case["id"]
            directory.mkdir(parents=True)
            air, library = directory / "shader.air", directory / "shader.metallib"
            row["input_key"] = case["input_key"]
            row["invocation_fingerprint"] = digest(canonical({"input_key": case["input_key"], "toolchain": toolchain_key}))
            row["status"] = "running"
            try:
                label = f"r{row['repeat']}-{case['id']}"
                event, _, _ = execute(compile_command(paths["metal"], case["snapshot"], air, case["options"], sdk_path), label + "-compile")
                row["compile_duration_ns"] = event["duration_ns"]
                stop = event["timed_out"]
                offline.require_success(event)
                offline.validate_artifact(air)
                event, _, _ = execute([paths["metallib"], air, "-o", library], label + "-link")
                row["link_duration_ns"] = event["duration_ns"]
                stop = event["timed_out"]
                offline.require_success(event)
                offline.validate_artifact(library, metallib=True)
                row["status"] = "passed"
            except KeyboardInterrupt:
                row["status"] = "interrupted"
                raise
            except (BudgetExpired, offline.BudgetExhausted):
                row["status"] = "budget_exhausted"
                raise
            except (OSError, offline.TrialError) as error:
                row.update(status="timed_out" if stop else "failed", error=f"{type(error).__name__}: {error}")
            finally:
                for path in (air, library):
                    if path.is_file():
                        row["artifacts"].append(offline.file_info(path))
            if stop:
                # Do not start another compiler after an uncertain tree cleanup.
                break
        report["offline_compile_verified"] = all(row["status"] == "passed" for row in report["runs"])
        report["status"] = "passed" if report["offline_compile_verified"] else "failed"
    except KeyboardInterrupt:
        report.update(status="interrupted", error="KeyboardInterrupt: child cleanup evidence is in the command records")
        raise
    except (BudgetExpired, offline.BudgetExhausted) as error:
        report.update(status="budget_exhausted", error=str(error))
    except (OSError, BatchError, offline.TrialError) as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}")
    finally:
        report["duration_ns"] = int((time.monotonic() - started) * 1e9)
        report["summary"] = {"planned": len(report["runs"]),
                             "passed": sum(row["status"] == "passed" for row in report["runs"]),
                             "failed": sum(row["status"] in ("failed", "timed_out") for row in report["runs"]),
                             "interrupted": sum(row["status"] == "interrupted" for row in report["runs"]),
                             "budget_exhausted": sum(row["status"] == "budget_exhausted" for row in report["runs"]),
                             "not_run": sum(row["status"] == "not_run" for row in report["runs"])}
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def bounded_number(value, low, high, integral=False):
    number = int(value) if integral else float(value)
    if not math.isfinite(number) or not low <= number <= high:
        raise argparse.ArgumentTypeError(f"value must be between {low} and {high}")
    return number


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    prepare_parser = commands.add_parser("prepare", help="snapshot private MSL with an explicit profile")
    prepare_parser.add_argument("--sources", type=Path, required=True)
    prepare_parser.add_argument("--profile", type=Path, required=True, help="JSON with every compile option explicitly supplied")
    prepare_parser.add_argument("--note", required=True, help="provenance and selection limits; never a runtime-capture assumption")
    prepare_parser.add_argument("--output", type=Path, required=True)
    run_parser = commands.add_parser("run", help="validate then compile serially into a fresh private directory")
    run_parser.add_argument("--manifest", type=Path, required=True)
    run_parser.add_argument("--output", type=Path, required=True)
    run_parser.add_argument("--tools", type=Path)
    run_parser.add_argument("--repeat", type=lambda value: bounded_number(value, 1, 3, True), default=1)
    run_parser.add_argument("--timeout", type=lambda value: bounded_number(value, 0.1, 120), default=60.0)
    run_parser.add_argument("--budget", type=lambda value: bounded_number(value, 0.1, 3600), default=900.0)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        report = prepare(args.sources, args.output, args.profile, args.note) if args.action == "prepare" else run(args)
    except (OSError, BatchError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print(f"private offline batch {report['status']}; evidence: {args.output.resolve()}")
    if report["status"] not in ("prepared", "passed"):
        print(report.get("error", "One or more compile/link attempts failed; inspect result.json"), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
