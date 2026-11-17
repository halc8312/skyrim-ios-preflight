"""Authored malformed/synthetic fixtures only; no game inputs are read by tests."""
from dataclasses import replace
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("shader_input_study", ROOT / "experiments/shader-inputs/study.py")
study = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = study
SPEC.loader.exec_module(study)


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


VS, PS, END = 0xFFFE0300, 0xFFFF0300, 0xFFFF
DST, SRC, REL = 0x800F0000, 0x90E40000, 0xB0000000


def shader(version=VS):
    # mov r0, v0; the two register words are authored, not extracted.
    return words(version, 0x02000001, DST, SRC, END)


def record(code=None, marker=b"BSVS", key=7):
    code = shader() if code is None else code
    return struct.pack("<4sII", marker, key, len(code)) + code


def bsa(payloads=None, *, compressed=False, flipped=False, embedded=False, direct=False):
    payloads = [(b"first.fxp", record())] if payloads is None else payloads
    folder = b"shaders\0"
    names = b"".join(name + b"\0" for name, _ in payloads)
    block = 36 + 16
    header_flags = 3 | (4 if compressed else 0) | (0x100 if embedded else 0)
    header = struct.pack("<4s8I", b"BSA\0", 104, 36, header_flags, 1, len(payloads), len(folder), len(names), 0)
    folder_record = struct.pack("<QII", 0, len(payloads), block if direct else block + len(names))
    payload_start = block + 1 + len(folder) + 16 * len(payloads) + len(names)
    file_records, stored, offsets = [], [], []
    for name, payload in payloads:
        body = struct.pack("<I", len(payload)) + zlib.compress(payload) if compressed ^ flipped else payload
        if embedded:
            body = bytes([len(name)]) + name + body
        file_records.append(struct.pack("<QII", 0, len(body) | (0x40000000 if flipped else 0), payload_start))
        offsets.append(payload_start)
        stored.append(body)
        payload_start += len(body)
    return header + folder_record + bytes([len(folder)]) + folder + b"".join(file_records) + names + b"".join(stored), offsets


class BoundedArchiveTests(unittest.TestCase):
    def invalid(self, data, code=None, limits=study.Limits()):
        with self.assertRaises(study.Invalid) as error:
            study.read_bsa(data, limits)
        if code:
            self.assertEqual(error.exception.code, code)

    def test_named_archive_decodes_direct_and_adjusted_offsets_and_compression_toggle(self):
        for compressed, flipped, embedded, direct in ((False, False, False, False), (True, False, True, False),
                                                       (True, True, False, True), (False, True, False, False)):
            with self.subTest(compressed=compressed, flipped=flipped, embedded=embedded, direct=direct):
                archive, _ = bsa(compressed=compressed, flipped=flipped, embedded=embedded, direct=direct)
                catalog = study.read_bsa(archive)
                self.assertEqual(len(catalog["entries"]), 1)
                self.assertEqual(study.decode_member(archive, catalog["entries"][0]), record())

    def test_every_truncated_archive_prefix_is_rejected(self):
        archive, _ = bsa()
        for length in range(len(archive)):
            with self.subTest(length=length):
                self.invalid(archive[:length])

    def test_counts_flags_names_and_offsets_have_explicit_limits(self):
        original, _ = bsa()
        for offset, value, reason in ((4, 105, "archive_version"), (12, 0x43, "archive_flags"),
                                       (16, 4097, "archive_counts"), (20, 16385, "archive_counts"),
                                       (8, 5 << 20, "folder_table_offset"), (24, 5 << 20, "names_limit"),
                                       (48, 0xFFFFFFFF, "folder_block_offset")):
            data = bytearray(original)
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset):
                self.invalid(bytes(data), reason)
        self.invalid(original, "archive_size", replace(study.Limits(), archive_bytes=len(original) - 1))

    def test_payload_overlap_metadata_overlap_and_duplicate_names_rejected(self):
        archive, offsets = bsa([(b"a.fxp", record()), (b"b.fxp", record())])
        # File record starts at 52 + folder-name length byte and eight bytes.
        file_start = 61
        data = bytearray(archive)
        struct.pack_into("<I", data, file_start + 16 + 12, offsets[0])
        self.invalid(bytes(data), "payload_overlap")
        data = bytearray(archive)
        struct.pack_into("<I", data, file_start + 12, 36)
        self.invalid(bytes(data), "payload_overlaps_metadata")
        duplicate, _ = bsa([(b"a.fxp", record()), (b"A.FXP", record())])
        self.invalid(duplicate, "duplicate_member_name")

    def test_paths_are_virtual_only_and_unsafe_names_are_rejected(self):
        for name in (b"../out.fxp", b"c:evil.fxp", b"bad\\out.fxp", b"bad\x00.fxp", b"\xff.fxp"):
            with self.subTest(name=name):
                self.invalid(bsa([(name, record())])[0])

    def test_member_limit_is_checked_before_decompression(self):
        archive, _ = bsa(compressed=True)
        entry = study.read_bsa(archive)["entries"][0]
        changed = bytearray(archive)
        struct.pack_into("<I", changed, entry["offset"], (16 << 20) + 1)
        with patch.object(study.zlib, "decompressobj") as call:
            with self.assertRaisesRegex(study.Invalid, "decoded_size_limit"):
                study.decode_member(bytes(changed), entry)
        call.assert_not_called()

    def test_compression_bomb_caps_actual_output_not_only_declared_size(self):
        encoded = struct.pack("<I", 8) + zlib.compress(b"A" * (2 << 20))
        entry = {"offset": 0, "stored_bytes": len(encoded), "embedded_name": False, "compressed": True}
        with self.assertRaisesRegex(study.Invalid, "zlib_output_exceeded"):
            study.decode_member(encoded, entry)

    def test_compressed_truncation_trailing_bytes_and_size_mismatch_fail(self):
        base = struct.pack("<I", 9) + zlib.compress(b"123456789")
        variants = (base[:-1], base + b"trailing", struct.pack("<I", 10) + base[4:],
                    struct.pack("<I", 9) + zlib.compress(b"123456789") + zlib.compress(b"x"))
        for data in variants:
            with self.subTest(length=len(data)):
                entry = {"offset": 0, "stored_bytes": len(data), "embedded_name": False, "compressed": True}
                with self.assertRaises(study.Invalid):
                    study.decode_member(data, entry)

    def test_embedded_name_cannot_escape_member(self):
        data = b"\xffhello"
        entry = {"offset": 0, "stored_bytes": len(data), "embedded_name": True, "compressed": False}
        with self.assertRaisesRegex(study.Invalid, "truncated"):
            study.decode_member(data, entry)


class Sm3KeyTests(unittest.TestCase):
    def test_stage_and_hash_cover_version_through_end_including_comments(self):
        comment = b"authored-comment"
        code = words(VS, 0x0004FFFE) + comment + words(0x02000001, DST, SRC, END)
        found = study.validate_sm3(code + words(0xDEADBEEF), b"BSVS", require_exact_end=False)
        self.assertEqual(found["stage_vk"], 1)
        self.assertEqual(found["analyzed_bytes"], len(code))
        self.assertEqual(found["trailing_bytes"], 4)
        self.assertEqual(found["sha1"], hashlib.sha1(code).hexdigest())
        self.assertEqual(found["dxvk_key"], "VS_" + hashlib.sha1(code).hexdigest())
        self.assertEqual(found["comment_words"], 4)
        pixel = study.validate_sm3(shader(PS), b"BSPS")
        self.assertEqual(pixel["stage_vk"], 16)
        self.assertTrue(pixel["dxvk_key"].startswith("FS_"))
        with self.assertRaisesRegex(study.Invalid, "shader_trailing_bytes"):
            study.validate_sm3(code + words(0), b"BSVS")

    def test_version_stage_misalignment_and_missing_end_rejected(self):
        for code, marker in ((shader(), b"BSPS"), (shader(0xFFFE0200), b"BSVS"),
                              (shader()[:-1], b"BSVS"), (shader()[:-4], b"BSVS")):
            with self.subTest(code_length=len(code), marker=marker), self.assertRaises(study.Invalid):
                study.validate_sm3(code, marker)

    def test_relative_and_predicated_normal_instructions_match_consumption(self):
        cases = (
            words(VS, 0x03000001, DST, SRC | 0x2000, REL, END),
            words(VS, 0x03000001, DST | 0x2000, REL, SRC, END),
            words(VS, 0x13000001, DST, 0xB0001000, SRC, END),
            words(VS, 0x14000001, DST, 0xB0001000, SRC | 0x2000, REL, END),
        )
        for code in cases:
            with self.subTest(code=code.hex()):
                self.assertEqual(study.validate_sm3(code, b"BSVS")["analyzed_bytes"], len(code))

    def test_relative_operand_cannot_read_end_or_next_instruction(self):
        bad = words(VS, 0x02000001, DST, SRC | 0x2000, END)
        with self.assertRaisesRegex(study.Invalid, "operand_exceeds_instruction"):
            study.validate_sm3(bad, b"BSVS")
        bad = words(VS, 0x03000001, DST, SRC | 0x2000, REL | 0x2000, END)
        with self.assertRaisesRegex(study.Invalid, "relative_register"):
            study.validate_sm3(bad, b"BSVS")

    def test_dcl_and_def_do_not_accept_declared_length_that_decoder_ignores(self):
        valid = words(VS, 0x0200001F, 0x80000000, DST,
                      0x05000051, 0xA00F0000, 1, 2, 3, 4,
                      0x0200002F, 0xE00F0800, 1, END)
        self.assertEqual(study.validate_sm3(valid, b"BSVS")["analyzed_bytes"], len(valid))
        for instruction in (words(0x0000001F), words(0x01000051, DST),
                            words(0x0200001F, 0x80000000, DST | 0x2000)):
            with self.subTest(instruction=instruction.hex()), self.assertRaises(study.Invalid):
                study.validate_sm3(words(VS) + instruction + words(END), b"BSVS")

    def test_source_only_control_flow_and_immediate_bitpatterns(self):
        code = words(VS, 0x0200001B, SRC, SRC, 0x0000001D,
                     0x05000051, DST, 0xFFFF, 0x53565342, 0, 0xFFFFFFFF, END)
        self.assertEqual(study.validate_sm3(code, b"BSVS")["analyzed_bytes"], len(code))

    def test_malformed_instruction_comment_register_opcode_and_limits(self):
        examples = (
            (words(VS, 0x00000001, END), "operand_count"),
            (words(VS, 0x02000001, 0, SRC, END), "register_tag"),
            (words(VS, 0x0008FFFE, END), "comment_truncated"),
            (words(VS, 0x03000001, DST, SRC), "instruction_truncated"),
            (words(VS, 0x00000100, END), "unsupported_opcode"),
            (words(VS, 0x0000FFFD, END), "unsupported_opcode"),
            (words(VS, 0x40000000, END), "instruction_reserved_or_coissue"),
            (words(VS, 0x0100FFFF), "end_flags"),
        )
        for code, expected in examples:
            with self.subTest(expected=expected), self.assertRaisesRegex(study.Invalid, expected):
                study.validate_sm3(code, b"BSVS")
        with self.assertRaisesRegex(study.Invalid, "instruction_limit"):
            study.validate_sm3(shader(), b"BSVS", replace(study.Limits(), shader_instructions=1))
        with self.assertRaisesRegex(study.Invalid, "shader_size"):
            study.validate_sm3(shader(), b"BSVS", replace(study.Limits(), shader_bytes=8))

    def test_record_size_does_not_allocate_or_trust_truncated_content(self):
        oversized = struct.pack("<4sII", b"BSVS", 0, 0xFFFFFFFF)
        rows = study.scan_member(oversized + b"BSPS")
        self.assertEqual([row["status"] for row in rows], ["rejected", "rejected"])
        self.assertEqual(rows[0]["reason"], "record_payload_truncated")
        self.assertEqual(rows[1]["reason"], "record_header_truncated")

    def test_nested_valid_marker_inside_comment_rejects_both_candidates(self):
        inner = record()
        code = words(VS, ((len(inner) // 4) << 16) | 0xFFFE) + inner + words(END)
        rows = study.scan_member(record(code))
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(row["reason"] == "candidate_overlap" for row in rows))

    def test_adjacent_duplicates_are_allowed_and_marker_budget_is_bounded(self):
        rows = study.scan_member(record() + record(key=8))
        self.assertEqual([row["status"] for row in rows], ["key_candidate"] * 2)
        self.assertEqual(rows[0]["dxvk_key"], rows[1]["dxvk_key"])
        with self.assertRaisesRegex(study.Invalid, "candidate_limit"):
            study.scan_member(record() * 2, replace(study.Limits(), candidates=1))

    def test_total_validation_work_is_charged_before_each_native_free_check(self):
        budget = study.ValidationBudget()
        limits = replace(study.Limits(), validation_bytes=len(shader()))
        self.assertEqual(study.scan_member(record(), limits, budget)[0]["status"], "key_candidate")
        self.assertEqual(budget.used, len(shader()))
        with patch.object(study, "validate_sm3") as validator, self.assertRaisesRegex(study.Invalid, "validation_work_limit"):
            study.scan_member(record(), limits, budget)
        validator.assert_not_called()

    def test_nested_candidates_cannot_repeatedly_parse_past_total_work_cap(self):
        inner = record()
        code = words(VS, ((len(inner) // 4) << 16) | 0xFFFE) + inner + words(END)
        with self.assertRaisesRegex(study.Invalid, "validation_work_limit"):
            study.scan_member(record(code), replace(study.Limits(), validation_bytes=len(code)))


class PrivateOutputTests(unittest.TestCase):
    def test_private_root_link_cannot_redirect_writes_into_tracked_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "vendor").mkdir()
            (root / "private").mkdir()
            try:
                (root / "private/analysis").symlink_to(root / "vendor", target_is_directory=True)
            except OSError:
                self.skipTest("host cannot create symlink fixture")
            with self.assertRaisesRegex(study.Invalid, "output_must_be_private_analysis"):
                study.private_output(root / "private/analysis/escape", root)
            self.assertFalse((root / "vendor/escape").exists())

    def test_input_symlink_is_rejected_before_read(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "authored.bsa"
            source.write_bytes(bsa()[0])
            link = root / "linked.bsa"
            try:
                link.symlink_to(source)
            except OSError:
                self.skipTest("host cannot create symlink fixture")
            with self.assertRaisesRegex(study.Invalid, "input_symlink"):
                study.read_snapshot(link, study.Limits())

    def test_source_guard_accepts_crlf_only_change_and_rejects_semantic_change(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "authored.cpp"
            target.write_bytes(b"authored\r\nsource\r\n")
            with patch.object(study, "SOURCE_HASHES", {"authored.cpp": hashlib.sha256(b"authored\nsource\n").hexdigest()}):
                self.assertIn("normalized_sha256", study.verify_key_sources(root)["authored.cpp"])
                target.write_bytes(b"modified\nsource\n")
                with self.assertRaisesRegex(study.Invalid, "key_algorithm_source_changed"):
                    study.verify_key_sources(root)

    def test_output_must_be_new_and_under_private_analysis(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            with self.assertRaisesRegex(study.Invalid, "output_must_be_private_analysis"):
                study.private_output(root / "public", root)
            output = study.private_output(root / "private/analysis/new", root)
            sentinel = output / "sentinel"
            sentinel.write_bytes(b"preserve me")
            with self.assertRaises(FileExistsError):
                study.private_output(output, root)
            self.assertEqual(sentinel.read_bytes(), b"preserve me")

    def test_cli_level_run_preserves_input_deduplicates_and_keeps_unknowns_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            archive, _ = bsa([(b"first.fxp", record() + record(key=8)), (b"second.fxp", record(shader(PS), b"BSPS"))])
            source = root / "owned-copy.bsa"
            source.write_bytes(archive)
            output = root / "private/analysis/run"
            args = type("Args", (), {"archive": source, "output": output, "extract": True})()
            # Source-reference metadata points at real tracked source, while the
            # test output boundary remains a fresh synthetic temporary tree.
            original = study.private_output
            with patch.object(study, "private_output", side_effect=lambda p: original(p, root)):
                report = study.run(args)
            self.assertEqual(source.read_bytes(), archive)
            self.assertEqual(report["status"], "completed")
            self.assertEqual(report["summary"]["key_candidates"], 3)
            self.assertEqual(report["summary"]["unique_stage_sha1_keys"], 2)
            self.assertEqual(len(list((output / "bytecode").glob("*.dxso"))), 2)
            self.assertEqual(report["input"]["sha256_before"], report["input"]["sha256_after"])
            self.assertFalse(report["native_decoder_executed"])
            self.assertFalse(report["fxp_container_grammar_verified"])
            self.assertFalse(report["runtime_coverage_verified"])
            self.assertEqual(report["tool"]["sha256"], hashlib.sha256((output / "study-source.py").read_bytes()).hexdigest())
            self.assertEqual(json.loads((output / "inventory.json").read_text()), report)

    def test_invalid_input_preserves_failure_report_and_no_extractions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "bad.bsa"
            source.write_bytes(b"malformed" * 5)
            output = root / "private/analysis/failure"
            args = type("Args", (), {"archive": source, "output": output, "extract": True})()
            original = study.private_output
            with patch.object(study, "private_output", side_effect=lambda p: original(p, root)):
                report = study.run(args)
            self.assertEqual(report["status"], "failed")
            self.assertTrue((output / "inventory.json").is_file())
            self.assertFalse((output / "bytecode").exists())

    def test_cumulative_budget_failure_preserves_incomplete_private_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            archive, _ = bsa([(b"first.fxp", record()), (b"second.fxp", record())])
            source = root / "owned-copy.bsa"
            source.write_bytes(archive)
            output = root / "private/analysis/limited"
            args = type("Args", (), {"archive": source, "output": output, "extract": False})()
            original, limits = study.private_output, replace(study.Limits(), decoded_bytes=len(record()))
            with patch.object(study, "private_output", side_effect=lambda p: original(p, root)), \
                    patch.object(study, "Limits", return_value=limits):
                report = study.run(args)
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["error"], "total_decoded_limit")
            self.assertNotIn("summary", report)
            self.assertEqual(source.read_bytes(), archive)

    def test_archive_uses_one_validation_work_budget_across_members(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            archive, _ = bsa([(b"first.fxp", record()), (b"second.fxp", record())])
            source = root / "owned-copy.bsa"
            source.write_bytes(archive)
            output = root / "private/analysis/work-limited"
            args = type("Args", (), {"archive": source, "output": output, "extract": False})()
            original, limits = study.private_output, replace(study.Limits(), validation_bytes=len(shader()))
            with patch.object(study, "private_output", side_effect=lambda p: original(p, root)), \
                    patch.object(study, "Limits", return_value=limits):
                report = study.run(args)
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["error"], "validation_work_limit")
            self.assertNotIn("summary", report)
            self.assertEqual(source.read_bytes(), archive)

    def test_changed_input_during_study_cannot_report_completion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            archive, _ = bsa()
            source = root / "authored.bsa"
            source.write_bytes(archive)
            output = root / "private/analysis/changed"
            args = type("Args", (), {"archive": source, "output": output, "extract": False})()
            original, snapshot, calls = study.private_output, study.read_snapshot, []

            def changed(path, limits):
                if calls:
                    source.write_bytes(archive[:-1] + bytes([archive[-1] ^ 1]))
                calls.append(True)
                return snapshot(path, limits)

            with patch.object(study, "private_output", side_effect=lambda p: original(p, root)), \
                    patch.object(study, "read_snapshot", side_effect=changed):
                report = study.run(args)
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["error"], "input_changed_during_study")
            self.assertNotEqual(report["input"]["sha256_before"], report["input"]["sha256_after"])


if __name__ == "__main__":
    unittest.main()
