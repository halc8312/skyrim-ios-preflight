"""Check the independent Apple job's source pinning and test coverage contract."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import check_classic_apple as apple


class ClassicAppleChecks(unittest.TestCase):
    def test_sdl_pin_comes_from_the_same_checkout_as_the_renderer(self):
        recipe = (ROOT / "ios/dependencies/build-dependencies.sh").read_text()
        url, commit = apple.sdl_pin(recipe)
        self.assertEqual(url, "https://github.com/libsdl-org/SDL.git")
        self.assertRegex(commit, r"^[a-f0-9]{40}$")
        for bad in ("", recipe + "\n" + recipe, recipe.replace(commit, "release")):
            with self.assertRaises(ValueError):
                apple.sdl_pin(bad)

    def test_committed_subtree_is_identical_with_a_dirty_mirror_or_a_fresh_fetch(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            mirror = base / "mirror"
            mirror.mkdir()

            def git(*args):
                return subprocess.check_output(["git", "-C", str(mirror), *args], text=True).strip()

            git("init", "-q")
            (mirror / "runtime").mkdir()
            sentinel = mirror / "runtime/public.c"
            sentinel.write_bytes(b"int initial;\n")
            (mirror / "elsewhere").write_text("out of scope")
            git("add", "runtime", "elsewhere")
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture")
            revision = git("rev-parse", "HEAD")
            sentinel.write_bytes(b"local edit")
            (mirror / "runtime/untracked").write_text("local only")
            for checkout, name in ((mirror, "local"), (base / "absent", "fresh")):
                target = base / name
                apple.export_subtree(mirror.as_uri(), revision, "runtime", target, checkout)
                self.assertEqual(list(target.iterdir()), [target / "public.c"])
                self.assertEqual((target / "public.c").read_bytes(), b"int initial;\n")
            with self.assertRaises(ValueError):
                apple.export_subtree(mirror.as_uri(), "HEAD", "runtime", base / "bad", mirror)
            self.assertEqual(sentinel.read_bytes(), b"local edit")

    def test_missing_or_skipped_native_fixtures_do_not_pass(self):
        class Fixture(unittest.TestCase):
            @unittest.skip("intentionally unavailable")
            def runTest(self):
                pass

        for cases in ([], [Fixture()]):
            with mock.patch.dict(os.environ), contextlib.redirect_stderr(io.StringIO()):
                with mock.patch.object(unittest.defaultTestLoader, "discover",
                                       side_effect=lambda *a, **k: unittest.TestSuite(cases)):
                    with self.assertRaises(ValueError):
                        apple.native_fixtures(ROOT)

    def test_manual_default_and_classic_use_separate_jobs(self):
        path = ROOT.parent / ".github/workflows/ios-preflight.yml"
        if not path.is_file():
            self.skipTest("published workspace workflow is absent from this standalone source copy")
        header, normal = path.read_text().split("  apple-sdk-preflight:\n")
        normal, classic = normal.split("  classic-skse-apple:\n")
        self.assertIn("type: boolean\n        default: false", header)
        self.assertIn("if: ${{ !inputs.classic_skse }}", normal)
        self.assertIn("if: ${{ inputs.classic_skse }}", classic)
        self.assertIn("build-ios.sh unsigned", normal)
        self.assertIn("runs-on: macos-26", classic)
        self.assertIn("run: python3 scripts/check_classic_apple.py", classic)
        self.assertNotIn("upload-artifact", classic)


if __name__ == "__main__":
    unittest.main()
