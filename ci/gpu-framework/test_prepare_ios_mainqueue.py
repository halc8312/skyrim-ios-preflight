"""Public overlay refusal and dependency-integrity tests; no SDK or title inputs."""
import copy
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import tomllib
import unittest
from unittest.mock import patch

ROOT = Path(__file__).parent
spec = importlib.util.spec_from_file_location("ios_mainqueue", ROOT / "prepare_ios_mainqueue.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MainQueueOverlayTests(unittest.TestCase):
    def lock(self):
        return (ROOT / "testdata/wgpu-native-6aed509.Cargo.lock").read_bytes()

    def test_exact_lock_changes_only_hal_source_and_existing_dispatch_edge(self):
        original = self.lock()
        before = tomllib.loads(original.decode())
        expected = copy.deepcopy(before)
        hal, = [p for p in expected["package"] if p["name"] == "wgpu-hal"]
        del hal["source"]
        del hal["checksum"]
        hal["dependencies"] = sorted([*hal["dependencies"], "dispatch2"])
        actual = module.rewrite_lock(original)
        self.assertEqual(tomllib.loads(actual.decode()), expected)
        self.assertEqual(module.sha(actual), "9cf564aba13ef1a761948b2fae93c66cbfae8ee975e3cfd48202afb6130dbe04")

    def test_changed_or_already_patched_lock_is_rejected(self):
        original = self.lock()
        for bad in (original + b"\n", original.replace(b'29.0.3', b'29.0.4'),
                    module.rewrite_lock(original)):
            with self.assertRaises(ValueError):
                module.rewrite_lock(bad)

    def archive(self, members):
        result = io.BytesIO()
        with tarfile.open(fileobj=result, mode="w") as archive:
            for name, kind in members:
                info = tarfile.TarInfo(name)
                info.type = kind
                if kind == tarfile.REGTYPE:
                    info.size = 6
                    archive.addfile(info, io.BytesIO(b"public"))
                else:
                    info.linkname = "Cargo.toml"
                    archive.addfile(info)
        return result.getvalue()

    def test_regular_archive_extracts_without_filesystem_side_effects(self):
        data = self.archive([("wgpu-hal-29.0.3/Cargo.toml", tarfile.REGTYPE)])
        self.assertEqual(module.archive_files(data, "wgpu-hal-29.0.3"),
                         {"Cargo.toml": b"public"})

    def test_archive_paths_links_duplicates_and_wrong_root_are_rejected(self):
        for members, prefix in [
            ([("../Cargo.toml", tarfile.REGTYPE)], None),
            ([("/Cargo.toml", tarfile.REGTYPE)], None),
            ([("x\\Cargo.toml", tarfile.REGTYPE)], None),
            ([("x", tarfile.SYMTYPE)], None),
            ([("x", tarfile.LNKTYPE)], None),
            ([("x", tarfile.REGTYPE), ("x", tarfile.REGTYPE)], None),
            ([("other/Cargo.toml", tarfile.REGTYPE)], "wgpu-hal-29.0.3"),
        ]:
            with self.subTest(members=members), self.assertRaises(ValueError):
                module.archive_files(self.archive(members), prefix)

    def test_wrong_commit_or_modified_source_is_rejected(self):
        with patch.object(module, "git", return_value=b"wrong\n"):
            with self.assertRaises(ValueError):
                module.checked_tree(ROOT, module.SOURCE_COMMIT)
        with patch.object(module, "git", side_effect=[
                (module.SOURCE_COMMIT + "\n").encode(), b" M Cargo.lock\n"]):
            with self.assertRaises(ValueError):
                module.checked_tree(ROOT, module.SOURCE_COMMIT)

    def test_patch_and_preimage_identity_are_pinned(self):
        data = (ROOT / "wgpu-hal-ios-mainqueue.patch").read_bytes()
        self.assertEqual(module.sha(data), module.PATCH_SHA256)
        self.assertEqual(set(module.PATCH_FILES), {"Cargo.toml", "src/metal/surface.rs"})

    def test_existing_nested_or_wrong_crate_outputs_fail_before_writing(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / "source"
            source.mkdir()
            for output in (source, source / "nested", base):
                with self.subTest(output=output), self.assertRaises(ValueError):
                    module.prepare(source, output, b"invalid crate", ROOT / "wgpu-hal-ios-mainqueue.patch")
            output = base / "fresh"
            with self.assertRaises(ValueError):
                module.prepare(source, output, b"invalid crate", ROOT / "wgpu-hal-ios-mainqueue.patch")
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
