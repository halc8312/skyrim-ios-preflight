"""Generate a private, original-body diagnostic guest; never execute an EXE.

This route deliberately does not enable the unreviewed fixed-address native
replacements in input-profile.json. It retains original guest bodies, records
rejections, and makes no compatibility or gameplay-success assertion.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
CANDIDATE_SHA = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605"
DISCOVERY_SHA = "1fc28ce90d668d635ace8fd641d95720782e24c0f64eaf08cbaf8bf170d15b6d"

def sha(path):
    return hashlib.file_digest(Path(path).open("rb"), "sha256").hexdigest()

def run(command, cwd, log):
    with log.open("xb") as f:
        subprocess.run([str(a) for a in command], cwd=cwd, stdout=f,
                       stderr=subprocess.STDOUT, check=True)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--candidate", required=True, type=Path)
    p.add_argument("--discovery", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--extra-patch", action="append", type=Path, default=[])
    p.add_argument("--generate", action="store_true")
    args = p.parse_args()
    candidate, discovery, output = (v.resolve() for v in
                                   (args.candidate, args.discovery, args.output))
    if not output.is_relative_to(ROOT / "private") or output.exists():
        raise ValueError("A fresh output inside this project's private/ is required")
    if sha(candidate) != CANDIDATE_SHA or sha(discovery) != DISCOVERY_SHA:
        raise ValueError("The locally reviewed candidate/discovery identity changed")
    lock = json.loads((ROOT / "source-lock.json").read_text())
    source = next(s for s in lock["sources"] if s["name"] == "skydream")
    if source["revision"] != PIN:
        raise ValueError("Translator revision changed")
    patches = []
    for item in source["patches"]:
        path = ROOT / item["path"]
        if sha(path) != item["sha256"]:
            raise ValueError("Translator patch identity changed: " + str(path))
        patches.append(path)
    for path in args.extra_patch:
        path = path.resolve()
        if not path.is_relative_to(ROOT / "patches") or not path.is_file():
            raise ValueError("Extra patch must be an explicit local source patch")
        if path not in patches:
            patches.append(path)
    output.mkdir(parents=True)
    work = output / "source"
    work.mkdir()
    archive = output / "source.tar"
    run(["git", "-c", "core.autocrlf=false", "-C", ROOT / source["path"],
         "archive", "--format=tar", "-o", archive, PIN, "tools", "runtime"],
        ROOT, output / "archive.log")
    with tarfile.open(archive) as f:
        f.extractall(work, filter="data")
    run(["git", "init", "--quiet"], work, output / "stage-git.log")
    applied = []
    for n, path in enumerate(patches):
        # The audio-source patch only touches desktop/CMakeLists.txt; this
        # guest-only stage does not include or compile the desktop runtime.
        if b"desktop/CMakeLists.txt" in path.read_bytes():
            applied.append({"path": str(path.relative_to(ROOT)), "sha256": sha(path),
                            "scope": "public-host-only"})
            continue
        run(["git", "-c", "core.autocrlf=false", "apply", "--check", path], work,
            output / f"patch-{n}-check.log")
        run(["git", "-c", "core.autocrlf=false", "apply", path], work,
            output / f"patch-{n}-apply.log")
        applied.append({"path": str(path.relative_to(ROOT)), "sha256": sha(path),
                        "scope": "guest-applied"})
    record = {"schema": 1, "source_commit": PIN, "patches": applied,
              "candidate_sha256": CANDIDATE_SHA, "discovery_sha256": DISCOVERY_SHA,
              "candidate": str(candidate), "discovery": str(discovery),
              "mode": "original-guest-bodies", "fixed_native_replacements": False,
              "profile_approved": False, "game_executed": False,
              "ios_verified": False, "generation_finished": False}
    result_path = output / "generation.json"
    result_path.write_text(json.dumps(record, indent=2) + "\n")
    if not args.generate:
        print("Prepared original-body diagnostic source", flush=True)
        return
    driver = output / "lift-original.py"
    driver.write_text('''import os, sys
from pathlib import Path
source, candidate, discovery, dest = sys.argv[1:]
sys.path.insert(0, str(Path(source) / "tools"))
os.environ.update(LIFT_GAME="skyrim", LIFT_TSO="1", LIFT_MEMORDER="1", LIFT_FILES="512")
sys.argv = [str(Path(source)/"tools/lift.py"), candidate, discovery, dest, "exe"]
ns = {"__name__": "original_guest_lifter", "__file__": sys.argv[0]}
exec(compile(Path(sys.argv[0]).read_bytes(), sys.argv[0], "exec"), ns)
ns["HOST_OVERRIDES"] = {}
ns["main"]()
''')
    generated = output / "gen"
    started = time.monotonic()
    print("Generating original guest bodies (512 translation units)...", flush=True)
    try:
        run([sys.executable, "-E", "-s", "-B", driver, work, candidate, discovery,
             generated], work, output / "generation.log")
        rejected = json.loads((generated / "exe_rejected.json").read_text())
        table = (generated / "exe_table.c").read_text()
        if "{0xF568A1u, f_F568A1}" not in table:
            raise RuntimeError("Actual PE entry was not translated")
        record.update(generation_finished=True, rejected_function_count=len(rejected),
                      translated_function_count=table.count("{0x"),
                      elapsed_seconds=round(time.monotonic() - started, 2),
                      source_headers_sha256={name: sha(work / "runtime" / name)
                          for name in ("x86.h", "lift_prelude.h", "rt_table.h")})
    finally:
        record["inputs_unchanged"] = sha(candidate) == CANDIDATE_SHA and sha(discovery) == DISCOVERY_SHA
        result_path.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({k: record[k] for k in ("generation_finished", "translated_function_count",
                     "rejected_function_count", "elapsed_seconds", "inputs_unchanged")}))

if __name__ == "__main__":
    main()
