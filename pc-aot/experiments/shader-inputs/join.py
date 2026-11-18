#!/usr/bin/env python3
"""Join two private key manifests; never replay recipes or compile shaders.

The result measures only key presence in these two snapshots. It does not prove
FXP parsing, native safety, rendering semantics, observed use, or game coverage.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from study import SOURCE_HASHES, UPSTREAM

ROOT = Path(__file__).resolve().parents[2]
MAX_JSON_BYTES = 64 << 20
MAX_JSON_DEPTH = 32
MAX_JSON_ITEMS = 2_000_000
MAX_ENTRIES = 16384
MAX_CANDIDATES = 65536
MAX_CACHE_RECORDS = 65536
MAX_ARCHIVE_BYTES = 64 << 20
MAX_MEMBER_BYTES = 16 << 20
MAX_SHADER_BYTES = 1 << 20
STAGES = {1: "VS", 2: "TCS", 4: "TES", 8: "GS", 16: "FS", 32: "CS"}
FLAGS = {
    "fxp_container_grammar_verified": False,
    "native_decoder_executed": False,
    "native_compiler_safety_verified": False,
    "runtime_coverage_verified": False,
    "production_option_parity_verified": False,
    "render_semantics_verified": False,
    "replay_authorized": False,
    "usage_frequency_measured": False,
    "game_rendering_verified": False,
    "bytecode_revalidated": False,
    "cache_payload_revalidated": False,
    "source_manifests_authenticated": False,
    "unresolved_keys_classified": False,
}


class Invalid(ValueError):
    pass


def need(condition, reason):
    if not condition:
        raise Invalid(reason)


def integer(value, low, high, reason):
    need(type(value) is int and low <= value <= high, reason)
    return value


def mapping(value, reason):
    need(type(value) is dict, reason)
    return value


def sequence(value, limit, reason):
    need(type(value) is list and len(value) <= limit, reason)
    return value


def hex_value(value, digits, reason):
    need(type(value) is str and re.fullmatch("[0-9a-f]{" + str(digits) + "}", value) is not None, reason)
    return value


def exact_false(document, fields):
    for name in FLAGS:
        if name in document:
            need(document[name] is False, "scope_flag:" + name)
    for name in fields:
        need(document.get(name) is False, "scope_flag:" + name)


def check_key(row, allowed=STAGES):
    mapping(row, "key_object")
    stage = integer(row.get("stage_vk"), 1, 32, "key_stage")
    need(stage in allowed and row.get("stage") == STAGES[stage], "key_stage")
    sha1 = hex_value(row.get("sha1"), 40, "key_sha1")
    key = STAGES[stage] + "_" + sha1
    need(row.get("dxvk_key") == key, "key_string")
    return key


def parse_json(raw, max_bytes=MAX_JSON_BYTES):
    """Bound text size, nesting, tokens and string size before JSON allocation."""
    need(type(raw) is bytes and 0 < len(raw) <= max_bytes, "json_byte_limit")
    depth = items = string_bytes = 0
    quoted = escaped = False
    for byte in raw:
        if quoted:
            string_bytes += 1
            need(string_bytes <= 4096, "json_string_limit")
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted, string_bytes = True, 0
        elif byte in (91, 123):
            depth += 1
            items += 1
            need(depth <= MAX_JSON_DEPTH, "json_depth_limit")
        elif byte in (93, 125):
            depth -= 1
            need(depth >= 0, "json_syntax")
        elif byte in (44, 58):
            items += 1
        need(items <= MAX_JSON_ITEMS, "json_item_limit")
    need(depth == 0 and not quoted, "json_syntax")

    def pairs(values):
        result = {}
        for key, value in values:
            need(key not in result, "json_duplicate_field")
            result[key] = value
        return result

    def number(value):
        need(len(value.lstrip("-")) <= 20, "json_integer_limit")
        return int(value)

    def not_integer(_):
        raise Invalid("json_noninteger_number")

    try:
        result = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs,
                            parse_int=number, parse_float=not_integer, parse_constant=not_integer)
    except (UnicodeError, json.JSONDecodeError, RecursionError):
        raise Invalid("json_syntax") from None
    return mapping(result, "json_root")


def validate_inventory(document):
    mapping(document, "inventory_object")
    need(type(document.get("schema")) is int and document["schema"] == 1, "inventory_schema")
    need(document.get("status") == "completed", "inventory_incomplete")
    need(document.get("upstream_revision") == UPSTREAM, "inventory_pin")
    exact_false(document, ("fxp_container_grammar_verified", "native_decoder_executed",
                           "runtime_coverage_verified", "production_option_parity_verified"))
    need(type(document.get("extracted")) is bool, "inventory_extracted_flag")
    source = mapping(document.get("input"), "inventory_input")
    archive_bytes = integer(source.get("bytes"), 36, MAX_ARCHIVE_BYTES, "inventory_input_size")
    before = hex_value(source.get("sha256_before"), 64, "inventory_input_sha256")
    need(hex_value(source.get("sha256_after"), 64, "inventory_input_sha256") == before,
         "inventory_input_changed")
    sources = mapping(document.get("key_algorithm_sources"), "inventory_key_sources")
    need(set(sources) == set(SOURCE_HASHES), "inventory_key_sources")
    for name, expected in SOURCE_HASHES.items():
        evidence = mapping(sources[name], "inventory_key_source")
        hex_value(evidence.get("sha256"), 64, "inventory_source_sha256")
        need(evidence.get("normalized_sha256") == expected, "inventory_key_source_changed")
    archive = mapping(document.get("archive"), "inventory_archive")
    need(type(archive.get("version")) is int and archive["version"] == 104, "inventory_archive_version")
    flags = integer(archive.get("flags"), 0, 0xffffffff, "inventory_archive_flags")
    need(flags & 3 == 3 and flags & ~(3 | 4 | 0x100) == 0, "inventory_archive_flags")
    metadata_end = integer(archive.get("metadata_bytes"), 36, archive_bytes, "inventory_metadata")
    entries = sequence(document.get("entries"), MAX_ENTRIES, "inventory_entries_limit")
    counts, stages, rejections, sha256s = Counter(), Counter(), Counter(), {}
    names, payloads = set(), []
    decoded_total = marker_total = accepted_total = 0
    for ordinal, entry in enumerate(entries):
        mapping(entry, "inventory_entry")
        need(type(entry.get("ordinal")) is int and entry["ordinal"] == ordinal, "inventory_ordinal")
        name = entry.get("name")
        need(type(name) is str and 0 < len(name) <= 1024, "inventory_member_name")
        need(name.casefold() not in names, "inventory_duplicate_member")
        names.add(name.casefold())
        offset = integer(entry.get("offset"), metadata_end, archive_bytes, "inventory_member_offset")
        stored = integer(entry.get("stored_bytes"), 1, MAX_MEMBER_BYTES, "inventory_member_size")
        need(stored <= archive_bytes - offset, "inventory_member_bounds")
        payloads.append((offset, offset + stored))
        need(type(entry.get("compressed")) is bool and type(entry.get("embedded_name")) is bool,
             "inventory_member_flags")
        need(entry["embedded_name"] == bool(flags & 0x100), "inventory_member_flags")
        decoded = integer(entry.get("decoded_bytes"), 0, MAX_MEMBER_BYTES, "inventory_decoded_size")
        decoded_total += decoded
        need(decoded_total <= MAX_ARCHIVE_BYTES, "inventory_total_decoded_limit")
        hex_value(entry.get("sha256"), 64, "inventory_member_sha256")
        records = sequence(entry.get("records"), MAX_CANDIDATES, "inventory_record_limit")
        marker_total += len(records)
        need(marker_total <= MAX_CANDIDATES, "inventory_record_limit")
        previous_offset, accepted_end = -1, 0
        for row in records:
            mapping(row, "inventory_record")
            start = integer(row.get("record_offset"), 0, max(0, decoded - 4), "inventory_record_offset")
            need(decoded >= 4 and start > previous_offset, "inventory_record_order")
            previous_offset = start
            need(type(row.get("bytecode_offset")) is int and row["bytecode_offset"] == start + 12,
                 "inventory_bytecode_offset")
            status = row.get("status")
            need(status in ("key_candidate", "rejected"), "inventory_record_status")
            if status == "rejected":
                reason = row.get("reason")
                need(type(reason) is str and re.fullmatch(r"[a-z][a-z0-9_]{0,63}", reason), "inventory_rejection_reason")
                rejections[reason] += 1
            if status == "key_candidate" or any(field in row for field in ("stage", "stage_vk", "sha1", "sha256", "dxvk_key")):
                key = check_key(row, {1, 16})
                digest = hex_value(row.get("sha256"), 64, "candidate_sha256")
                need(row.get("marker") == ("BSVS" if row["stage_vk"] == 1 else "BSPS"), "candidate_marker_stage")
                integer(row.get("container_key"), 0, 0xffffffff, "candidate_container_key")
                size = integer(row.get("declared_bytes"), 8, MAX_SHADER_BYTES, "candidate_size")
                need(size % 4 == 0 and type(row.get("analyzed_bytes")) is int and row["analyzed_bytes"] == size,
                     "candidate_analyzed_size")
                need(type(row.get("trailing_bytes")) is int and row["trailing_bytes"] == 0, "candidate_trailing_bytes")
                need(size <= decoded - row["bytecode_offset"], "candidate_member_bounds")
                if status == "key_candidate":
                    need(start >= accepted_end, "candidate_overlap")
                    accepted_end = row["bytecode_offset"] + size
                    need(key not in sha256s or sha256s[key] == digest, "candidate_sha1_collision")
                    sha256s[key] = digest
                    counts[key] += 1
                    stages[row["stage"]] += 1
                    accepted_total += 1
    previous_end = metadata_end
    for start, end in sorted(payloads):
        need(start >= previous_end, "inventory_payload_overlap")
        previous_end = end
    expected = {"archive_entries": len(entries), "decoded_bytes": decoded_total,
                "markers": marker_total, "key_candidates": accepted_total,
                "unique_stage_sha1_keys": len(counts), "stages": dict(stages), "rejections": dict(rejections)}
    actual = mapping(document.get("summary"), "inventory_summary")
    # Equality alone would accept True as an integer count.
    for name, value in expected.items():
        need(type(actual.get(name)) is type(value) and actual[name] == value, "inventory_summary:" + name)
        if type(value) is dict:
            need(all(type(count) is int for count in actual[name].values()), "inventory_summary:" + name)
    return counts


def validate_cache(document):
    mapping(document, "cache_object")
    need(type(document.get("schema")) is int and document["schema"] == 1, "cache_schema")
    need(document.get("status") == "indexed", "cache_incomplete")
    need(type(document.get("format_version")) is int and document["format_version"] == 10, "cache_format_version")
    need(document.get("upstream") == UPSTREAM, "cache_pin")
    exact_false(document, ("render_semantics_verified", "replay_authorized",
                           "usage_frequency_measured", "game_rendering_verified"))
    size = integer(document.get("file_bytes"), 12, MAX_ARCHIVE_BYTES, "cache_file_size")
    hex_value(document.get("sha256"), 64, "cache_sha256")
    records = sequence(document.get("records"), MAX_CACHE_RECORDS, "cache_record_limit")
    counts, recipe_shapes, previous_end = Counter(), {}, 12
    for ordinal, record in enumerate(records):
        mapping(record, "cache_record")
        need(type(record.get("ordinal")) is int and record["ordinal"] == ordinal, "cache_ordinal")
        need(type(record.get("offset")) is int and record["offset"] == previous_end, "cache_record_offset")
        payload_bytes = integer(record.get("payload_bytes"), 1, 1024, "cache_payload_size")
        previous_end += 24 + payload_bytes
        need(previous_end <= size, "cache_record_bounds")
        recipe = hex_value(record.get("recipe_sha256"), 64, "cache_recipe_sha256")
        keys = sequence(record.get("keys"), 6, "cache_keys")
        need(keys, "cache_keys")
        identities = tuple(check_key(key) for key in keys)
        stages = [key["stage_vk"] for key in keys]
        need(stages == sorted(set(stages)), "cache_stage_order")
        compute = stages == [32]
        need(32 not in stages or compute, "cache_mixed_compute")
        need(record.get("kind") == ("compute" if compute else "graphics"), "cache_kind")
        attributes = integer(record.get("attributes"), 0, 32, "cache_attributes")
        bindings = integer(record.get("bindings"), 0, 32, "cache_bindings")
        constants = integer(record.get("specialization_count"), 0, 12, "cache_specializations")
        need(not compute or attributes == bindings == 0, "cache_compute_layout")
        expected_size = len(keys) * 24 + (52 if compute else 135 + (attributes + bindings) * 4) + constants * 4
        need(payload_bytes == expected_size, "cache_payload_shape")
        shape = (identities, compute, attributes, bindings, constants)
        need(recipe not in recipe_shapes or recipe_shapes[recipe] == shape, "cache_recipe_key_conflict")
        recipe_shapes[recipe] = shape
        counts.update(identities)
    need(previous_end == size, "cache_file_size_mismatch")
    need(type(document.get("record_count")) is int and document["record_count"] == len(records), "cache_record_count")
    need(type(document.get("unique_recipes")) is int and document["unique_recipes"] == len(recipe_shapes), "cache_unique_recipes")
    supplied = mapping(document.get("key_recipe_counts"), "cache_key_counts")
    need(all(type(value) is int for value in supplied.values()) and supplied == dict(counts), "cache_key_counts")
    return records, counts


def coverage(present, total):
    return {"all_keys_present": present, "total": total,
            "fraction": present / total if total else None}


def join_documents(inventory, cache):
    candidates = validate_inventory(inventory)
    records, cache_counts = validate_cache(cache)
    joined, unique_recipes, missing_recipes = [], {}, {}
    for record in records:
        unknown = [key["dxvk_key"] for key in record["keys"] if key["dxvk_key"] not in candidates]
        present = not unknown
        recipe = record["recipe_sha256"]
        joined.append({"ordinal": record["ordinal"], "recipe_sha256": recipe,
                       "key_count": len(record["keys"]), "all_keys_present": present,
                       "unresolved_keys": unknown})
        unique_recipes[recipe] = present
        for key in unknown:
            missing_recipes.setdefault(key, set()).add(recipe)
    duplicates = {key: count for key, count in candidates.items() if count > 1}
    unresolved = [{"dxvk_key": key, "stage": key.split("_", 1)[0],
                   "cache_record_references": cache_counts[key], "unique_recipe_references": len(recipes)}
                  for key, recipes in sorted(missing_recipes.items())]
    return {"schema": 1, "status": "joined", "upstream": UPSTREAM, "cache_format_version": 10,
            **FLAGS, "scope": "Key presence only; unresolved origins are unknown and record counts are not usage frequency.",
            "candidate_records": sum(candidates.values()), "candidate_unique_keys": len(candidates),
            "candidate_duplicate_occurrences": sum(count - 1 for count in duplicates.values()),
            "candidate_duplicate_key_groups": len(duplicates),
            "candidate_max_multiplicity": max(candidates.values(), default=0),
            "candidate_duplicates": dict(sorted(duplicates.items())),
            "cache_records": coverage(sum(row["all_keys_present"] for row in joined), len(joined)),
            "unique_cache_recipes": coverage(sum(unique_recipes.values()), len(unique_recipes)),
            "cache_unique_keys": len(cache_counts), "cache_unique_keys_present": len(set(cache_counts) & set(candidates)),
            "unresolved_unique_keys": len(unresolved), "unresolved": unresolved, "records": joined}


def private_output(path):
    root = ROOT.resolve()
    allowed = root / "private" / "analysis"
    resolved = Path(path).expanduser().absolute().resolve()
    need(resolved != allowed and resolved.is_relative_to(allowed), "output_must_be_private_analysis")
    resolved.mkdir(parents=True, exist_ok=False)
    need(resolved.resolve().is_relative_to(allowed), "output_changed")
    return resolved


def read_snapshot(path):
    path = Path(path).expanduser().absolute()
    need(not path.is_symlink(), "input_symlink")
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        need(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= MAX_JSON_BYTES, "input_size_or_type")
        raw = stream.read(MAX_JSON_BYTES + 1)
        after = os.fstat(stream.fileno())
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    need(identity(before) == identity(after) and len(raw) == before.st_size, "input_changed_during_read")
    return raw, identity(after)


def inspect(inventory_path, index_path, output):
    output = private_output(output)
    result = {"schema": 1, "status": "rejected", **FLAGS}
    try:
        own_source = Path(__file__).read_bytes()
        (output / "join-source.py").write_bytes(own_source)
        helper_source = Path(__file__).with_name("study.py").read_bytes()
        (output / "study-source.py").write_bytes(helper_source)
        inventory_raw, inventory_identity = read_snapshot(inventory_path)
        index_raw, index_identity = read_snapshot(index_path)
        result = join_documents(parse_json(inventory_raw), parse_json(index_raw))
        for path, raw, identity in ((inventory_path, inventory_raw, inventory_identity),
                                    (index_path, index_raw, index_identity)):
            after, after_identity = read_snapshot(path)
            need(identity == after_identity and hashlib.sha256(raw).digest() == hashlib.sha256(after).digest(),
                 "input_changed_during_join")
        result["inputs"] = {"inventory_json_sha256": hashlib.sha256(inventory_raw).hexdigest(),
                            "index_json_sha256": hashlib.sha256(index_raw).hexdigest()}
        result["tool_sha256"] = hashlib.sha256(own_source).hexdigest()
        result["key_reference_helper_sha256"] = hashlib.sha256(helper_source).hexdigest()
    except Invalid as error:
        result = {"schema": 1, "status": "rejected", **FLAGS, "reason": str(error)}
    except OSError as error:
        result = {"schema": 1, "status": "rejected", **FLAGS, "reason": type(error).__name__}
    with (output / "join.json").open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--index", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.inventory, args.index, args.output)
    except (Invalid, OSError) as error:
        parser.exit(2, type(error).__name__ + ": output was not started\n")
    print("Private key join " + result["status"] + "; no runtime coverage or replay claimed.")
    return 0 if result["status"] == "joined" else 1


if __name__ == "__main__":
    raise SystemExit(main())
