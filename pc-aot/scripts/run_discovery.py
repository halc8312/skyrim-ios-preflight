"""Private diagnostic discovery only; never approves a profile or generates C.

Use the approved POSIX Python environment. Each invocation owns a fresh private
directory; interrupted/failed output remains there and is never resumed in place.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_translation import translator_state

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
REQUIREMENTS = {"capstone": "5.0.9", "pefile": "2024.8.26"}
SOURCE_FILES = ("tools/discover.py", "tools/peimg.py")


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def new_run(path):
    root, output = ROOT.resolve(), path.resolve()
    allowed = [root / name for name in ("private", "local")]
    if not any(output != base and output.is_relative_to(base) for base in allowed):
        raise ValueError("output must be a new directory beneath workspace private/ or local/")
    output.mkdir(parents=True, exist_ok=False)
    return output


def save_result(run, result):
    temporary = run / ".result-next.json"
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
    os.replace(temporary, run / "result.json")


def package_evidence():
    evidence = {}
    for name, required in REQUIREMENTS.items():
        dist = importlib.metadata.distribution(name)
        if dist.version != required:
            raise ValueError(f"{name} metadata version must be {required}; found {dist.version}")
        if not dist.files:
            raise ValueError(f"{name} has no installed-file inventory")
        files = []
        for entry in sorted(dist.files, key=str):
            path = Path(dist.locate_file(entry))
            if not path.is_file():
                raise ValueError(f"{name} installed-file inventory contains a missing file")
            files.append({"path": str(entry), "bytes": path.stat().st_size, "sha256": sha256(path)})
        canonical = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
        evidence[name] = {"metadata_version": dist.version, "location": str(dist.locate_file("")),
                          "inventory_sha256": hashlib.sha256(canonical).hexdigest(), "files": files}
    return evidence


def snapshot_input(source, destination, expected):
    digest = hashlib.sha256()
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        for chunk in iter(lambda: incoming.read(1024 * 1024), b""):
            digest.update(chunk)
            outgoing.write(chunk)
    actual = digest.hexdigest()
    if actual != expected:
        raise ValueError("candidate SHA-256 mismatch; retained snapshot is not approved for discovery")
    # Check format only. This is explicitly not executable version/profile approval.
    with destination.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError("candidate is not a PE file")
        offset = struct.unpack_from("<I", header, 60)[0]
        if offset > destination.stat().st_size - 48:
            raise ValueError("candidate PE header is outside the file")
        stream.seek(offset)
        pe = stream.read(48)
        if pe[:4] != b"PE\0\0" or struct.unpack_from("<H", pe, 4)[0] != 0x14C or struct.unpack_from("<H", pe, 24)[0] != 0x10B:
            raise ValueError("discovery requires an x86-32 PE; no architecture fallback")
    return {"sha256": actual, "bytes": destination.stat().st_size, "format": "x86-32 PE",
            "version_identity_verified": False, "snapshot": str(destination)}


def materialize_source(git, repository, run):
    archive = run / "source.tar"
    # Archive conversion must not inherit Windows checkout EOL preferences.
    command = [git, "-c", "core.autocrlf=false", "-C", str(repository),
               "archive", "--format=tar", PIN, *SOURCE_FILES]
    with archive.open("xb") as stream, (run / "source-archive.log").open("xb") as errors:
        subprocess.run(command, check=True, stdout=stream, stderr=errors, timeout=60,
                       env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})
    destination = run / "source"
    destination.mkdir()
    found = set()
    with tarfile.open(archive, "r:") as contents:
        for member in contents:
            if member.isdir() and member.name == "tools":
                continue
            if member.name not in SOURCE_FILES or member.name in found or not member.isfile():
                raise ValueError("unexpected source archive member")
            if member.size > 2 * 1024 * 1024:
                raise ValueError("unexpected source archive member size")
            path = destination / member.name
            path.parent.mkdir(exist_ok=True)
            data = contents.extractfile(member)
            if data is None:
                raise ValueError("missing source archive data")
            with path.open("xb") as stream:
                stream.write(data.read())
            found.add(member.name)
    if found != set(SOURCE_FILES):
        raise ValueError("source archive is incomplete")
    return {"argv": command, "archive_sha256": sha256(archive),
            "files": {name: sha256(destination / name) for name in SOURCE_FILES}}


def inspect_output(path, kind):
    if not path.is_file():
        return {"state": "missing"}
    result = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    try:
        with path.open(encoding="utf-8") as stream:
            value = json.load(stream)
    except (UnicodeError, json.JSONDecodeError):
        return {**result, "state": "truncated_or_invalid_json"}
    if kind == "functions":
        expected = {"image_base": int, "entry": int, "exports": dict, "functions": dict,
                    "jump_tables": dict, "indirect_jumps": list, "iat": dict}
        valid = isinstance(value, dict) and all(isinstance(value.get(key), typ) for key, typ in expected.items())
        valid = valid and bool(value["functions"])
        if valid:
            result["counts"] = {key: len(value[key]) for key in ("functions", "jump_tables", "indirect_jumps")}
    else:
        valid = isinstance(value, list) and all(isinstance(pair, list) and len(pair) == 2 and
                all(isinstance(item, int) for item in pair) and pair[1] >= pair[0] for pair in value)
    return {**result, "state": "valid_json" if valid else "invalid_schema"}


def execute_owned(command, cwd, environment, log):
    """Forward cancellation to run-bounded, which owns the discovery group."""
    pending = []
    process = None

    def forward(signum, _frame):
        if not pending:
            pending.append(signum)
        if process is not None and process.poll() is None:
            try:
                process.send_signal(signum)
            except ProcessLookupError:
                pass

    handlers = {sig: signal.signal(sig, forward) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        if pending:
            forward(pending[0], None)
        code = process.wait()
        return 128 + pending[0] if pending else (code if code >= 0 else 128 - code)
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


def run_discovery(candidate, expected, output, seconds):
    if not re.fullmatch(r"[0-9a-f]{64}", expected) or seconds < 1:
        raise ValueError("supply an exact lowercase SHA-256 and a positive deadline")
    run = new_run(output)
    result = {"schema_version": 1, "scope": "diagnostic_discovery_only", "status": "preflight",
              "started_utc": now(), "source_revision_required": PIN,
              "translation_patches_applied": False,
              "candidate": {"original_path": str(candidate.resolve()), "required_sha256": expected},
              "profile_approved": False, "generation_authorized": False, "game_code_executed": False,
              "workers": 2, "deadline_seconds": seconds, "memory_bounded": False,
              "memory_note": "Decode caches are capped by upstream; total graph/coverage/worker RSS is not bounded.",
              "resume_supported": False, "discovery_started": False}
    save_result(run, result)
    code = 2
    try:
        if os.name != "posix" or sys.version_info < (3, 10):
            raise ValueError("requires POSIX Python 3.10+ with fork; no Windows fallback")
        source = next(s for s in json.loads((ROOT / "source-lock.json").read_text(encoding="utf-8"))["sources"] if s["name"] == "skydream")
        if source["revision"] != PIN or source["path"] != "vendor/skydream" or source["python_requirements"] != REQUIREMENTS:
            raise ValueError("source lock differs from the audited discovery pin or dependencies")
        result["source_lock_sha256"] = sha256(ROOT / "source-lock.json")
        result["source_state"] = translator_state(source)
        if not result["source_state"].get("matches"):
            raise ValueError("translator source missing, modified, or not at exact audited pin")
        result["python"] = {"executable": sys.executable, "version": sys.version, "prefix": sys.prefix,
                            "executable_sha256": sha256(Path(sys.executable).resolve())}
        result["dependencies"] = package_evidence()
        result["runner_sources"] = {name: sha256(ROOT / "scripts" / name) for name in
                                    ("run_discovery.py", "run-bounded.py", "prepare_translation.py", "steam_inputs.py")}
        (run / "inputs").mkdir()
        snapshot = run / "inputs/candidate.exe"
        result["candidate"].update(snapshot_input(candidate, snapshot, expected))
        result["source_snapshot"] = materialize_source(result["source_state"]["git"]["executable"], ROOT / source["path"], run)
        analysis = run / "analysis"
        analysis.mkdir()
        functions = analysis / "functions.json"
        (run / "runner").mkdir()
        bounded = run / "runner/run-bounded.py"
        with bounded.open("xb") as stream:
            stream.write((ROOT / "scripts/run-bounded.py").read_bytes())
        if sha256(bounded) != result["runner_sources"]["run-bounded.py"]:
            raise ValueError("deadline helper changed while preparing the run")
        command = [sys.executable, "-E", "-s", "-B", "-u", str(bounded), str(seconds),
                   sys.executable, "-E", "-s", "-B", "-u", str(run / "source/tools/discover.py"), str(snapshot), str(functions)]
        result.update(command=command, cwd=str(run / "source/tools"), environment_overrides={"DISCOVER_JOBS": "2"},
                      python_environment_ignored=True, status="running", discovery_started=True)
        save_result(run, result)
        with (run / "discovery.log").open("xb") as log:
            code = execute_owned(command, result["cwd"], {**os.environ, "DISCOVER_JOBS": "2"}, log)
        result["process_exit_code"] = code
        result["outputs"] = {"functions": inspect_output(functions, "functions"),
                             "gaps": inspect_output(Path(str(functions) + ".gaps"), "gaps")}
        result["candidate"]["original_sha256_after"] = sha256(candidate)
        result["candidate"]["snapshot_sha256_after"] = sha256(snapshot)
        intact = result["candidate"]["original_sha256_after"] == expected == result["candidate"]["snapshot_sha256_after"]
        completed = code == 0 and intact and all(out["state"] == "valid_json" for out in result["outputs"].values())
        result["status"] = "completed" if completed else ("timed_out" if code == 124 else "interrupted" if code in (130, 143) else "failed")
        if code == 0 and not completed:
            code = 3
    except (OSError, ValueError, KeyError, StopIteration, subprocess.SubprocessError,
            tarfile.TarError, importlib.metadata.PackageNotFoundError) as error:
        result.update(status="failed" if result["discovery_started"] else "blocked", error=str(error))
        code = code if code else 3
    except KeyboardInterrupt:
        result.update(status="interrupted", error="interrupted outside owned discovery command")
        code = 130
    finally:
        result.update(exit_code=code, finished_utc=now())
        result["preserved_files"] = [{"path": p.relative_to(run).as_posix(), "bytes": p.stat().st_size, "sha256": sha256(p)}
                                     for p in sorted(run.rglob("*")) if p.is_file() and p.name != "result.json"]
        save_result(run, result)
    return code, run / "result.json"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=21600)
    args = parser.parse_args(argv)
    try:
        code, manifest = run_discovery(args.input.resolve(), args.sha256, args.output, args.timeout_seconds)
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "request_failed", "error": str(error)}), file=sys.stderr)
        return 2
    print(json.dumps({"exit_code": code, "manifest": str(manifest), "scope": "diagnostic_discovery_only", "profile_approved": False}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
