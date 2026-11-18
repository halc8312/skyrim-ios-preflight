#!/usr/bin/env python3
"""Bounded private BSA/SM3 key inventory; no native decoder or GPU execution.

Accepted records are key-matching candidates, not proof of complete FXP parsing,
D3D validity, compiler safety, runtime coverage, or effective pipeline settings.
Only the CLI writes files, exclusively to a fresh ignored private/analysis path.
"""
import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
# LF-normalized source guards allow Git's CRLF checkout conversion, but reject a
# changed decoder/key algorithm instead of claiming this fixed-source contract.
SOURCE_HASHES = {
    "src/dxso/dxso_header.cpp": "222788dc98e64f32222dfe06caae89d0904e7ca57c73dcfc35211a7aeb7f0858",
    "src/dxso/dxso_decoder.cpp": "33990f19fab4ad2b13445caa2ddd1067e399532911ff8d4025fbb9f8f7288194",
    "src/dxso/dxso_module.cpp": "0cb359082a0e4895a6420b71dee1154e4ca14e79dce3f8a33a9cb43d5aa63b5b",
    "src/dxso/dxso_analysis.cpp": "e3549a7f57e58c20fd69ccb6efe85d5c02a51f2016a69cbba60e21d8b01bb62c",
    "src/dxso/dxso_enums.h": "f4105a4cd7adab3c20bf2b52321c107f8bece8e47f4c11fd6b46a5d192729b61",
    "src/dxso/dxso_decoder.h": "80a9a59971e9fd9236cc576fd6194958f9cbd2cf4b2c7c978103d80382537268",
    "src/d3d9/d3d9_shader.cpp": "40e72c614732f25b99ba298fa3a3fd3dfec2cbbb771e05f4b6821b5478d166b4",
    "src/dxvk/dxvk_shader_key.cpp": "5d639d96e2a247154824c16c9bc41ddcd811e91c2128f79619d279804f40d782",
}
MARKER = re.compile(rb"BS(?:VS|PS)")
STAGES = {b"BSVS": (0xFFFE0300, 1, "VS"), b"BSPS": (0xFFFF0300, 16, "FS")}


@dataclass(frozen=True)
class Limits:
    archive_bytes: int = 64 << 20
    metadata_bytes: int = 4 << 20
    members: int = 16384
    folders: int = 4096
    member_bytes: int = 16 << 20
    decoded_bytes: int = 64 << 20
    shader_bytes: int = 1 << 20
    shader_instructions: int = 65536
    candidates: int = 65536
    validation_bytes: int = 64 << 20


@dataclass
class ValidationBudget:
    """Shared across archive members; overlapping markers still consume work."""
    used: int = 0


class Invalid(ValueError):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def need(condition, code):
    if not condition:
        raise Invalid(code)


def digest(data, algorithm="sha256"):
    return hashlib.new(algorithm, data).hexdigest()


def verify_key_sources(dependency):
    result = {}
    for name, expected in SOURCE_HASHES.items():
        raw = (dependency / name).read_bytes()
        actual = digest(raw.replace(b"\r\n", b"\n"))
        need(actual == expected, "key_algorithm_source_changed")
        result[name] = {"sha256": digest(raw), "normalized_sha256": actual}
    return result


class Reader:
    def __init__(self, data, position=0, end=None):
        self.data, self.position = data, position
        self.end = len(data) if end is None else end
        need(0 <= position <= self.end <= len(data), "reader_bounds")

    def take(self, size):
        need(0 <= size <= self.end - self.position, "truncated")
        start = self.position
        self.position += size
        return self.data[start:self.position]

    def unpack(self, layout):
        return struct.unpack(layout, self.take(struct.calcsize(layout)))


def virtual_name(raw, folder=False):
    need(raw and all(32 <= value < 127 for value in raw), "name_encoding")
    name = raw.decode("ascii").replace("\\", "/")
    parts = name.split("/")
    need(all(part and part not in (".", "..") and ":" not in part for part in parts), "name_path")
    need(folder or len(parts) == 1, "filename_path")
    return name


def read_bsa(data, limits=Limits()):
    """Read only the named, sequential-folder-block BSA v104 subset.

    Folder offsets in these archives include the filename-table length. Accept
    the direct-offset convention too, but do not seek using untrusted offsets.
    Payloads must be disjoint and entirely after the complete metadata region.
    """
    need(36 <= len(data) <= limits.archive_bytes, "archive_size")
    reader = Reader(data)
    magic, version, offset, flags, folders, files, folder_names, file_names, kinds = reader.unpack("<4s8I")
    need(magic == b"BSA\0" and version == 104, "archive_version")
    need(flags & 3 == 3 and flags & ~(3 | 4 | 0x100) == 0, "archive_flags")
    need(0 < folders <= limits.folders and 0 < files <= limits.members, "archive_counts")
    need(36 <= offset <= limits.metadata_bytes, "folder_table_offset")
    need(folder_names <= limits.metadata_bytes and file_names <= limits.metadata_bytes, "names_limit")
    reader = Reader(data, offset, min(len(data), limits.metadata_bytes))
    folder_table = [reader.unpack("<QII") for _ in range(folders)]
    need(sum(item[1] for item in folder_table) == files, "folder_file_count")
    items, names_seen, total_names, conventions = [], set(), 0, set()
    for _, count, declared_offset in folder_table:
        block = reader.position
        if declared_offset == block + file_names:
            conventions.add("includes_filename_table")
        elif declared_offset == block:
            conventions.add("direct")
        else:
            raise Invalid("folder_block_offset")
        length = reader.unpack("<B")[0]
        need(length > 1, "folder_name_length")
        raw_name = reader.take(length)
        need(raw_name[-1:] == b"\0" and b"\0" not in raw_name[:-1], "folder_name_terminator")
        folder = virtual_name(raw_name[:-1], folder=True)
        total_names += length
        for _ in range(count):
            _, encoded_size, begin = reader.unpack("<QII")
            need(not encoded_size & 0x80000000, "member_size_flags")
            size = encoded_size & ~0x40000000
            need(0 < size <= limits.member_bytes and begin <= len(data) - size, "member_bounds")
            items.append({"folder": folder, "offset": begin, "stored_bytes": size,
                          "compressed": bool(flags & 4) ^ bool(encoded_size & 0x40000000),
                          "embedded_name": bool(flags & 0x100)})
    need(total_names == folder_names, "folder_names_size")
    name_table = reader.take(file_names)
    names = name_table.split(b"\0", files + 1)
    need(len(names) == files + 1 and names[-1] == b"", "file_names_size")
    metadata_end = reader.position
    for ordinal, (item, raw_name) in enumerate(zip(items, names)):
        name = item.pop("folder") + "/" + virtual_name(raw_name)
        need(name.casefold() not in names_seen, "duplicate_member_name")
        names_seen.add(name.casefold())
        need(item["offset"] >= metadata_end, "payload_overlaps_metadata")
        item.update(ordinal=ordinal, name=name)
    end = metadata_end
    for item in sorted(items, key=lambda row: row["offset"]):
        need(item["offset"] >= end, "payload_overlap")
        end = item["offset"] + item["stored_bytes"]
    return {"version": version, "flags": flags, "file_types": kinds, "metadata_bytes": metadata_end,
            "folder_offset_conventions": sorted(conventions), "entries": items}


def decode_member(archive, entry, limits=Limits()):
    reader = Reader(archive, entry["offset"], entry["offset"] + entry["stored_bytes"])
    if entry["embedded_name"]:
        reader.take(reader.unpack("<B")[0])
    if not entry["compressed"]:
        payload = reader.take(reader.end - reader.position)
        need(len(payload) <= limits.member_bytes, "decoded_size_limit")
        return payload
    expected = reader.unpack("<I")[0]
    need(0 < expected <= limits.member_bytes, "decoded_size_limit")
    compressed = reader.take(reader.end - reader.position)
    decoder = zlib.decompressobj()
    try:
        # The cap applies during allocation, not after an unbounded decompress.
        payload = decoder.decompress(compressed, expected + 1)
    except zlib.error:
        raise Invalid("zlib_invalid") from None
    need(len(payload) <= expected and not decoder.unconsumed_tail, "zlib_output_exceeded")
    need(decoder.eof and not decoder.unused_data, "zlib_incomplete_or_trailing")
    need(len(payload) == expected, "zlib_size_mismatch")
    return payload


# Operand counts are a conservative SM3 envelope, not a complete D3D validator.
# The actual read order below follows pinned dxso_decoder.cpp. Legacy-only
# opcodes and unknown shapes remain explicit rejections rather than guessed.
ARITY = {
    0: (0,), 1: (2,), 2: (3,), 3: (3,), 4: (4,), 5: (3,), 6: (2,), 7: (2,),
    8: (3,), 9: (3,), 10: (3,), 11: (3,), 12: (3,), 13: (3,), 14: (2,),
    15: (2,), 16: (2,), 17: (3,), 18: (4,), 19: (2,), 20: (3,), 21: (3,),
    22: (3,), 23: (3,), 24: (3,), 25: (1,), 26: (2,), 27: (2,), 28: (0,),
    29: (0,), 30: (1,), 32: (3,), 33: (3,), 34: (2, 4), 35: (2,), 36: (2,),
    37: (2, 4), 38: (1,), 39: (0,), 40: (1,), 41: (2,), 42: (0,), 43: (0,),
    44: (0,), 45: (2,), 46: (2,), 65: (1,), 66: (3,), 78: (2,), 79: (2,),
    88: (4,), 90: (4,), 91: (2,), 92: (2,), 93: (5,), 94: (3,), 95: (3,), 96: (1,),
}
SOURCE_ONLY = {27, 38, 40, 41, 45, 96}


def validate_sm3(code, marker, limits=Limits(), require_exact_end=True):
    need(marker in STAGES, "stage_marker")
    need(8 <= len(code) <= limits.shader_bytes and len(code) % 4 == 0, "shader_size")
    version, stage, prefix = STAGES[marker]
    need(struct.unpack_from("<I", code)[0] == version, "shader_stage_or_version")
    words = memoryview(code).cast("B")
    count = len(code) // 4

    def word(index):
        need(0 <= index < count, "shader_truncated")
        return struct.unpack_from("<I", words, index * 4)[0]

    at, instructions, comments = 1, 0, 0
    opcodes = Counter()
    while at < count:
        token = word(at)
        opcode = token & 0xFFFF
        instructions += 1
        need(instructions <= limits.shader_instructions, "instruction_limit")
        at += 1
        if opcode == 0xFFFF:
            need(token == 0xFFFF, "end_flags")
            need(not require_exact_end or at == count, "shader_trailing_bytes")
            analyzed = at * 4  # version token and END are both included by DXSO.
            key_bytes = code[:analyzed]
            sha1 = digest(key_bytes, "sha1")
            return {"stage": prefix, "stage_vk": stage, "analyzed_bytes": analyzed,
                    "trailing_bytes": len(code) - analyzed, "sha1": sha1,
                    "sha256": digest(key_bytes), "dxvk_key": prefix + "_" + sha1,
                    "instructions_including_end": instructions, "comment_words": comments,
                    "opcodes": {str(op): amount for op, amount in sorted(opcodes.items())}}
        if opcode == 0xFFFE:
            need(not token & 0x80000000, "comment_flags")
            length = (token >> 16) & 0x7FFF
            need(length <= count - at, "comment_truncated")
            at += length
            comments += length
            continue
        need(opcode in ARITY or opcode in (31, 47, 48, 81), "unsupported_opcode")
        need(not token & 0xE0000000, "instruction_reserved_or_coissue")
        predicated = bool(token & (1 << 28))
        length = (token >> 24) & 15
        end = at + length
        need(end <= count, "instruction_truncated")
        cursor, logical, sources, predicate_read = at, 0, 0, False

        def take_word():
            nonlocal cursor
            need(cursor < end, "operand_exceeds_instruction")
            value = word(cursor)
            cursor += 1
            return value

        def register(relative=True):
            value = take_word()
            need(value & 0x80000000, "register_tag")
            reg_type = ((value >> 8) & 0x18) | ((value >> 28) & 7)
            need(reg_type <= 20, "register_type")
            extra = bool(value & (1 << 13)) and relative
            if extra:
                extension = take_word()
                need(extension & 0x80000000 and not extension & (1 << 13), "relative_register")
            return extra

        if opcode == 31:
            need(length == 2 and not predicated, "declaration_shape")
            take_word()
            need(not register(), "declaration_relative")
        elif opcode in (47, 48, 81):
            need(length == (2 if opcode == 47 else 5) and not predicated, "definition_shape")
            need(not register(), "definition_relative")
            for _ in range(length - 1):
                take_word()  # immediate values are not register tokens.
        else:
            i = 0
            while i < length:
                if opcode in SOURCE_ONLY:
                    sources += 1
                    extra = register()
                    logical += 1
                elif i == 0:
                    extra = register()
                    logical += 1
                elif i == 1 and predicated:
                    register(relative=False)
                    predicate_read, extra = True, False
                else:
                    sources += 1
                    extra = register()
                    logical += 1
                need(sources <= 8, "source_register_limit")
                i += 1 + int(extra)
            need(logical in ARITY[opcode], "operand_count")
            need(not predicated or predicate_read, "predicate_shape")
        need(cursor == end, "decoder_length_mismatch")
        at = end
        opcodes[opcode] += 1
    raise Invalid("shader_missing_end")


def scan_member(payload, limits=Limits(), validation_budget=None):
    """Find bounded candidates, explicitly retaining unknown FXP grammar status."""
    budget = ValidationBudget() if validation_budget is None else validation_budget
    records = []
    for match in MARKER.finditer(payload):
        need(len(records) < limits.candidates, "candidate_limit")
        start = match.start()
        row = {"record_offset": start, "bytecode_offset": start + 12, "status": "rejected"}
        try:
            need(start + 12 <= len(payload), "record_header_truncated")
            marker, key, size = struct.unpack_from("<4sII", payload, start)
            row.update(marker=marker.decode("ascii"), container_key=key, declared_bytes=size)
            need(size <= len(payload) - (start + 12), "record_payload_truncated")
            need(8 <= size <= limits.shader_bytes, "shader_size")
            need(size <= limits.validation_bytes - budget.used, "validation_work_limit")
            budget.used += size
            row.update(validate_sm3(payload[start + 12:start + 12 + size], marker, limits))
            row["status"] = "key_candidate"
        except Invalid as error:
            if error.code == "validation_work_limit":
                raise  # A work limit terminates the study, not just one record.
            row["reason"] = error.code
        records.append(row)
    # A marker inside an accepted bytecode/comment must not become a second
    # independently trusted record. Reject all overlapping accepted candidates.
    group, group_end = [], 0

    def close_group():
        if len(group) > 1:
            for overlapping in group:
                overlapping["overlap"] = True

    for row in records:
        if row["status"] != "key_candidate":
            continue
        if row["record_offset"] >= group_end:
            close_group()
            group = []
        group.append(row)
        group_end = max(group_end, row["bytecode_offset"] + row["declared_bytes"])
    close_group()
    for row in records:
        if row.pop("overlap", False):
            row["status"], row["reason"] = "rejected", "candidate_overlap"
    return records


def private_output(path, root=ROOT):
    # Keep the allowed anchor lexical: resolving a junction here would approve
    # private/analysis redirected into a tracked directory such as vendor.
    base = root.resolve() / "private" / "analysis"
    path = Path(path).expanduser().absolute()
    resolved = path.resolve()
    need(resolved != base and resolved.is_relative_to(base), "output_must_be_private_analysis")
    # Fresh mkdir refuses files, existing directories, and dangling symlinks.
    path.mkdir(parents=True, exist_ok=False)
    need(path.resolve().is_relative_to(base), "output_changed")
    return path.resolve()


def read_snapshot(path, limits):
    path = Path(path)
    need(not path.is_symlink(), "input_symlink")
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        need(stat.S_ISREG(before.st_mode) and 36 <= before.st_size <= limits.archive_bytes, "input_size_or_type")
        data = stream.read(limits.archive_bytes + 1)
        after = os.fstat(stream.fileno())
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    need(identity(before) == identity(after) and len(data) == before.st_size, "input_changed_during_read")
    return data, identity(after)


def run(args):
    limits = Limits()
    output = private_output(args.output)
    report = {"schema": 1, "status": "running", "limits": asdict(limits), "entries": [],
              "scope": "Bounded SM3 key candidates; FXP grammar, native safety and runtime coverage unverified",
              "fxp_container_grammar_verified": False, "native_decoder_executed": False,
              "runtime_coverage_verified": False, "production_option_parity_verified": False,
              "extracted": bool(args.extract), "upstream_revision": UPSTREAM}
    started = time.monotonic_ns()
    archive = None
    try:
        tool_bytes = Path(__file__).read_bytes()
        (output / "study-source.py").write_bytes(tool_bytes)
        report["tool"] = {"sha256": digest(tool_bytes), "snapshot": "study-source.py",
                          "python": sys.version.split()[0]}
        source = Path(args.archive).expanduser().absolute()
        archive, identity = read_snapshot(source, limits)
        report["input"] = {"path": str(source), "bytes": len(archive), "sha256_before": digest(archive)}
        dependency = ROOT / "vendor/skydream/third_party/dxvk"
        report["key_algorithm_sources"] = verify_key_sources(dependency)
        catalog = read_bsa(archive, limits)
        report["archive"] = {key: value for key, value in catalog.items() if key != "entries"}
        decoded_total, candidate_total, emitted = 0, 0, {}
        validation_budget = ValidationBudget()
        if args.extract:
            (output / "bytecode").mkdir()
        for member in catalog["entries"]:
            payload = decode_member(archive, member, limits)
            decoded_total += len(payload)
            need(decoded_total <= limits.decoded_bytes, "total_decoded_limit")
            row = dict(member, decoded_bytes=len(payload), sha256=digest(payload), records=[])
            report["entries"].append(row)
            if not member["name"].lower().endswith(".fxp"):
                row["status"] = "unexamined_member_type"
                continue
            row["records"] = scan_member(payload, limits, validation_budget)
            candidate_total += len(row["records"])
            need(candidate_total <= limits.candidates, "total_candidate_limit")
            for candidate in row["records"]:
                if candidate["status"] != "key_candidate":
                    continue
                key = candidate["dxvk_key"]
                need(key not in emitted or emitted[key] == candidate["sha256"], "sha1_collision")
                if args.extract and key not in emitted:
                    begin = candidate["bytecode_offset"]
                    with (output / "bytecode" / (key + ".dxso")).open("xb") as stream:
                        stream.write(payload[begin:begin + candidate["analyzed_bytes"]])
                emitted[key] = candidate["sha256"]
        after, after_identity = read_snapshot(source, limits)
        report["input"]["sha256_after"] = digest(after)
        need(identity == after_identity and digest(archive) == digest(after), "input_changed_during_study")
        records = [record for entry in report["entries"] for record in entry["records"]]
        accepted = [record for record in records if record["status"] == "key_candidate"]
        report["summary"] = {"archive_entries": len(report["entries"]), "decoded_bytes": decoded_total,
                             "markers": len(records), "key_candidates": len(accepted),
                             "unique_stage_sha1_keys": len(emitted), "stages": dict(Counter(r["stage"] for r in accepted)),
                             "rejections": dict(Counter(r["reason"] for r in records if r["status"] == "rejected")),
                             "validation_bytes": validation_budget.used}
        report["status"] = "completed"
    except (Invalid, OSError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
    except BaseException:
        report["status"], report["error"] = "interrupted", "interrupted_before_completion"
        raise
    finally:
        report["duration_ns"] = time.monotonic_ns() - started
        with (output / "inventory.json").open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--extract", action="store_true", help="write deduplicated candidate DXSO only inside the fresh private run")
    args = parser.parse_args(argv)
    try:
        result = run(args)
    except (Invalid, OSError) as error:
        print(f"Study could not start: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"status": result["status"], "summary": result.get("summary"),
                      "error": result.get("error"), "runtime_coverage_verified": False}))
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
