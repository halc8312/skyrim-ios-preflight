"""Read-only PE/section/address evidence. No unpacking, execution or compatibility approval."""
from __future__ import annotations

import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from steam_inputs import executable_record

ROOT = Path(__file__).resolve().parents[1]
WINDOW = 32


def source_locations():
    lock = json.loads((ROOT / "source-lock.json").read_text(encoding="utf-8"))
    source = next(s for s in lock["sources"] if s["name"] == "skydream")
    relative = "tools/lift.py"
    blob = subprocess.run(["git", "-C", str(ROOT / source["path"]), "show", f'{source["revision"]}:{relative}'],
                          check=True, capture_output=True, timeout=15).stdout
    for node in ast.parse(blob.decode("utf-8")).body:
        if isinstance(node, ast.Assign) and any(isinstance(n, ast.Name) and n.id == "HOST_OVERRIDES_SKYRIM" for n in node.targets):
            addresses = sorted(ast.literal_eval(node.value)["exe"])
            if len(addresses) != 16 or any(not isinstance(a, int) or not 0 <= a < 2**32 for a in addresses):
                raise ValueError("unexpected pinned Skyrim override contract; review required")
            return addresses, {"repository": source["url"], "revision": source["revision"], "file": relative,
                               "line": node.lineno, "source_sha256": hashlib.sha256(blob).hexdigest(),
                               "symbol": "HOST_OVERRIDES_SKYRIM", "addresses": [f"0x{a:08x}" for a in addresses],
                               "scope": "16 lifter overrides only; not a complete inventory of runtime fixed addresses",
                               "uncovered_motion_contract": {
                                   "source_file": "tools/prepare_physics_motion.py", "source_line": 8,
                                   "functions": ["0x00d1da60", "0x00d369e0", "0x00d64d70", "0x00d65860"],
                                   "clock_global_va": "0x012d18dc", "status": "not_evaluated",
                                   "requirement": "Review exact generated load-expression counts and semantic identities; these functions/global are outside this 16-window audit."}}
    raise ValueError("pinned Skyrim override definition missing")


def range_hash(stream, offset, count, size):
    if offset < 0 or count < 0 or offset > size or count > size - offset:
        raise ValueError("file range outside executable")
    stream.seek(offset)
    digest = hashlib.sha256()
    while count:
        chunk = stream.read(min(count, 1024 * 1024))
        if not chunk:
            raise ValueError("input truncated while hashing")
        digest.update(chunk)
        count -= len(chunk)
    return digest.hexdigest()


def read_layout(path, metadata):
    if metadata["machine"] != "0x014c" or metadata["bits"] != 32:
        raise ValueError("requires x86 PE32")
    size = path.stat().st_size
    with path.open("rb") as stream:
        def read(offset, count):
            if offset < 0 or count < 0 or offset > size or count > size - offset:
                raise ValueError("header or section table outside file")
            stream.seek(offset)
            data = stream.read(count)
            if len(data) != count:
                raise ValueError("input changed or truncated")
            return data
        pe = struct.unpack("<I", read(60, 4))[0]
        count = struct.unpack("<H", read(pe + 6, 2))[0]
        optional_size = struct.unpack("<H", read(pe + 20, 2))[0]
        if optional_size < 224 or not 1 <= count <= 96:
            raise ValueError("invalid PE32 header/section count")
        optional = read(pe + 24, optional_size)
        base, image_size, headers = (struct.unpack_from("<I", optional, n)[0] for n in (28, 56, 60))
        entry = struct.unpack_from("<I", optional, 16)[0]
        if base != 0x400000 or not 0 < entry < image_size <= 0x10000000:
            raise ValueError("unsupported Skyrim image base/entry/size")
        table = pe + 24 + optional_size
        if not table + count * 40 <= headers <= min(size, image_size):
            raise ValueError("section table outside bounded headers")
        sections = []
        spans = [(0, headers)]
        raw_spans = [(0, headers)]
        for index in range(count):
            row = read(table + 40 * index, 40)
            name = row[:8].split(b"\0")[0].decode("ascii", errors="replace")
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<4I", row, 8)
            memory_size = virtual_size or raw_size
            if rva > image_size or memory_size > image_size - rva:
                raise ValueError("section outside image")
            if memory_size and any(rva < end and start < rva + memory_size for start, end in spans):
                raise ValueError("overlapping virtual sections/headers")
            if raw_size and any(raw_offset < end and start < raw_offset + raw_size for start, end in raw_spans):
                raise ValueError("overlapping raw sections/headers")
            spans.append((rva, rva + memory_size))
            if raw_size:
                raw_spans.append((raw_offset, raw_offset + raw_size))
            sections.append({"index": index, "name": name, "rva": rva, "virtual_size": virtual_size,
                             "memory_size": memory_size, "raw_size": raw_size, "raw_offset": raw_offset,
                             "characteristics": struct.unpack_from("<I", row, 36)[0],
                             "raw_sha256": range_hash(stream, raw_offset, raw_size, size)})
        return {"image_size": image_size, "headers_size": headers, "entry_va": base + entry,
                "has_bind_section": any(s["name"] == ".bind" for s in sections), "sections": sections}


def address_evidence(path, layout, addresses):
    result = []
    with path.open("rb") as stream:
        for address in addresses:
            row = {"va": f"0x{address:08x}", "window_bytes": WINDOW}
            rva = address - 0x400000
            found = [s for s in layout["sections"] if s["rva"] <= rva < s["rva"] + s["memory_size"]]
            if not found:
                row["status"] = "unmapped"
            else:
                section = found[0]
                row.update(section=section["name"], executable=bool(section["characteristics"] & 0x20000000))
                delta = rva - section["rva"]
                if layout["has_bind_section"]:
                    row["status"] = "packed_image_not_hook_evidence"
                elif delta + WINDOW > min(section["raw_size"], section["memory_size"]):
                    row["status"] = "window_not_fully_file_backed"
                elif not row["executable"]:
                    row["status"] = "not_in_executable_section"
                else:
                    row.update(status="fingerprinted_not_verified", file_offset=section["raw_offset"] + delta)
                    row["sha256"] = range_hash(stream, row["file_offset"], WINDOW, path.stat().st_size)
            result.append(row)
    return result


def audit_image(path, addresses, role="candidate"):
    before = path.stat()
    record = executable_record(path, role)
    record["compatibility"] = "not_reviewed"
    record["limitations"] = ["installation_completion_not_verified", "imports_TLS_relocations_and_function_semantics_not_validated"]
    if "inspection_error" not in record:
        try:
            record["layout"] = read_layout(path, record["pe"])
            record["hook_windows"] = address_evidence(path, record["layout"], addresses)
        except (ValueError, struct.error) as error:
            record["layout_error"] = str(error)
    after = path.stat()
    record["stable_during_read"] = (before.st_size, before.st_mtime_ns, before.st_ino) == (after.st_size, after.st_mtime_ns, after.st_ino)
    if not record["stable_during_read"]:
        record["layout_error"] = "input changed during evidence collection; discard comparisons"
    return record


def compare(candidate, reference):
    if any(not r["stable_during_read"] or "layout" not in r or "layout_error" in r or r["layout"]["has_bind_section"] for r in (candidate, reference)):
        return {"status": "not_comparable", "compatibility": "not_reviewed"}
    other = {r["va"]: r for r in reference["hook_windows"]}
    windows = [{"va": r["va"], "matching_fingerprint": bool(r.get("sha256") and r.get("sha256") == other.get(r["va"], {}).get("sha256"))} for r in candidate["hook_windows"]]
    return {"status": "byte_identity_evidence_only", "whole_file_equal": candidate["sha256"] == reference["sha256"],
            "hook_windows": windows, "compatibility": "not_reviewed",
            "limitation": "Reference provenance and full function/data/ABI behavior require independent review; 32-byte agreement cannot establish compatibility."}


def save_report(report, output):
    output, root = output.resolve(), ROOT.resolve()
    if not any(output != root / n and output.is_relative_to(root / n) for n in ("local", "private", "generated")):
        raise ValueError("output must be a new directory under local/, private/ or generated/")
    output.mkdir(parents=True, exist_ok=False)
    destination = output / "pe-address-audit.json"
    with destination.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True, help="completed-install original or separately prepared unpacked input")
    parser.add_argument("--reference", type=Path, help="optional independently obtained unpacked reference; never trusted automatically")
    parser.add_argument("--output", type=Path, required=True, help="new private report directory")
    args = parser.parse_args(argv)
    try:
        addresses, provenance = source_locations()
        report = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(), "source": provenance,
                  "candidate": audit_image(args.exe, addresses), "compatibility": "not_reviewed", "executed": False}
        if args.reference:
            report["reference"] = audit_image(args.reference, addresses, "untrusted_reference")
            report["comparison"] = compare(report["candidate"], report["reference"])
        output = save_report(report, args.output)
    except (OSError, ValueError, KeyError, StopIteration, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "audit_failed", "error": str(error)}), file=sys.stderr)
        return 1
    invalid = any("inspection_error" in r or "layout_error" in r for r in (report["candidate"], report.get("reference", {})))
    print(json.dumps({"status": "invalid_or_unstable_input" if invalid else "evidence_collected_not_approved", "report": str(output), "compatibility": "not_reviewed"}))
    return 2 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
