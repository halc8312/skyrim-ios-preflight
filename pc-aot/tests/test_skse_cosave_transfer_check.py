"""Run the read-only local Classic pair checker with real files on Windows and POSIX."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "experiments" / "skse-cosave" / "compare_pair.py"
SPEC = importlib.util.spec_from_file_location("classic_local_compare", SCRIPT)
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


def base(number, tail=b""):
    # Only the checked Classic file prefix is part of this read-only fixture.
    return b"TESV_SAVEGAME" + struct.pack("<III", 8, 9, number) + tail


def companion(marker):
    chunk = struct.pack("<III", 0x41424344, 1, len(marker)) + marker
    owner = struct.pack("<III", 0x48554e54, 1, len(chunk)) + chunk
    return struct.pack("<4s4I", b"SKSE", 1, 0x01070030, 0x09200000, 1) + owner


class TransferTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name).resolve()
        for name in ("original", "from phone"):
            (self.root / name).mkdir()
        self.a = self.root / "original" / "Quicksave.ess"
        self.b = self.root / "from phone" / "Returned.ess"
        self.a.write_bytes(base(51, bytes(range(256)) * 1300))
        self.sa = self.a.with_suffix(".skse")
        self.sa.write_bytes(companion(bytes(range(256)) * 1080))
        self.sb = self.b.with_suffix(".SKSE")
        shutil.copyfile(self.a, self.b)
        shutil.copyfile(self.sa, self.sb)

    def command(self):
        return subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.a), str(self.b)],
                              capture_output=True, text=True, check=False)

    def test_two_renamed_ordinary_copies_match_with_numeric_output_and_no_writes(self):
        paths = [self.a, self.b, self.sa, self.sb]
        before = [(x.read_bytes(), x.stat().st_mtime_ns) for x in paths]
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ess_match=1 skse_match=1", result.stdout)
        self.assertNotIn(str(self.root), result.stdout + result.stderr)
        self.assertEqual(before, [(x.read_bytes(), x.stat().st_mtime_ns) for x in paths])
        self.assertEqual(set(self.root.rglob("*")), {*paths, self.a.parent, self.b.parent})

    def test_each_member_detects_replaced_quicksave_of_the_same_size_and_timestamp(self):
        for b, backup, field in [(self.b, self.a, "ess_match"), (self.sb, self.sa, "skse_match")]:
            with self.subTest(member=field):
                data = bytearray(b.read_bytes())
                data[-7] ^= 0xdf
                b.write_bytes(data)
                stat = backup.stat()
                os.utime(b, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                result = self.command()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(field + "=0", result.stdout)
                other = "skse_match" if field == "ess_match" else "ess_match"
                self.assertIn(other + "=1", result.stdout)
                shutil.copyfile(backup, b)

    def test_same_file_hardlink_missing_sibling_and_malformed_headers(self):
        with self.assertRaisesRegex(checker.PairCheckError, "same_file"):
            checker.compare(self.a, self.a)
        self.sb.unlink()
        with self.assertRaisesRegex(checker.PairCheckError, "missing_or_ambiguous"):
            checker.compare(self.a, self.b)
        os.link(self.sa, self.sb)
        with self.assertRaisesRegex(checker.PairCheckError, "same_file"):
            checker.compare(self.a, self.b)
        self.sb.unlink()
        self.sb.write_bytes(struct.pack("<4s4I", b"SKSE", 1, 0x02000000, 0x01060000, 0))
        with self.assertRaisesRegex(checker.PairCheckError, "companion_header"):
            checker.compare(self.a, self.b)
        shutil.copyfile(self.sa, self.sb)
        self.b.write_bytes(base(1)[:18])
        result = self.command()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stderr.strip(), "check_error=base_header")

    def test_one_local_sibling_and_source_under_size_bounds(self):
        if os.path.normcase("save.skse") != os.path.normcase("save.SKSE"):
            second = self.b.with_suffix(".skse")
            if not second.exists():
                second.write_bytes(companion(b"second"))
                with self.assertRaisesRegex(checker.PairCheckError, "missing_or_ambiguous"):
                    checker.compare(self.a, self.b)
                second.unlink()
        with patch.object(checker, "MAX_ESS", 32):
            with self.assertRaisesRegex(checker.PairCheckError, "file_size"):
                checker.compare(self.a, self.b)
        with self.assertRaisesRegex(checker.PairCheckError, "select_ess"):
            checker.compare(self.sa, self.b)

    def test_ascii_aliases_do_not_merge_distinct_unicode_stems(self):
        self.b = self.b.rename(self.b.with_name("Straße.ess"))
        self.sb = self.sb.rename(self.b.with_suffix(".SKSE"))
        unrelated = self.b.with_name("STRASSE.skse")
        if unrelated.exists():
            self.skipTest("volume aliases these two Unicode stems")
        shutil.copyfile(self.sa, unrelated)
        paths = [self.a, self.sa, self.b, self.sb, unrelated]
        before = [(p.read_bytes(), p.stat().st_mtime_ns) for p in paths]
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ess_match=1 skse_match=1", result.stdout)
        self.assertEqual(before, [(p.read_bytes(), p.stat().st_mtime_ns) for p in paths])
        self.sb.unlink()
        with self.assertRaisesRegex(checker.PairCheckError, "missing_or_ambiguous_companion"):
            checker.compare(self.a, self.b)

    def test_case_duplicate_base_is_ambiguous_even_if_companion_is_unique(self):
        alias = self.b.with_name(self.b.name.upper())
        if alias.exists():
            self.skipTest("case-insensitive volume")
        shutil.copyfile(self.b, alias)
        with self.assertRaisesRegex(checker.PairCheckError, "missing_or_ambiguous_base"):
            checker.compare(self.a, self.b)

    def test_symlinks_are_rejected_when_available_and_help_does_not_open_inputs(self):
        path = self.b.parent / "link.ess"
        try:
            path.symlink_to(self.a)
        except (NotImplementedError, OSError):
            path = None
        if path:
            with self.assertRaisesRegex(checker.PairCheckError, "link_or_nonlocal"):
                checker.compare(path, self.b)
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)

    def test_all_four_files_stay_pinned_until_both_companions_have_been_read(self):
        actual = checker.fingerprint
        attempts = []
        def mutate(file, kind):
            value = actual(file, kind)
            if not attempts:
                try:
                    with self.a.open("r+b") as original:
                        original.seek(-1, os.SEEK_END)
                        original.write(b"x")
                    attempts.append("changed")
                except PermissionError:
                    attempts.append("write-blocked")
            return value
        with patch.object(checker, "fingerprint", mutate):
            if os.name == "nt":
                self.assertTrue(checker.compare(self.a, self.b)["ess_match"])
                self.assertEqual(attempts, ["write-blocked"])
            else:
                with self.assertRaisesRegex(checker.PairCheckError, "file_changed"):
                    checker.compare(self.a, self.b)
                self.assertEqual(attempts, ["changed"])


if __name__ == "__main__":
    unittest.main()
