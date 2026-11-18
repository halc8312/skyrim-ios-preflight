"""Synthetic local-copy tests; no game files, device, signing or network calls."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("owned_data", ROOT / "scripts/prepare_owned_data.py")
owned = importlib.util.module_from_spec(spec)
spec.loader.exec_module(owned)


class OwnedDataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.game = self.root / "original"
        (self.game / "data" / "strings").mkdir(parents=True)
        self.inputs = {"Data/Skyrim.esm": b"authored master",
                       "Data/Strings/Skyrim_English.STRINGS": b"authored strings",
                       "Skyrim_default.ini": b"[General]\n"}
        for name, data in self.inputs.items():
            p = self.game.joinpath(*name.lower().split("/"))
            p.write_bytes(data)
        self.candidate = self.root / "candidate.bin"
        self.candidate.write_bytes(b"authored synthetic executable")
        self.output = self.root / "private" / "run" / "Skyrim"
        self.options = dict(root=self.root, required_files=tuple(self.inputs),
                            candidate_bytes=self.candidate.stat().st_size,
                            candidate_sha256=hashlib.sha256(self.candidate.read_bytes()).hexdigest())

    def prepare(self, **changes):
        options = self.options | changes
        return owned.prepare_data(self.game, self.candidate, self.output, **options)

    def no_completion(self):
        self.assertFalse((self.output.parent / owned.MANIFEST).exists())

    def require_case_sensitive(self):
        if (self.game / "DATA").exists():
            self.skipTest("Case-sensitive filesystem required for duplicate-name fixture")

    def symlink(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError):
            self.skipTest("Symlink creation is unavailable in this environment")

    def test_complete_case_normalized_readback_and_only_allowed_files(self):
        (self.game / "data" / "mod.esp").write_bytes(b"excluded mod")
        (self.game / "save.ess").write_bytes(b"excluded save")
        originals = {p: (owned._fingerprint(p.stat()), p.read_bytes())
                     for p in self.game.rglob("*") if p.is_file()}
        result = self.prepare()
        self.assertTrue(result["complete"])
        self.assertFalse(result["game_executed"])
        self.assertFalse(result["vanilla_asset_identity_verified"])
        self.assertEqual({r["path"] for r in result["files"]}, set(self.inputs) | {"TESV_unpacked.exe"})
        for r in result["files"]:
            data = (self.output / r["path"]).read_bytes()
            self.assertEqual(r["bytes"], len(data))
            self.assertEqual(r["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(json.loads((self.output.parent / owned.MANIFEST).read_text()), result)
        self.assertFalse((self.output / owned.MANIFEST).exists())
        self.assertFalse(list(self.output.parent.glob("*.pending")))
        for p, (info, data) in originals.items():
            self.assertEqual(owned._fingerprint(p.stat()), info)
            self.assertEqual(p.read_bytes(), data)

    def test_missing_required_file_is_rejected_before_output(self):
        (self.game / "skyrim_default.ini").unlink()
        with self.assertRaisesRegex(owned.PreparationError, "Missing required"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_candidate_wrong_size_or_hash_is_rejected_before_output(self):
        for options in ({"candidate_bytes": 1}, {"candidate_sha256": "0" * 64}):
            with self.subTest(options=options), self.assertRaises(owned.PreparationError):
                self.prepare(**options)
            self.assertFalse(self.output.exists())

    def test_empty_file_and_size_limits(self):
        for options in ({"max_file_bytes": 2}, {"max_total_bytes": 2}):
            with self.subTest(options=options), self.assertRaises(owned.PreparationError):
                self.prepare(**options)
        (self.game / "skyrim_default.ini").write_bytes(b"")
        with self.assertRaisesRegex(owned.PreparationError, "empty"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_source_case_alias_is_rejected(self):
        self.require_case_sensitive()
        (self.game / "data" / "SKYRIM.ESM").write_bytes(b"ambiguous")
        with self.assertRaisesRegex(owned.PreparationError, "Ambiguous"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_source_file_link_and_directory_link_are_rejected(self):
        original = self.game / "data" / "skyrim.esm"
        original.unlink()
        self.symlink(original, self.candidate)
        with self.assertRaisesRegex(owned.PreparationError, "links"):
            self.prepare()
        original.unlink()
        original.write_bytes(b"restored fixture")
        other = self.root / "strings-target"
        (self.game / "data" / "strings").rename(other)
        self.symlink(self.game / "data" / "strings", other, True)
        with self.assertRaisesRegex(owned.PreparationError, "links"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_output_boundary_and_source_tree_are_preserved(self):
        for output in (self.root / "outside" / "Skyrim",
                       self.root / "private" / "Skyrim",
                       self.root / "private" / "run" / "WrongName"):
            self.output = output
            with self.subTest(output=output), self.assertRaises(owned.PreparationError):
                self.prepare()
            self.assertFalse(output.exists())
        self.output = self.root / "private" / "run" / ".." / "Skyrim"
        with self.assertRaisesRegex(owned.PreparationError, "traversal"):
            self.prepare()

    def test_symlinked_output_parent_is_rejected(self):
        (self.root / "private").mkdir()
        self.symlink(self.root / "private" / "run", self.game, True)
        with self.assertRaisesRegex(owned.PreparationError, "links"):
            self.prepare()
        self.assertFalse((self.game / "Skyrim").exists())

    def test_output_inside_original_tree_is_rejected(self):
        (self.root / "private").mkdir()
        original = self.root / "private" / "original"
        self.game.rename(original)
        self.game = original
        self.output = original / "attempt" / "Skyrim"
        with self.assertRaisesRegex(owned.PreparationError, "original game"):
            self.prepare()
        self.assertFalse(self.output.parent.exists())

    def test_existing_output_or_marker_are_never_overwritten(self):
        self.output.mkdir(parents=True)
        sentinel = self.output / "keep.txt"
        sentinel.write_bytes(b"keep")
        with self.assertRaisesRegex(owned.PreparationError, "already exists"):
            self.prepare()
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.output = self.root / "private" / "another" / "Skyrim"
        self.output.parent.mkdir()
        marker = self.output.parent / owned.MANIFEST
        marker.write_text("old manifest")
        with self.assertRaisesRegex(owned.PreparationError, "already exists"):
            self.prepare()
        self.assertEqual(marker.read_text(), "old manifest")
        self.assertFalse(self.output.exists())

    def test_dlc_is_opt_in_paired_and_not_activated(self):
        for suffix in ("esm", "bsa"):
            (self.game / "data" / ("dawnguard." + suffix)).write_bytes(b"authored DLC")
        result = self.prepare()
        self.assertEqual(result["dlc_copied"], [])
        self.assertFalse((self.output / "Data/Dawnguard.esm").exists())
        self.output = self.root / "private" / "dlc" / "Skyrim"
        result = self.prepare(include_dlc=True)
        self.assertEqual(result["dlc_copied"], ["Dawnguard"])
        self.assertFalse(result["dlc_activation_configured"])
        self.assertTrue((self.output / "Data/Dawnguard.bsa").is_file())
        self.assertFalse(list(self.output.rglob("plugins.txt")))

    def test_incomplete_dlc_pair_is_rejected_when_requested(self):
        (self.game / "data" / "Dragonborn.esm").write_bytes(b"authored DLC")
        with self.assertRaisesRegex(owned.PreparationError, "Incomplete"):
            self.prepare(include_dlc=True)
        self.assertFalse(self.output.exists())

    def test_original_change_or_copy_corruption_prevents_completion(self):
        actual_copy = owned._copy_source
        def change_original(source, destination, expected):
            result = actual_copy(source, destination, expected)
            if destination.name == "TESV_unpacked.exe":
                (self.game / "skyrim_default.ini").write_bytes(b"changed during copy")
            return result
        with mock.patch.object(owned, "_copy_source", change_original):
            with self.assertRaisesRegex(owned.PreparationError, "original changed"):
                self.prepare()
        self.no_completion()
        self.assertTrue(self.output.is_dir())  # Failure evidence is retained.
        self.output = self.root / "private" / "corrupt" / "Skyrim"
        def corrupt_copy(source, destination, expected):
            result = actual_copy(source, destination, expected)
            if destination.name == "TESV_unpacked.exe":
                destination.write_bytes(b"corrupt")
            return result
        with mock.patch.object(owned, "_copy_source", corrupt_copy):
            with self.assertRaisesRegex(owned.PreparationError, "readback"):
                self.prepare()
        self.no_completion()

    def test_atomic_marker_collision_preserves_other_publication(self):
        actual_link = os.link
        def conflict(source, destination):
            Path(destination).write_text("concurrent result")
            actual_link(source, destination)
        with mock.patch.object(owned.os, "link", conflict), self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual((self.output.parent / owned.MANIFEST).read_text(), "concurrent result")

    def test_default_allowlist_matches_verified_base_package(self):
        self.assertEqual(len(owned.REQUIRED_FILES), 25)
        self.assertEqual(len(set(n.casefold() for n in owned.REQUIRED_FILES)), 25)
        self.assertIn("Data/Update.bsa", owned.REQUIRED_FILES)
        self.assertEqual(sum("Data/Strings/" in n for n in owned.REQUIRED_FILES), 6)
        self.assertEqual(owned.CANDIDATE_BYTES, 17_462_064)
        self.assertEqual(owned.CANDIDATE_SHA256,
                         "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605")


if __name__ == "__main__":
    unittest.main()
