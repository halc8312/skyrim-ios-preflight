"""Read Fuz Ro D-oh settings from ordered Data roots with authored INI text."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
NAME = "SKSE/Plugins/Fuz Ro D'oh.ini"
UNREADABLE, WORDS, SKIP = 1, 2, 4


@unittest.skipUnless(os.name == "posix", "Requires POSIX paths")
class FuzSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "fuz"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_fuz.c", PORT / "mod_fuz.c",
                        PORT / "mod_plugins.c", "-o", cls.binary], check=True, timeout=60)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = Path(tmp.name)

    def run_binary(self, *args):
        out = subprocess.check_output([self.binary, *args], text=True, timeout=10).split()
        return tuple(int(v) for v in out)

    def parse(self, text):
        return self.run_binary("parse", text)

    def root(self, name, text, filename=NAME):
        base = self.top / name
        target = base / filename
        target.parent.mkdir(parents=True)
        target.write_bytes(text if isinstance(text, bytes) else text.encode())
        return str(base)

    def test_author_defaults_and_values(self):
        self.assertEqual(self.parse(""), (2, 1, 0, 0))
        self.assertEqual(self.parse("[General]\nWordsPerSecondSilence=3\nSkipEmptyResponses=0\n"), (3, 0, 0, 0))
        self.assertEqual(self.parse("[general]\r\n  wordspersecondsilence = 4 \r\n SKIPEMPTYRESPONSES=7\r\n"), (4, 1, 0, 0))

    def test_sections_comments_bom_and_unknown_keys(self):
        text = "\xef\xbb\xbf; note\n[Other]\nWordsPerSecondSilence=9\n[General]\n# c\nUnknown=1\nWordsPerSecondSilence=5\n"
        self.assertEqual(self.parse(text), (5, 1, 0, 0))
        self.assertEqual(self.parse("WordsPerSecondSilence=9\n"), (2, 1, 0, 0))
        self.assertEqual(self.parse("[General\nWordsPerSecondSilence=9\n"), (2, 1, 0, 0))

    def test_unusable_values_fall_back_and_are_reported(self):
        for value in ("0", "-3", "abc", "", "1.5", "99999999999999999999"):
            self.assertEqual(self.parse(f"[General]\nWordsPerSecondSilence={value}\n"), (2, 1, WORDS, 0), value)
        self.assertEqual(self.parse("[General]\nSkipEmptyResponses=yes\n"), (2, 1, SKIP, 0))
        # a later valid value replaces an earlier invalid one
        self.assertEqual(self.parse("[General]\nWordsPerSecondSilence=x\nWordsPerSecondSilence=6\n"), (6, 1, 0, 0))

    def test_long_lines_and_embedded_nul_are_ignored(self):
        self.assertEqual(self.parse("[General]\nWordsPerSecondSilence=" + "9" * 3000 + "\n"), (2, 1, 0, 0))
        self.assertEqual(self.run_binary("parse", "[General]\nWordsPerSecondSilence=5\n", "20"), (2, 1, 0, 0))
        text = "[General]\nWordsPerSecondSilence=5\nSkipEmptyResponses=0"
        self.assertEqual(self.run_binary("parse", text, str(len(text))), (5, 0, 0, 0))

    def test_later_root_wins_and_missing_file_keeps_defaults(self):
        a = self.root("a", "[General]\nWordsPerSecondSilence=3\n")
        b = self.root("b", "[General]\nWordsPerSecondSilence=4\n", "skse/plugins/FUZ RO D'OH.INI")
        empty = str(self.top / "empty")
        os.mkdir(empty)
        self.assertEqual(self.run_binary("read", a, b), (4, 1, 0, 1))
        self.assertEqual(self.run_binary("read", b, a), (3, 1, 0, 1))
        self.assertEqual(self.run_binary("read", a, empty), (3, 1, 0, 1))
        self.assertEqual(self.run_binary("read", empty, str(self.top / "missing")), (2, 1, 0, 0))
        self.assertEqual(self.run_binary("read"), (2, 1, 0, 0))

    def test_links_aliases_and_oversize_are_unreadable_not_silently_used(self):
        real = self.root("real", "[General]\nWordsPerSecondSilence=3\n")
        linked = self.top / "linked"
        (linked / "SKSE/Plugins").mkdir(parents=True)
        (linked / "SKSE/Plugins/Fuz Ro D'oh.ini").symlink_to(Path(real) / NAME)
        self.assertEqual(self.run_binary("read", real, str(linked)), (2, 1, UNREADABLE, 0))
        alias = self.root("alias", "x", NAME)
        (Path(alias) / "skse/plugins").mkdir(parents=True)
        (Path(alias) / "skse/plugins/fuz ro d'oh.ini").write_bytes(b"x")
        self.assertEqual(self.run_binary("read", real, alias), (2, 1, UNREADABLE, 0))
        big = self.root("big", b"[General]\n" + b";" * (70 * 1024))
        self.assertEqual(self.run_binary("read", big), (2, 1, UNREADABLE, 0))


if __name__ == "__main__":
    unittest.main()
