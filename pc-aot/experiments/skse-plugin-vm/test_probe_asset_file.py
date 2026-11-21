"""Run the selected-directory asset reader against owned, disposable POSIX files."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


@unittest.skipUnless(os.name == "posix", "Requires openat and regular POSIX file fingerprints")
class SelectedAssetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        directory = Path(cls.build.name)
        cls.binary = directory / "fixture"
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKSE_PROBE_SANITIZE") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, HERE / "test_probe_asset_file.c", "-o", cls.binary], check=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.top = Path(temporary.name)
        self.root = self.top / "selected"
        self.root.mkdir()

    def run_read(self, path, status, value=None, *, limit=1024, mode="normal", root=None):
        proc = subprocess.run([self.binary, root or self.root, path, str(limit), mode], check=True,
                              capture_output=True, text=True, timeout=4)
        code, size, reads, data = proc.stdout.rstrip("\n").split("|")
        self.assertEqual(int(code), status, (path, mode, proc.stdout))
        self.assertEqual(bytes.fromhex(data), value or b"")
        self.assertEqual(int(size), len(value or b""))
        return int(reads)

    def test_nested_unicode_and_empty_snapshots_at_the_exact_size_limit(self):
        child = self.root / "設定" / "魔法"
        child.mkdir(parents=True)
        path = child / "色 🌙.ini"
        value = b"a\0b\r\n\xff"
        path.write_bytes(value)
        relative = path.relative_to(self.root)
        self.run_read(relative, 0, value, limit=len(value))
        self.assertEqual(self.run_read(relative, 223, limit=len(value)-1), 0)
        self.assertEqual(path.read_bytes(), value)
        (self.root / "empty").touch()
        self.run_read("empty", 0, b"", limit=0)
        self.assertEqual(self.run_read(relative, 223, limit=0), 0)

    def test_distinguishes_missing_file_directory_and_invalid_root(self):
        (self.root / "directory").mkdir()
        (self.root / "plain").touch()
        self.run_read("missing", 2)
        self.run_read("directory/missing", 2)
        self.run_read("uncreated/missing", 3)
        self.run_read("plain/child", 3)
        self.run_read("directory", 5)
        self.run_read("plain", 5, root=self.root / "plain")
        for mode in ("no-context", "bad-fd", "no-path", "no-bytes", "no-length"):
            self.run_read("plain", 5, mode=mode)

    def test_attributes_use_only_selected_metadata_without_reading_bytes(self):
        child = self.root / "設定"
        child.mkdir()
        asset = child / "選択.ini"
        asset.write_bytes(b"data")
        (self.root / "linked").symlink_to(child)
        for path, expected in (
            ("設定", "0|16|0"), ("設定/選択.ini", "0|128|0"), ("設定/missing.ini", "2|0|0"),
            ("unavailable/missing.ini", "3|0|0"), ("linked/選択.ini", "5|0|0"),
            ("../設定", "5|0|0"), ("linked", "5|0|0")
        ):
            with self.subTest(path=path):
                result = subprocess.check_output([self.binary, self.root, path, "0", "attributes"], text=True, timeout=4)
                self.assertEqual(result.strip(), expected)
        self.assertEqual(asset.read_bytes(), b"data")

    def test_windows_spelling_keeps_non_ascii_components_and_refuses_links(self):
        child = self.root / "Data" / "SKSE" / "設定"
        child.mkdir(parents=True)
        asset = child / "選択.ini"
        asset.write_bytes(b"data")
        self.run_read("data/skse/設定/選択.INI", 0, b"data")
        output = subprocess.check_output([self.binary, self.root, "DATA/sKsE/設定", "0", "attributes"],
                                         text=True, timeout=4)
        self.assertEqual(output.strip(), "0|16|0")
        (self.root / "Link").symlink_to(child)
        self.assertEqual(self.run_read("LINK/選択.INI", 5), 0)

    def test_ambiguous_case_is_refused_without_opening_either_file(self):
        (self.root / "Enabled.ini").write_bytes(b"first")
        alternate = self.root / "ENABLED.ini"
        if alternate.exists():
            self.skipTest("Case-sensitive directory required")
        alternate.write_bytes(b"second")
        self.assertEqual(self.run_read("enabled.INI", 5), 0)
        self.run_read("Enabled.ini", 0, b"first")
        self.run_read("ENABLED.ini", 0, b"second")
        for path in ("enabled.INI", "Enabled.ini", "ENABLED.ini"):
            self.assertEqual(self.run_read(path, 5, mode="unique"), 0)
        (self.root / "Nested").mkdir()
        (self.root / "Nested/test").write_bytes(b"unique snapshot")
        self.run_read("NESTED/TEST", 0, b"unique snapshot", mode="unique")
        (self.root / "NESTED").mkdir()
        self.assertEqual(self.run_read("Nested/test", 5, mode="unique"), 0)

    def test_case_fallback_has_a_bound_and_exact_matches_still_work(self):
        for index in range(4096):
            (self.root / f"other{index}.ini").touch()
        (self.root / "Exact.ini").write_bytes(b"small")
        self.assertEqual(self.run_read("absent.ini", 13), 0)
        self.run_read("Exact.ini", 0, b"small")

    def test_links_fifos_and_sockets_are_never_read(self):
        external = self.top / "private"
        external.mkdir()
        other = external / "other"
        other.write_bytes(b"outside")
        (self.root / "linked-dir").symlink_to(external)
        (self.root / "linked-file").symlink_to(other)
        (self.root / "broken").symlink_to(external / "missing")
        os.link(other, self.root / "hardlink")
        os.mkfifo(self.root / "pipe")
        import socket
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            previous = Path.cwd()
            try:
                os.chdir(self.root)
                s.bind("socket")
            finally:
                os.chdir(previous)
            for path in ("linked-dir/other", "linked-file", "broken", "broken/other", "hardlink", "pipe", "socket"):
                self.assertEqual(self.run_read(path, 5), 0)
        self.assertEqual(other.read_bytes(), b"outside")

    def test_noncanonical_names_and_malformed_utf8_are_rejected_before_lookup(self):
        invalid = (b"", b".", b"..", b"../file", b"a/../file", b"a/./file", b"/file", b"a//b", b"a/",
                   b"C:one", b"C:/test", b"a\\file", b"//host/file", b"file:other", b"a|b", b"*.ini",
                   b"bad\x7f", b"bad\x1f", b"a. /b", b"trailing.", b"trailing ", b"long/" + b"a"*256,
                   b"a/"*130 + b"b", b"a"*1024, b"\xc0\x80", b"\xc2", b"\xa0", b"\xe0\x80\x80",
                   b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf1\x80", b"\xf8\x80\x80\x80")
        for path in invalid:
            with self.subTest(path=path):
                self.assertEqual(self.run_read(path, 5), 0)

    def test_interrupted_and_partial_reads_retry_but_trailing_or_missing_bytes_fail(self):
        data = b"0123456789"
        (self.root / "sample").write_bytes(data)
        self.assertGreater(self.run_read("sample", 0, data, mode="short"), 2)
        self.assertGreater(self.run_read("sample", 0, data, mode="interrupt"), 2)
        for mode in ("short-eof", "error", "extra"):
            self.run_read("sample", 13, mode=mode)
        self.assertEqual(self.run_read("sample", 8, mode="oom"), 0)
        self.assertEqual((self.root / "sample").read_bytes(), data)

    def test_snapshot_remains_owned_by_the_caller_after_file_changes(self):
        source = self.root / "sample"
        source.write_bytes(b"initial")
        self.run_read("sample", 0, b"initial", mode="after-success-edit")
        self.assertEqual(source.read_bytes(), b"later")

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "Root bypasses fixture permissions")
    def test_permission_denial_is_reported_without_publishing_data(self):
        source = self.root / "closed"
        source.write_bytes(b"private")
        source.chmod(0)
        try:
            self.assertEqual(self.run_read("closed", 5), 0)
        finally:
            source.chmod(0o600)

    def test_replaced_file_same_size_edit_and_renamed_parent_discard_snapshot(self):
        source = self.root / "sample"
        source.write_bytes(b"owner-original")
        (self.root / "replacement").write_bytes(b"marshmallow!!!")
        self.run_read("sample", 13, mode="replace")
        self.run_read("sample", 13, mode="modify")
        nested = self.root / "nested"
        nested.mkdir()
        (nested / "sample").write_bytes(b"new folder data")
        self.run_read("NESTED/sample", 13, mode="move-dir")


if __name__ == "__main__":
    unittest.main()
