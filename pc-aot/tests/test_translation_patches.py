"""Patch identities and isolated canonical-archive application; synthetic data only."""
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("translation_patches", Path(__file__).parents[1] / "scripts" / "prepare_translation.py")
translation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(translation)


def diff(before, after):
    return ("diff --git a/fixture.txt b/fixture.txt\n"
            "--- a/fixture.txt\n+++ b/fixture.txt\n@@ -1 +1 @@\n"
            f"-{before}\n+{after}\n").encode()


class TranslationPatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve() / "workspace"
        self.root.mkdir()
        self.source = {"name": "skydream", "path": "vendor/skydream", "revision": "a" * 40, "python_requirements": {}}
        self.patcher = patch.object(translation, "ROOT", self.root)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        for name, result in (("translator_state", {"matches": True}),
                             ("dependencies", {"packages": {}, "posix_host": True, "sh": "sh", "python3_matches_preflight": True})):
            mocked = patch.object(translation, name, return_value=result)
            mocked.start()
            self.addCleanup(mocked.stop)
        self.write_config()

    def write_config(self):
        (self.root / "source-lock.json").write_text(json.dumps({"sources": [self.source]}), encoding="utf-8")
        (self.root / "input-profile.json").write_text(json.dumps({
            "translator_revision": self.source["revision"], "expected_pe": {}, "reviewed_unpacked": None}), encoding="utf-8")

    def add_patch(self, name="0001.patch", before="one", after="two"):
        relative = "patches/skydream/" + name
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(diff(before, after))
        return {"path": relative, "sha256": translation.hashes(path)["sha256"], "base_revision": self.source["revision"]}

    def plan(self):
        self.write_config()
        return translation.make_plan(sdk="iphoneos26.2")

    def test_absent_and_empty_patch_lists_preserve_contract_and_gate(self):
        absent = self.plan()
        self.source["patches"] = []
        empty = self.plan()
        self.assertEqual(absent["contract_sha256"], empty["contract_sha256"])
        self.assertNotIn("translator_patches", absent["contract"])
        self.assertEqual(absent["planned_patches"]["commands"], [])
        self.assertFalse(any("patch" in reason for reason in absent["blockers"]))
        self.assertIn("unpacked_hash_and_layout_not_reviewed", absent["blockers"])

    def test_order_and_content_are_bound_to_contract(self):
        first = self.add_patch()
        second = self.add_patch("0002.patch", "two", "three")
        self.source["patches"] = [first, second]
        before = self.plan()
        self.assertTrue(all(x["verified"] for x in before["patch_verification"]))
        self.assertEqual([x["path"] for x in before["contract"]["translator_patches"]], [first["path"], second["path"]])
        self.assertFalse(before["planned_patches"]["executed"])
        self.assertEqual(before["planned_patches"]["applicability"], "not_checked")
        self.assertIn("unpacked_hash_and_layout_not_reviewed", before["blockers"])
        self.source["patches"] = [second, first]
        self.assertNotEqual(before["contract_sha256"], self.plan()["contract_sha256"])
        self.source["patches"] = [first, second]
        (self.root / first["path"]).write_bytes(diff("one", "changed"))
        mismatch = self.plan()
        self.assertNotEqual(before["contract_sha256"], mismatch["contract_sha256"])
        self.assertIn("translator_patch_0_sha256_mismatch", mismatch["blockers"])
        self.assertEqual(mismatch["planned_patches"]["commands"], [])
        first["sha256"] = translation.hashes(self.root / first["path"])["sha256"]
        updated = self.plan()
        self.assertTrue(updated["patch_verification"][0]["verified"])
        self.assertNotEqual(before["contract_sha256"], updated["contract_sha256"])

    def test_bad_revision_hash_missing_and_nonregular_files_block(self):
        entry = self.add_patch()
        for change, reason in (({"base_revision": "b" * 40}, "base_revision_mismatch"),
                               ({"sha256": "invalid"}, "invalid_sha256"),
                               ({"sha256": "0" * 64}, "sha256_mismatch"),
                               ({"path": "patches/skydream/missing.patch"}, "missing_or_nonregular_file"),
                               ({"path": "patches/skydream/directory"}, "missing_or_nonregular_file")):
            (self.root / "patches/skydream/directory").mkdir(exist_ok=True)
            with self.subTest(change=change):
                self.source["patches"] = [{**entry, **change}]
                result = self.plan()
                self.assertIn("translator_patch_0_" + reason, result["blockers"])
                self.assertEqual(result["planned_patches"]["commands"], [])

    def test_invalid_list_and_entries_block(self):
        for value in (None, {}, "patches/skydream/test.patch"):
            self.source["patches"] = value
            self.assertIn("translator_patches_must_be_an_ordered_list", self.plan()["blockers"])
        for entry in (None, "file.patch", {}, {"path": 42}):
            self.source["patches"] = [entry]
            self.assertIn("translator_patch_0_invalid_identity", self.plan()["blockers"])

    def test_absolute_traversal_and_other_scopes_rejected_before_read(self):
        entry = self.add_patch()
        for value in (str((self.root / entry["path"]).resolve()), "patches/other/file.patch",
                      "patches/skydream/../outside.patch", "../outside.patch",
                      "patches\\skydream\\file.patch", "patches/skydream/file.patch:stream"):
            with self.subTest(path=value), patch.object(translation, "hashes", side_effect=AssertionError("outside patch read")):
                blockers = []
                records = translation.verify_patches({**self.source, "patches": [{**entry, "path": value}]}, blockers)
                self.assertIn("translator_patch_0_path_outside_patch_scope", blockers)
                self.assertFalse(records[0]["verified"])

    def symlink_or_skip(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlinks unavailable: {error}")

    def test_file_symlink_escape_blocked(self):
        entry = self.add_patch()
        outside = self.root.parent / "outside.patch"
        outside.write_bytes(diff("one", "outside"))
        link = self.root / "patches/skydream/link.patch"
        self.symlink_or_skip(link, outside)
        self.source["patches"] = [{**entry, "path": "patches/skydream/link.patch"}]
        self.assertIn("translator_patch_0_path_outside_patch_scope", self.plan()["blockers"])

    def test_patch_directory_symlink_escape_blocked(self):
        outside = self.root.parent / "outside"
        outside.mkdir()
        (outside / "file.patch").write_bytes(diff("one", "outside"))
        (self.root / "patches").mkdir()
        self.symlink_or_skip(self.root / "patches/skydream", outside, directory=True)
        self.source["patches"] = [{"path": "patches/skydream/file.patch", "sha256": "0" * 64,
                                   "base_revision": self.source["revision"]}]
        self.assertIn("translator_patch_0_path_outside_patch_scope", self.plan()["blockers"])

    @unittest.skipUnless(shutil.which("git"), "git required for canonical staging regression")
    def test_planned_commands_apply_ordered_chain_only_inside_fresh_archive(self):
        def git(cwd, *args):
            return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True)

        # Both archive and vendor live below an outer Git repository, as in use.
        git(self.root, "init", "--quiet")
        (self.root / "fixture.txt").write_bytes(b"outer must remain unchanged\n")
        vendor = self.root / self.source["path"]
        vendor.mkdir(parents=True)
        git(vendor, "init", "--quiet")
        (vendor / "fixture.txt").write_bytes(b"one\n")
        git(vendor, "-c", "core.autocrlf=false", "add", "fixture.txt")
        git(vendor, "-c", "user.name=Synthetic Fixture", "-c", "user.email=fixture@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "synthetic fixture")
        self.source["revision"] = git(vendor, "rev-parse", "HEAD").stdout.decode().strip()
        self.source["patches"] = [self.add_patch(), self.add_patch("0002.patch", "two", "three")]
        result = self.plan()
        self.assertEqual([x["operation"] for x in result["planned_patches"]["commands"]],
                         ["initialize_isolated_archive", "check", "apply", "check", "apply"])
        run = self.root / "private/run"
        stage = run / result["planned_source_materialization"]["destination_relative_to_run"]
        stage.mkdir(parents=True)
        archive = subprocess.run(result["planned_source_materialization"]["argv"], check=True, capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            # This fixture contains exactly one expected file; no generic extraction.
            (stage / "fixture.txt").write_bytes(tar.extractfile("fixture.txt").read())
        self.assertEqual((stage / "fixture.txt").read_bytes(), b"one\n")
        for command in result["planned_patches"]["commands"]:
            before = (stage / "fixture.txt").read_bytes()
            completed = subprocess.run(command["argv"], cwd=run / result["planned_patches"]["cwd_relative_to_run"],
                                       capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
            if command["operation"] == "check":
                self.assertEqual(before, (stage / "fixture.txt").read_bytes())
        self.assertEqual((stage / "fixture.txt").read_bytes(), b"three\n")
        self.assertEqual((vendor / "fixture.txt").read_bytes(), b"one\n")
        self.assertEqual((self.root / "fixture.txt").read_bytes(), b"outer must remain unchanged\n")
        self.assertEqual(git(vendor, "status", "--porcelain").stdout, b"")
        self.assertTrue((stage / ".git").is_dir())


if __name__ == "__main__":
    unittest.main()
