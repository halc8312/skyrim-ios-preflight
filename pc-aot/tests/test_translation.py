import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("prepare_translation", Path(__file__).parents[1] / "scripts" / "prepare_translation.py")
translation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(translation)


def fixture_pe(path, machine=0x14C, section=b".text", entry=0x1000):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<HH", data, 68, machine, 1)
    struct.pack_into("<H", data, 84, 224)
    struct.pack_into("<H", data, 88, 0x10B)
    struct.pack_into("<I", data, 88 + 16, entry)
    struct.pack_into("<I", data, 88 + 28, 0x400000)
    struct.pack_into("<8sIIIIIIHHI", data, 88 + 224, section, 512, 0x1000, 512, 512, 0, 0, 0, 0, 0x60000020)
    data[512] = 0xC3
    path.write_bytes(data)


class TranslationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.original = self.directory / "TESV.exe"
        self.unpacked = self.directory / "TESV_unpacked.exe"
        fixture_pe(self.original, section=b".bind")
        fixture_pe(self.unpacked)
        self.recipe = self.directory / "recipe.json"
        self.write_recipe()
        self.translator_patch = patch.object(translation, "translator_state", return_value={"matches": True})
        self.translator_patch.start()
        self.addCleanup(self.translator_patch.stop)
        self.deps_patch = patch.object(translation, "dependencies", return_value={"packages": {}, "posix_host": True, "sh": "/bin/sh", "python3_matches_preflight": True})
        self.deps_patch.start()
        self.addCleanup(self.deps_patch.stop)

    def write_recipe(self, **updates):
        value = {"method": "synthetic test fixture", "tool": "test_translation.py", "tool_version": "1",
                 "steps": ["Generate two synthetic PE fixtures; no game code."],
                 "original_sha256": translation.hashes(self.original)["sha256"],
                 "unpacked_sha256": translation.hashes(self.unpacked)["sha256"], **updates}
        self.recipe.write_text(json.dumps(value), encoding="utf-8")

    def plan(self, **changes):
        values = dict(original=self.original, unpacked=self.unpacked, recipe=self.recipe, sdk="iphoneos26.2")
        values.update(changes)
        return translation.make_plan(**values)

    def test_unknown_input_never_approved_by_correct_pe_shape(self):
        before = self.original.read_bytes(), self.unpacked.read_bytes()
        result = self.plan()
        self.assertEqual(result["status"], "blocked")
        self.assertIn("unpacked_hash_and_layout_not_reviewed", result["blockers"])
        self.assertNotEqual(result["original"]["path"], result["unpacked"]["path"])
        self.assertNotEqual(result["original"]["sha256"], result["unpacked"]["sha256"])
        self.assertIn("version", result["original"])
        self.assertFalse(result["executed"])
        self.assertFalse(result["staged_inputs"])
        self.assertEqual(before, (self.original.read_bytes(), self.unpacked.read_bytes()))

    def test_original_cannot_be_reused_as_unpacked_even_via_hardlink(self):
        alias = self.directory / "alias.exe"
        alias.hardlink_to(self.original)
        for unpacked in (self.original, alias):
            self.assertIn("original_and_unpacked_must_be_separate_files", self.plan(unpacked=unpacked)["blockers"])

    def test_packed_or_wrong_architecture_blocked(self):
        self.assertIn("unpacked_still_has_steamstub_section", self.plan(unpacked=self.original)["blockers"])
        fixture_pe(self.unpacked, machine=0x8664)
        result = self.plan()
        self.assertIn("unsupported_unpacked_architecture", result["blockers"])
        self.assertIn("unpacked_pe_layout_mismatch", result["blockers"])

    def test_malformed_pe_is_reported(self):
        self.unpacked.write_bytes(b"not an executable")
        self.assertIn("invalid_unpacked_pe", self.plan()["blockers"])

    def test_recipe_missing_invalid_or_bound_to_wrong_input_is_blocked(self):
        self.assertIn("missing_provenance_recipe", self.plan(recipe=None)["blockers"])
        self.write_recipe(unpacked_sha256="0" * 64)
        self.assertIn("recipe_unpacked_sha256_mismatch", self.plan()["blockers"])
        self.recipe.write_text('{"steps": "run something"}', encoding="utf-8")
        self.assertIn("invalid_provenance_recipe", self.plan()["blockers"])

    def test_contract_deterministic_and_sdk_sensitive(self):
        result = self.plan()
        self.assertEqual(result["contract_sha256"], self.plan()["contract_sha256"])
        self.assertNotEqual(result["contract_sha256"], self.plan(sdk="iphoneos26.3")["contract_sha256"])
        self.assertEqual(result["planned_regeneration"]["environment"]["LIFT_MEMORDER"], "1")
        self.assertEqual(result["contract"]["compile_options"]["LB_STRICT_ORDER"], "ON")
        self.assertIn("exact_iphoneos_sdk_not_supplied", self.plan(sdk=None)["blockers"])
        self.assertIn("sdk_must_be_exact_versioned_iphoneos_name", self.plan(sdk="iphonesimulator26.2")["blockers"])

    def test_dependency_and_translator_failures_are_explicit(self):
        deps = {"packages": {"capstone": {"matches": False}}, "posix_host": False, "sh": None, "python3_matches_preflight": False}
        with patch.object(translation, "dependencies", return_value=deps), patch.object(translation, "translator_state", return_value={"matches": False}):
            blockers = self.plan()["blockers"]
        self.assertIn("dependency_capstone_missing_or_wrong_version", blockers)
        self.assertIn("translator_missing_modified_or_wrong_revision", blockers)
        self.assertIn("generation_preflight_requires_posix_host_and_sh", blockers)
        self.assertIn("shell_python3_does_not_match_preflight_interpreter", blockers)

    def test_reviewed_profile_requires_matching_content_not_only_version(self):
        record = self.plan()["unpacked"]
        profile = {"translator_revision": "pinned", "expected_pe": {}, "reviewed_unpacked": {
            "sha1": record["sha1"], "sha256": "0" * 64, "entry_rva": record["pe"]["entry_rva"],
            "sections_sha256": record["sections_sha256"], "review_evidence": "synthetic-only test"}}
        self.assertIn("unpacked_not_in_reviewed_profile", translation.validate_profile(record, profile, "pinned"))
        self.assertIn("profile_translator_mismatch", translation.validate_profile(record, profile, "different"))

    def test_output_rejects_public_location_and_never_overwrites(self):
        with patch.object(translation, "ROOT", self.directory):
            with self.assertRaises(ValueError):
                translation.write_plan({}, self.directory / "docs" / "run")
            run = self.directory / "private" / "run"
            manifest = translation.write_plan({"preserve": True}, run)
            with self.assertRaises(FileExistsError):
                translation.write_plan({"replace": True}, run)
            self.assertEqual(json.loads(manifest.read_text()), {"preserve": True})

    def directory_symlink_or_skip(self, link, target):
        try:
            link.symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"directory symlinks unavailable: {error}")

    def test_output_accepts_symlink_alias_of_workspace_root(self):
        canonical = self.directory / "workspace"
        canonical.mkdir()
        alias = self.directory / "workspace-alias"
        self.directory_symlink_or_skip(alias, canonical)
        with patch.object(translation, "ROOT", alias):
            manifest = translation.write_plan({"alias": True}, alias / "private" / "run")
        self.assertEqual(manifest, canonical.resolve() / "private" / "run" / "translation-plan.json")
        self.assertEqual(json.loads(manifest.read_text()), {"alias": True})

    def test_output_rejects_private_directory_symlink_escape(self):
        workspace = self.directory / "workspace"
        outside = self.directory / "outside"
        workspace.mkdir()
        outside.mkdir()
        self.directory_symlink_or_skip(workspace / "private", outside)
        with patch.object(translation, "ROOT", workspace):
            with self.assertRaises(ValueError):
                translation.write_plan({}, workspace / "private" / "run")
        self.assertFalse((outside / "run").exists())

    def test_no_inputs_returns_blocked_manifest_and_exit_two(self):
        plan = translation.make_plan()
        for reason in ("missing_original", "missing_unpacked", "missing_provenance_recipe", "exact_iphoneos_sdk_not_supplied"):
            self.assertIn(reason, plan["blockers"])
        with patch.object(translation, "make_plan", return_value=plan), patch.object(translation, "ROOT", self.directory):
            code = translation.main(["--output", str(self.directory / "local" / "no-inputs")])
        self.assertEqual(code, 2)
        self.assertTrue((self.directory / "local" / "no-inputs" / "translation-plan.json").is_file())


if __name__ == "__main__":
    unittest.main()
