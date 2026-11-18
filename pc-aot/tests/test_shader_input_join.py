"""Authored key snapshots only: these tests never read a device/game cache."""
from collections import Counter
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "experiments/shader-inputs"


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, TOOLS / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


with patch.object(sys, "path", [str(TOOLS)] + sys.path):
    join = load("shader_input_join", "join.py")
    index = load("shader_input_join_index", "cache_index.py")
    import study


HEADER = struct.pack("<4sII", b"DXVK", 10, 0)


def code(stage=1, nops=0):
    return struct.pack("<" + "I" * (nops + 2), 0xfffe0300 if stage == 1 else 0xffff0300,
                       *([0] * nops), 0xffff)


def key(stage=1, data=None):
    data = code(stage) if data is None else data
    sha1 = hashlib.sha1(data).hexdigest()
    return {"stage_vk": stage, "stage": join.STAGES[stage], "sha1": sha1,
            "dxvk_key": join.STAGES[stage] + "_" + sha1}


def inventory(codes=None, suffix=b""):
    codes = [(1, code()), (1, code()), (16, code(16))] if codes is None else codes
    payload = b"".join(struct.pack("<4sII", b"BSVS" if stage == 1 else b"BSPS", ordinal, len(raw)) + raw
                       for ordinal, (stage, raw) in enumerate(codes)) + suffix
    rows = study.scan_member(payload)
    accepted = [r for r in rows if r["status"] == "key_candidate"]
    fingerprint = hashlib.sha256(payload).hexdigest()
    return {"schema": 1, "status": "completed", "upstream_revision": join.UPSTREAM,
            "fxp_container_grammar_verified": False, "native_decoder_executed": False,
            "runtime_coverage_verified": False, "production_option_parity_verified": False,
            "extracted": False, "input": {"bytes": 128 + len(payload), "sha256_before": fingerprint,
                                           "sha256_after": fingerprint},
            "key_algorithm_sources": {name: {"sha256": value, "normalized_sha256": value}
                                      for name, value in join.SOURCE_HASHES.items()},
            "archive": {"version": 104, "flags": 3, "metadata_bytes": 128},
            "entries": [{"ordinal": 0, "name": "authored/DO_NOT_COPY_THIS_NAME.fxp", "offset": 128,
                         "stored_bytes": len(payload), "compressed": False, "embedded_name": False,
                         "decoded_bytes": len(payload), "sha256": fingerprint, "records": rows}],
            "summary": {"archive_entries": 1, "decoded_bytes": len(payload), "markers": len(rows),
                        "key_candidates": len(accepted), "unique_stage_sha1_keys": len({r["dxvk_key"] for r in accepted}),
                        "stages": dict(Counter(r["stage"] for r in accepted)),
                        "rejections": dict(Counter(r["reason"] for r in rows if r["status"] == "rejected"))}}


def cache_record(keys, constant=None):
    mask = sum(row["stage_vk"] for row in keys)
    payload = b"".join(struct.pack("<I", row["stage_vk"]) + bytes.fromhex(row["sha1"]) for row in keys)
    payload += bytes(48) if mask == 32 else bytes([1, 0, 0]) + bytes(128)
    payload += struct.pack("<I", 0 if constant is None else 1)
    if constant is not None:
        payload += struct.pack("<I", constant)
    return struct.pack("<I", mask | (len(payload) << 8)) + hashlib.sha1(payload).digest() + payload


def cache(*recipes):
    return index.parse_cache(HEADER + b"".join(cache_record(keys) for keys in recipes))


class JoinTests(unittest.TestCase):
    def reject(self, inv, idx, reason):
        with self.assertRaisesRegex(join.Invalid, reason):
            join.join_documents(inv, idx)

    def test_presence_and_recipe_multiplicity_are_separate(self):
        inv = inventory()
        known = [key(), key(16)]
        absent = key(16, code(16, 2))
        compute = key(32, b"authored-compute-key")
        report = join.join_documents(inv, cache(known, known, [key(), absent], [compute]))
        self.assertEqual(report["candidate_records"], 3)
        self.assertEqual(report["candidate_unique_keys"], 2)
        self.assertEqual(report["candidate_duplicate_occurrences"], 1)
        self.assertEqual(report["candidate_duplicate_key_groups"], 1)
        self.assertEqual(report["candidate_max_multiplicity"], 2)
        self.assertEqual(report["cache_records"], {"all_keys_present": 2, "total": 4, "fraction": 0.5})
        self.assertEqual(report["unique_cache_recipes"], {"all_keys_present": 1, "total": 3, "fraction": 1 / 3})
        self.assertEqual(report["unresolved_unique_keys"], 2)
        self.assertEqual({r["dxvk_key"] for r in report["unresolved"]}, {absent["dxvk_key"], compute["dxvk_key"]})
        for name in join.FLAGS:
            self.assertIs(report[name], False)
        self.assertNotIn("DO_NOT_COPY_THIS_NAME", json.dumps(report))
        self.assertNotIn("fixed-function", json.dumps(report))

    def test_same_hash_in_another_stage_does_not_match(self):
        result = join.join_documents(inventory([(1, code())]), cache([key(16, code())]))
        self.assertEqual(result["cache_records"]["all_keys_present"], 0)
        self.assertEqual(result["unresolved_unique_keys"], 1)

    def test_empty_cache_is_not_reported_as_full_coverage(self):
        result = join.join_documents(inventory(), cache())
        self.assertEqual(result["cache_records"], {"all_keys_present": 0, "total": 0, "fraction": None})
        self.assertEqual(result["unique_cache_recipes"]["fraction"], None)

    def test_rejected_candidates_do_not_participate(self):
        result = join.join_documents(inventory([], b"BSVS"), cache([key()]))
        self.assertEqual(result["candidate_unique_keys"], 0)
        self.assertEqual(result["candidate_max_multiplicity"], 0)
        self.assertEqual(result["unresolved_unique_keys"], 1)

    def test_unresolved_reference_counts_are_not_frequency(self):
        missing = [key(16, code(16, 3))]
        raw = HEADER + cache_record(missing) * 3 + cache_record(missing, 2)
        result = join.join_documents(inventory(), index.parse_cache(raw))
        self.assertEqual(result["unresolved"][0]["cache_record_references"], 4)
        self.assertEqual(result["unresolved"][0]["unique_recipe_references"], 2)
        self.assertIs(result["usage_frequency_measured"], False)

    def test_wrong_version_pin_status_and_nonboolean_scope_are_rejected(self):
        for field, value, reason in (("schema", True, "inventory_schema"), ("schema", 2, "inventory_schema"),
                                     ("upstream_revision", "0" * 40, "inventory_pin"), ("status", "failed", "inventory_incomplete"),
                                     ("extracted", 0, "inventory_extracted_flag"), ("native_decoder_executed", 0, "scope_flag"),
                                     ("runtime_coverage_verified", True, "scope_flag"), ("replay_authorized", True, "scope_flag")):
            changed = inventory()
            changed[field] = value
            with self.subTest(field=field, value=value):
                self.reject(changed, cache(), reason)
        for field, value, reason in (("schema", 0, "cache_schema"), ("format_version", 9, "cache_format_version"),
                                     ("upstream", "0" * 40, "cache_pin"), ("status", "rejected", "cache_incomplete"),
                                     ("render_semantics_verified", True, "scope_flag"), ("usage_frequency_measured", 0, "scope_flag")):
            changed = cache()
            changed[field] = value
            with self.subTest(field=field):
                self.reject(inventory(), changed, reason)

    def test_declared_source_guards_and_archive_flags_are_checked(self):
        changed = inventory()
        next(iter(changed["key_algorithm_sources"].values()))["normalized_sha256"] = "0" * 64
        self.reject(changed, cache(), "inventory_key_source_changed")
        for field, value, reason in (("version", 105, "inventory_archive_version"), ("flags", 0x43, "inventory_archive_flags")):
            changed = inventory()
            changed["archive"][field] = value
            self.reject(changed, cache(), reason)
        changed = inventory()
        changed["input"]["sha256_after"] = "0" * 64
        self.reject(changed, cache(), "inventory_input_changed")

    def test_key_hex_stage_marker_and_string_must_agree(self):
        for field, value, reason in (("sha1", "A" * 40, "key_sha1"), ("sha1", "0" * 39, "key_sha1"),
                                     ("sha256", "g" * 64, "candidate_sha256"), ("stage_vk", True, "key_stage"),
                                     ("stage", "CS", "key_stage"), ("dxvk_key", "VS_" + "0" * 40, "key_string"),
                                     ("marker", "BSPS", "candidate_marker_stage")):
            changed = inventory()
            changed["entries"][0]["records"][0][field] = value
            with self.subTest(field=field):
                self.reject(changed, cache(), reason)

    def test_candidate_collision_offsets_and_length_are_rejected(self):
        changed = inventory()
        changed["entries"][0]["records"][1]["sha256"] = "0" * 64
        self.reject(changed, cache(), "candidate_sha1_collision")
        changed = inventory()
        row = changed["entries"][0]["records"][1]
        row["record_offset"], row["bytecode_offset"] = 4, 16
        self.reject(changed, cache(), "candidate_overlap")
        for field, value, reason in (("trailing_bytes", 4, "candidate_trailing_bytes"),
                                     ("analyzed_bytes", 12, "candidate_analyzed_size"),
                                     ("bytecode_offset", 13, "inventory_bytecode_offset")):
            changed = inventory()
            changed["entries"][0]["records"][0][field] = value
            self.reject(changed, cache(), reason)

    def test_inventory_and_cache_counts_are_recomputed(self):
        for field in ("markers", "key_candidates", "unique_stage_sha1_keys", "archive_entries"):
            changed = inventory()
            changed["summary"][field] = 999
            self.reject(changed, cache(), "inventory_summary")
        changed = inventory()
        changed["summary"]["archive_entries"] = True
        self.reject(changed, cache(), "inventory_summary")
        for field, value, reason in (("record_count", 3, "cache_record_count"), ("unique_recipes", 3, "cache_unique_recipes"),
                                     ("key_recipe_counts", {}, "cache_key_counts"), ("file_bytes", 9999, "cache_file_size_mismatch")):
            changed = cache([key()])
            changed[field] = value
            self.reject(inventory(), changed, reason)

    def test_cache_record_layout_and_duplicate_digest_consistency(self):
        for field, value, reason in (("ordinal", 1, "cache_ordinal"), ("offset", 13, "cache_record_offset"),
                                     ("payload_bytes", 1024, "cache_record_bounds"), ("kind", "compute", "cache_kind"),
                                     ("attributes", 1, "cache_payload_shape"), ("specialization_count", 13, "cache_specializations")):
            changed = cache([key()])
            changed["records"][0][field] = value
            self.reject(inventory(), changed, reason)
        changed = cache([key()], [key(16)])
        changed["records"][1]["recipe_sha256"] = changed["records"][0]["recipe_sha256"]
        self.reject(inventory(), changed, "cache_recipe_key_conflict")
        changed = cache([key(), key(16)])
        changed["records"][0]["keys"].reverse()
        self.reject(inventory(), changed, "cache_stage_order")

    def test_entry_and_candidate_limits_are_enforced(self):
        with patch.object(join, "MAX_CANDIDATES", 2):
            self.reject(inventory(), cache(), "inventory_record_limit")
        with patch.object(join, "MAX_ENTRIES", 0):
            self.reject(inventory(), cache(), "inventory_entries_limit")
        with patch.object(join, "MAX_CACHE_RECORDS", 0):
            self.reject(inventory(), cache([key()]), "cache_record_limit")


class JsonBoundaryTests(unittest.TestCase):
    def test_duplicate_fields_including_nested_and_escaped_names_are_rejected(self):
        for data in (b'{"schema":1,"schema":2}', b'{"outer":{"stage":1,"stage":16}}', b'{"x":1,"\\u0078":2}'):
            with self.subTest(data=data), self.assertRaisesRegex(join.Invalid, "json_duplicate_field"):
                join.parse_json(data)

    def test_noninteger_numbers_deep_wide_long_and_oversize_are_rejected(self):
        for data in (b'{"x":NaN}', b'{"x":Infinity}', b'{"x":1.5}', b'{"x":1e2}'):
            with self.assertRaisesRegex(join.Invalid, "json_noninteger_number"):
                join.parse_json(data)
        with self.assertRaisesRegex(join.Invalid, "json_integer_limit"):
            join.parse_json(b'{"x":111111111111111111111}')
        with self.assertRaisesRegex(join.Invalid, "json_depth_limit"):
            join.parse_json(b'[' * 33 + b'0' + b']' * 33)
        with patch.object(join, "MAX_JSON_ITEMS", 3), self.assertRaisesRegex(join.Invalid, "json_item_limit"):
            join.parse_json(b'{"x":[0,0,0,0]}')
        with self.assertRaisesRegex(join.Invalid, "json_string_limit"):
            join.parse_json(b'{"x":"' + b'x' * 4097 + b'"}')
        with self.assertRaisesRegex(join.Invalid, "json_byte_limit"):
            join.parse_json(b'{"x":1}', max_bytes=6)

    def test_string_brackets_escapes_utf8_and_invalid_syntax(self):
        self.assertEqual(join.parse_json(b'{"x":"[]{}\\\""}'), {"x": '[]{}"'})
        for data in (b'{"x":}', b'[]', b'{"x":"\xff"}', b'{"x":"unterminated}', b'{"x":0}}'):
            with self.assertRaises(join.Invalid):
                join.parse_json(data)


class PrivateRunTests(unittest.TestCase):
    def write_inputs(self, root):
        inv, idx = root / "inventory.json", root / "index.json"
        inv.write_text(json.dumps(inventory()), encoding="utf-8")
        idx.write_text(json.dumps(cache([key(), key(16)])), encoding="utf-8")
        return inv, idx

    def test_run_preserves_inputs_and_records_only_private_keys_and_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inv, idx = self.write_inputs(root)
            before = inv.read_bytes(), idx.read_bytes()
            out = root / "private/analysis/join"
            with patch.object(join, "ROOT", root):
                result = join.inspect(inv, idx, out)
            self.assertEqual(result["status"], "joined")
            self.assertEqual((inv.read_bytes(), idx.read_bytes()), before)
            self.assertEqual(json.loads((out / "join.json").read_text()), result)
            self.assertEqual(result["inputs"]["inventory_json_sha256"], hashlib.sha256(before[0]).hexdigest())
            self.assertEqual(result["tool_sha256"], hashlib.sha256((out / "join-source.py").read_bytes()).hexdigest())
            self.assertNotIn("DO_NOT_COPY_THIS_NAME", (out / "join.json").read_text())

    def test_failed_join_retains_reason_without_partial_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inv, idx = self.write_inputs(root)
            idx.write_bytes(b'{"schema":1,"schema":2}')
            out = root / "private/analysis/failure"
            with patch.object(join, "ROOT", root):
                result = join.inspect(inv, idx, out)
            self.assertEqual(result["status"], "rejected")
            self.assertEqual(result["reason"], "json_duplicate_field")
            self.assertNotIn("cache_records", result)
            self.assertTrue((out / "join.json").is_file())

    def test_existing_output_and_public_paths_are_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            out = root / "private/analysis/existing"
            out.mkdir(parents=True)
            sentinel = out / "join.json"
            sentinel.write_bytes(b"preserve")
            with patch.object(join, "ROOT", root):
                with self.assertRaises(FileExistsError):
                    join.inspect("unused", "unused", out)
                for path in (root / "public/new", root / "private/analysis"):
                    with self.assertRaisesRegex(join.Invalid, "output_must_be_private_analysis"):
                        join.inspect("unused", "unused", path)
            self.assertEqual(sentinel.read_bytes(), b"preserve")

    def test_private_root_symlink_cannot_escape_into_tracked_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "vendor").mkdir()
            (root / "private").mkdir()
            try:
                (root / "private/analysis").symlink_to(root / "vendor", target_is_directory=True)
            except OSError:
                self.skipTest("host cannot create symlink fixture")
            with patch.object(join, "ROOT", root), self.assertRaisesRegex(join.Invalid, "output_must_be_private_analysis"):
                join.inspect("unused", "unused", root / "private/analysis/new")
            self.assertFalse((root / "vendor/new").exists())

    def test_input_symlinks_and_oversize_are_rejected_before_parse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inv, idx = self.write_inputs(root)
            with patch.object(join, "MAX_JSON_BYTES", 8), self.assertRaisesRegex(join.Invalid, "input_size_or_type"):
                join.read_snapshot(inv)
            link = root / "linked.json"
            try:
                link.symlink_to(idx)
            except OSError:
                self.skipTest("host cannot create symlink fixture")
            with self.assertRaisesRegex(join.Invalid, "input_symlink"):
                join.read_snapshot(link)

    def test_change_between_input_reads_cannot_claim_join_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inv, idx = self.write_inputs(root)
            original, calls = join.read_snapshot, []

            def changed(path):
                if len(calls) == 2:
                    inv.write_text(inv.read_text() + " ", encoding="utf-8")
                calls.append(path)
                return original(path)

            with patch.object(join, "ROOT", root), patch.object(join, "read_snapshot", side_effect=changed):
                result = join.inspect(inv, idx, root / "private/analysis/changed")
            self.assertEqual(result["status"], "rejected")
            self.assertEqual(result["reason"], "input_changed_during_join")
            self.assertNotIn("cache_records", result)


if __name__ == "__main__":
    unittest.main()
