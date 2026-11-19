"""Check both files shared by Classic Lab and reopen the state with the original Windows DLL."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import uuid

from run import ROOT
from run_serialization import HASH, checked_file, expected_bytes, finish, windows


INPUT_HASH = "54af4dbfbdbb19a7f8b0516e5b97f72eb1a7dab686484b56221dce3efe9c8a36"
OUTPUT_HASH = "e03aada2cf67b714b29a0c24989d215879aad68763ac454fc13bc028c150549e"
RETURN_HASH = "75038342f9a0c0ad0a69f71faf85482fc93d9bf659e424906248891d16694216"
OBSERVATIONS = {"schema": 1, "input_value": 16, "first_save_after_revert": 5,
                "output_value": 21, "records_read": 1, "callback_mask": 7}
BUNDLE = "org.skyrimios.pc2011.classic-lab"


def read_shared(path, suffix, limit):
    if path.suffix.lower() != suffix or not stat.S_ISREG(path.lstat().st_mode):
        raise ValueError(f"expected a regular {suffix} file")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{suffix} file exceeds the fixture limit")
    return data


def unique_fields(pairs):
    obj = dict(pairs)
    if len(obj) != len(pairs):
        raise ValueError("duplicate report field")
    return obj


def read_pair(save, report, build, platform):
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", build):
        raise ValueError("supply the expected Classic Lab bundle build number")
    if platform not in ("ios_device", "ios_simulator"):
        raise ValueError("unknown expected iOS platform")
    data = read_shared(save, ".skse", 83)
    if data != expected_bytes(21) or hashlib.sha256(data).hexdigest() != OUTPUT_HASH:
        raise ValueError("the 83-byte returned file differs from the authored fixture")
    raw = read_shared(report, ".json", 4096)
    record = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_fields)
    identities = {"platform": platform, "bundle_id": BUNDLE, "build": build,
                  "input_sha256": INPUT_HASH, "output_sha256": OUTPUT_HASH}
    if not isinstance(record, dict) or set(record) != set(OBSERVATIONS) | set(identities) | {
            "run_id", "os_version", "completed_at"}:
        raise ValueError("unexpected Classic Lab report schema")
    for field, value in (OBSERVATIONS | identities).items():
        if type(record[field]) is not type(value) or record[field] != value:
            raise ValueError(f"unexpected Classic Lab {field}")
    if (not isinstance(record["os_version"], str) or
            not re.fullmatch(r"[0-9]{1,3}(?:\.[0-9]{1,3}){0,3}", record["os_version"])):
        raise ValueError("invalid iOS version")
    run_id = record["run_id"]
    if not isinstance(run_id, str):
        raise ValueError("invalid run ID")
    parsed = uuid.UUID(run_id)
    if not parsed.int or str(parsed) != run_id.lower():
        raise ValueError("invalid run ID")
    date = record["completed_at"]
    if not isinstance(date, str) or not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z", date):
        raise ValueError("expected UTC completion time")
    datetime.fromisoformat(date[:-1] + "+00:00")
    return data, raw, record


def verify_return(save, report, build, platform, llvm):
    data, raw, record = read_pair(save, report, build, platform)
    if sys.platform != "win32":
        raise ValueError("the final step requires the native Windows i386 loader")
    private = ROOT / "local" / "classic-lab-returns"
    if private.resolve() != ROOT.resolve() / "local/classic-lab-returns":
        raise ValueError("private evidence directory points outside the worktree's local folder")
    private.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=private) as temp:
        work = Path(temp)
        evidence = work / "evidence"
        evidence.mkdir()
        shared = evidence / "translated.skse"
        shared.write_bytes(data)
        (evidence / "classic-lab-result.json").write_bytes(raw)
        original = work / "reference"
        native = windows(original, llvm)
        if (native["windows_save"] != "11 0 16 0 0" or native["dll_sha256"] != HASH or
                hashlib.sha256(checked_file(original / "windows.skse", 16)).hexdigest() != INPUT_HASH):
            raise ValueError("native Windows reference differs")
        reopened = work / "reopen"
        observed = finish(original, shared, reopened, llvm)
        returned = checked_file(reopened / "returned.skse", 26)
        if (observed["windows_restore_and_save"] != "99 21 26 2 1" or not observed["foreign_section_preserved"]
                or hashlib.sha256(returned).hexdigest() != RETURN_HASH):
            raise ValueError("the original DLL could not complete the return trip on Windows")
        (evidence / "returned.skse").write_bytes(returned)
        result = {"run_id": record["run_id"], "app_reported_platform": platform, "bundle_id": BUNDLE,
                  "build": build, "os_version": record["os_version"], "app_completed_at": record["completed_at"],
                  "origin_evidence": "Classic Lab shared report, without hardware attestation",
                  "app_input": record["input_value"], "app_saved_after_revert": record["first_save_after_revert"],
                  "app_output": record["output_value"], "app_records_read": record["records_read"],
                  "app_callback_mask": record["callback_mask"],
                  "windows_dll_sha256": HASH, "input_sha256": INPUT_HASH, "shared_sha256": OUTPUT_HASH,
                  "windows_return_sha256": RETURN_HASH, "windows_original_saved": 16,
                  "windows_fresh_plugin_handle": 99, "windows_loaded": 21, "windows_saved": 26,
                  "unknown_section_preserved": True}
        (evidence / "windows-verification.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        # NT rename refuses an existing destination. Re-sharing the same run preserves the original evidence.
        destination = private / (record["run_id"].lower() + "-" + uuid.uuid4().hex[:8])
        evidence.rename(destination)
    return result, destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path, help="the .skse file shared by Classic Lab")
    parser.add_argument("report", type=Path, help="the JSON shared alongside the .skse file")
    parser.add_argument("--build", required=True, help="expected CFBundleVersion from the signed Classic Lab build")
    parser.add_argument("--simulator", action="store_true", help="require a simulator report for a control run")
    parser.add_argument("--llvm-bin", type=Path, default=Path("C:/Program Files/LLVM/bin"))
    args = parser.parse_args()
    try:
        result, destination = verify_return(args.save, args.report, args.build,
                "ios_simulator" if args.simulator else "ios_device", args.llvm_bin.resolve())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Returned Classic Lab test could not be verified: {error}\n")
    print(json.dumps(result | {"private_evidence": str(destination)}, indent=2))


if __name__ == "__main__":
    main()
