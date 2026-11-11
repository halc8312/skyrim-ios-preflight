"""Plan private AOT regeneration; never unpack, run game code, install or translate.

Exit 2 means blocked prerequisites, 1 means invalid request, 0 means plan ready.
All manifests contain private paths and must stay under ignored workspace roots.
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
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from steam_inputs import executable_record, hashes

ROOT = Path(__file__).resolve().parents[1]
FLAGS = {"LIFT_MEMORDER": "1", "LIFT_TSO": "1", "DISCOVER_JOBS": "2"}
COMPILE = {"LB_STRICT_ORDER": "ON", "LB_GEN_JOBS": "2", "max_build_jobs": 2}


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def sections(path):
    """Read section identity without importing dependencies or executing the PE."""
    with path.open("rb") as stream:
        stream.seek(60)
        offset = struct.unpack("<I", stream.read(4))[0]
        stream.seek(offset + 6)
        count = struct.unpack("<H", stream.read(2))[0]
        stream.seek(offset + 20)
        optional_size = struct.unpack("<H", stream.read(2))[0]
        stream.seek(offset + 24 + optional_size)
        result = []
        for _ in range(count):
            raw = stream.read(40)
            name, virtual_size, rva, raw_size, raw_offset, _, _, _, _, characteristics = struct.unpack("<8sIIIIIIHHI", raw)
            result.append({"name": name.rstrip(b"\0").decode("ascii", errors="replace"),
                           "rva": rva, "virtual_size": virtual_size, "raw_size": raw_size,
                           "raw_offset": raw_offset, "characteristics": characteristics})
        return result


def inspect_input(path, role, blockers):
    if path is None:
        blockers.append(f"missing_{role}")
        return None
    if not path.is_file():
        blockers.append(f"missing_{role}_file")
        return {"path": str(path.resolve()), "role": role, "error": "not a regular file"}
    record = executable_record(path, role)
    if "inspection_error" in record:
        blockers.append(f"invalid_{role}_pe")
        return record
    record["section_metadata"] = sections(path)
    record["sections_sha256"] = digest(record["section_metadata"])
    if record["pe"]["machine"] != "0x014c" or record["pe"]["bits"] != 32:
        blockers.append(f"unsupported_{role}_architecture")
    return record


def dependencies(requirements):
    result = {}
    for name, wanted in requirements.items():
        try:
            actual = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            actual = None
        result[name] = {"required": wanted, "installed": actual, "matches": actual == wanted}
    python3 = shutil.which("python3")
    return {"python": sys.executable, "python_version": list(sys.version_info[:3]),
            "python3_in_path": python3,
            "python3_matches_preflight": bool(python3 and os.path.abspath(python3) == os.path.abspath(sys.executable)),
            "packages": result, "posix_host": os.name == "posix", "sh": shutil.which("sh")}


def translator_state(source):
    path = ROOT / source["path"]
    result = {"path": str(path), "required_revision": source["revision"]}
    try:
        head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], check=True,
                              capture_output=True, text=True, timeout=10).stdout.strip()
        result["actual_revision"] = head
        dirty = subprocess.run(["git", "-C", str(path), "status", "--porcelain", "--untracked-files=all"],
                               check=True, capture_output=True, text=True, timeout=30,
                               env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"}).stdout.strip()
        result.update(actual_revision=head, clean=not dirty,
                      matches=head == source["revision"] and not dirty)
    except (OSError, subprocess.SubprocessError) as error:
        result.update(matches=False, error=type(error).__name__)
    return result


def validate_profile(record, profile, revision):
    problems = []
    if profile.get("translator_revision") != revision:
        problems.append("profile_translator_mismatch")
    if record and "pe" in record:
        if any(record["pe"].get(k) != v for k, v in profile["expected_pe"].items()):
            problems.append("unpacked_pe_layout_mismatch")
        if any(s["name"] == ".bind" for s in record["section_metadata"]):
            problems.append("unpacked_still_has_steamstub_section")
    reviewed = profile.get("reviewed_unpacked")
    required = ("sha1", "sha256", "entry_rva", "sections_sha256", "review_evidence")
    if not isinstance(reviewed, dict) or not all(reviewed.get(k) for k in required):
        problems.append("unpacked_hash_and_layout_not_reviewed")
    elif not record or "pe" not in record or any(
            record.get(k) != reviewed[k] for k in ("sha1", "sha256", "sections_sha256")) or record["pe"]["entry_rva"] != reviewed["entry_rva"]:
        problems.append("unpacked_not_in_reviewed_profile")
    return problems


def make_plan(original=None, unpacked=None, recipe=None, sdk=None):
    source = next(s for s in json.loads((ROOT / "source-lock.json").read_text(encoding="utf-8"))["sources"] if s["name"] == "skydream")
    profile_path = ROOT / "input-profile.json"
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    blockers = []
    original_record = inspect_input(original, "original", blockers)
    unpacked_record = inspect_input(unpacked, "unpacked", blockers)
    if original and unpacked and original.is_file() and unpacked.is_file() and original.samefile(unpacked):
        blockers.append("original_and_unpacked_must_be_separate_files")
    recipe_record = None
    if recipe is None or not recipe.is_file():
        blockers.append("missing_provenance_recipe")
    else:
        recipe_record = {"path": str(recipe.resolve()), **hashes(recipe)}
        try:
            if recipe.stat().st_size > 1024 * 1024:
                raise ValueError("recipe exceeds 1 MiB")
            value = json.loads(recipe.read_text(encoding="utf-8"))
            if not isinstance(value, dict) or not all(isinstance(value.get(k), str) and value[k].strip() for k in ("method", "tool", "tool_version")):
                raise ValueError("method/tool/tool_version are required nonempty strings")
            if not isinstance(value.get("steps"), list) or not value["steps"] or not all(isinstance(s, str) and s.strip() for s in value["steps"]):
                raise ValueError("steps must be a nonempty list of text; never executed")
            recipe_record["description"] = value
            for key, record in (("original_sha256", original_record), ("unpacked_sha256", unpacked_record)):
                if not record or value.get(key) != record.get("sha256") or not re.fullmatch(r"[0-9a-f]{64}", value.get(key, "")):
                    blockers.append(f"recipe_{key}_mismatch")
        except (OSError, ValueError, TypeError) as error:
            blockers.append("invalid_provenance_recipe")
            recipe_record["error"] = str(error)
    blockers.extend(validate_profile(unpacked_record, profile, source["revision"]))
    translator = translator_state(source)
    if not translator["matches"]:
        blockers.append("translator_missing_modified_or_wrong_revision")
    deps = dependencies(source["python_requirements"])
    blockers.extend(f"dependency_{name}_missing_or_wrong_version" for name, state in deps["packages"].items() if not state["matches"])
    if not deps["posix_host"] or not deps["sh"]:
        blockers.append("generation_preflight_requires_posix_host_and_sh")
    if not deps["python3_matches_preflight"]:
        blockers.append("shell_python3_does_not_match_preflight_interpreter")
    if sys.version_info < (3, 10):
        blockers.append("python_3_10_or_newer_required")
    if not sdk:
        blockers.append("exact_iphoneos_sdk_not_supplied")
    elif not re.fullmatch(r"iphoneos[0-9]+\.[0-9]+(?:\.[0-9]+)?", sdk):
        blockers.append("sdk_must_be_exact_versioned_iphoneos_name")
    contract = {"translator_revision": source["revision"], "input_profile_sha256": hashes(profile_path)["sha256"],
                "input_sha256": {"original": (original_record or {}).get("sha256"), "unpacked": (unpacked_record or {}).get("sha256")},
                "provenance_recipe_sha256": (recipe_record or {}).get("sha256"),
                "generation_environment": FLAGS, "compile_options": COMPILE,
                "target": {"platform": "iphoneos", "architecture": "arm64", "sdk": sdk, "sdk_verified": False}}
    return {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
            "status": "blocked" if blockers else "plan_ready", "blockers": blockers,
            "contract_sha256": digest(contract), "contract": contract,
            "original": original_record, "unpacked": unpacked_record, "provenance_recipe": recipe_record,
            "translator": translator, "dependencies": deps,
            "planned_regeneration": {"cwd_relative_to_run": "work/skydream", "environment": FLAGS,
                "argv": ["sh", "tools/regen_gen.sh", "../../inputs"],
                "command": "env LIFT_MEMORDER=1 LIFT_TSO=1 DISCOVER_JOBS=2 sh tools/regen_gen.sh ../../inputs",
                "input_relative_to_run": "inputs/TESV_unpacked.exe", "outputs_relative_to_run": ["work/skydream/analysis", "work/skydream/gen"],
                "prerequisite": "Create a fresh isolated copy of the pinned translator; copy the approved unpacked input, verify hashes again, and run under the preflighted Python/POSIX environment."},
            "executed": False, "staged_inputs": False,
            "ios_build": {"status": "not_implemented", "requires": ["iPhoneOS dependency closure", "SDK verification on Mac", "native application integration", "device fidelity tests"]}}


def write_plan(plan, output):
    output = output.resolve()
    allowed = [ROOT / name for name in ("local", "private", "generated")]
    if not any(output != base and output.is_relative_to(base) for base in allowed):
        raise ValueError("output must be a new directory under workspace local/, private/ or generated/")
    output.mkdir(parents=True, exist_ok=False)
    with (output / "translation-plan.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(plan, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return output / "translation-plan.json"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path)
    parser.add_argument("--unpacked", type=Path)
    parser.add_argument("--recipe", type=Path)
    parser.add_argument("--sdk", help="exact iPhoneOS SDK name, for example iphoneos26.2; no inferred default")
    parser.add_argument("--output", type=Path, required=True, help="new private run directory (never overwritten)")
    args = parser.parse_args(argv)
    try:
        plan = make_plan(args.original, args.unpacked, args.recipe, args.sdk)
        output = write_plan(plan, args.output)
    except (OSError, ValueError, KeyError, StopIteration, struct.error) as error:
        print(json.dumps({"status": "request_failed", "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps({"status": plan["status"], "blockers": plan["blockers"], "manifest": str(output), "executed": False}))
    return 2 if plan["blockers"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
