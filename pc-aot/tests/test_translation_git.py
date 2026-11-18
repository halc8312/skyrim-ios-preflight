"""Real-Git regression fixtures for shared Windows/WSL checkout preflight."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("translation_git", Path(__file__).parents[1] / "scripts/prepare_translation.py")
translation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(translation)


@unittest.skipUnless(shutil.which("git"), "Git required for checkout regression fixtures")
class TranslatorGitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / "translator"
        self.repo.mkdir()
        self.git("init", "-q")
        # Materialize a real Windows-style checkout, including its index stat
        # data, then inspect it with Linux-style defaults. All config changes
        # here affect only this disposable fixture, never the vendor checkout.
        self.git("config", "core.autocrlf", "true")
        (self.repo / "main.c").write_bytes(b"int value = 1;\nint other = 2;\n")
        (self.repo / ".gitattributes").write_bytes(b"raw.txt -text\n")
        (self.repo / "raw.txt").write_bytes(b"literal\nbytes\n")
        (self.repo / "binary.bin").write_bytes(b"\x00binary\nbytes\n")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "Synthetic fixture")
        self.source = {"path": "translator", "revision": self.git("rev-parse", "HEAD").strip()}
        self.recheckout()
        self.assertIn(b"\r\n", (self.repo / "main.c").read_bytes())
        self.git("config", "core.autocrlf", "false")
        # WSL sees different stat metadata than Windows; force content checks.
        stamp = (self.repo / "main.c").stat().st_mtime + 5
        os.utime(self.repo / "main.c", (stamp, stamp))
        self.root_patch = patch.object(translation, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              capture_output=True, text=True, timeout=10).stdout

    def recheckout(self):
        # Remove only the four known files created in this temporary fixture so
        # Git actually rematerializes them under the chosen EOL setting.
        for name in ("main.c", ".gitattributes", "raw.txt", "binary.bin"):
            (self.repo / name).unlink()
        self.git("checkout-index", "--index", "--all")
        self.git("status", "--porcelain")

    def state(self):
        before = {name: (self.repo / ".git" / name).read_bytes() for name in ("config", "index")}
        files = {p: p.read_bytes() for p in self.repo.iterdir() if p.is_file()}
        result = translation.translator_state(self.source)
        self.assertEqual(before, {name: (self.repo / ".git" / name).read_bytes() for name in before})
        self.assertEqual(files, {p: p.read_bytes() for p in files})
        return result

    def test_clean_crlf_checkout_and_lf_checkout_match_without_writes(self):
        self.assertIn(" M main.c", self.git("status", "--porcelain"))
        result = self.state()
        self.assertTrue(result["clean"], result)
        self.assertTrue(result["matches"], result)
        self.assertEqual(result["actual_revision"], self.source["revision"])
        self.assertEqual(result["git"]["status_config_overrides"], {"core.autocrlf": "true"})
        self.assertEqual(result["git"]["version"], self.git("--version").strip())
        self.assertTrue(Path(result["git"]["executable"]).is_file())
        self.recheckout()
        self.assertNotIn(b"\r\n", (self.repo / "main.c").read_bytes())
        self.assertTrue(self.state()["matches"])

    def test_source_edits_still_block(self):
        for content in (b"int value = 9;\r\nint other = 2;\r\n",
                        b"int value = 1; \r\nint other = 2;\r\n",
                        b"int value = 1;\rint other = 2;\r"):
            with self.subTest(content=content):
                (self.repo / "main.c").write_bytes(content)
                result = self.state()
                self.assertFalse(result["clean"])
                self.assertFalse(result["matches"])

    def test_staged_edits_still_block(self):
        (self.repo / "main.c").write_bytes(b"int value = 9;\n")
        self.git("add", "main.c")
        self.assertFalse(self.state()["matches"])

    def test_untracked_files_still_block(self):
        (self.repo / "untracked-source.c").write_bytes(b"int added;\n")
        result = self.state()
        self.assertFalse(result["clean"])
        self.assertFalse(result["matches"])

    def test_binary_and_explicit_nontext_eol_changes_still_block(self):
        for filename, original in (("raw.txt", b"literal\nbytes\n"),
                                   ("binary.bin", b"\x00binary\nbytes\n")):
            with self.subTest(filename=filename):
                (self.repo / filename).write_bytes(original.replace(b"\n", b"\r\n"))
                self.assertFalse(self.state()["matches"])
                (self.repo / filename).write_bytes(original)
        self.assertTrue(self.state()["matches"])

    def test_exact_pin_remains_required(self):
        self.source["revision"] = "0" * 40
        result = self.state()
        self.assertTrue(result["clean"])
        self.assertFalse(result["matches"])


if __name__ == "__main__":
    unittest.main()
