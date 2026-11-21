"""Verify the one-shot policy and the owned Classic slider stage."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from stage_slider import (APPLY, BEFORE_APPLY, CLEAR_COMMIT, COMMIT_STATE, DECLARATIONS, DESCRIPTOR,
                          LOCALS, MARK_ADMIT, MARK_AFTER, MARK_BEFORE, SETTER_ADMIT, SETTER_AFTER,
                          SETTER_BEFORE, SLIDER_UNIT, SUBSTITUTE, UPDATE_UNIT, function_span, stage_slider)


COMMIT_SNIPPETS = {
    "commit-state": COMMIT_STATE, "clear-commit": CLEAR_COMMIT, "setter-admit": SETTER_ADMIT,
    "setter-before": SETTER_BEFORE, "setter-after": SETTER_AFTER, "mark-admit": MARK_ADMIT,
    "mark-before": MARK_BEFORE, "mark-after": MARK_AFTER,
}

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[1] / "ios/runtime-port"


class RaceMenuApplyTests(unittest.TestCase):
    def setUp(self):
        self.compiler = shutil.which(os.environ.get("CC", "cc"))
        if not self.compiler:
            self.skipTest("C compiler unavailable")

    def test_nose_height_admission(self):
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory(prefix="racemenu-apply-") as tmp:
            binary = Path(tmp) / ("apply.exe" if os.name == "nt" else "apply")
            subprocess.run([self.compiler, *flags, HERE / "racemenu_apply.c",
                            HERE / "test_racemenu_apply.c", "-o", binary], check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=30)

    def test_staged_substitution_checks_the_stock_descriptor(self):
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory(prefix="racemenu-substitute-") as tmp:
            folder = Path(tmp)
            (folder / "substitute.inc").write_bytes(SUBSTITUTE)
            (folder / "before.inc").write_bytes(BEFORE_APPLY)
            for name, chunk in COMMIT_SNIPPETS.items():
                (folder / f"{name}.inc").write_bytes(chunk)
            binary = folder / ("substitute.exe" if os.name == "nt" else "substitute")
            subprocess.run([self.compiler, *flags, "-I", folder, HERE / "test_substitute.c",
                            "-o", binary], check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=30)

    def test_unreviewed_units_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
            stage_slider(b"authored slider", b"authored update")

    def test_pinned_stage_preserves_stock_calls_and_compiles(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation.")
        gen = Path(generation) / "gen"
        runtime = Path(generation) / "source/runtime"
        original = (gen / SLIDER_UNIT).read_bytes()
        staged = stage_slider(original, (gen / UPDATE_UNIT).read_bytes())
        start, end = function_span(staged, "882290")
        body = staged[start:end]
        self.assertLess(body.index(SUBSTITUTE), body.index(APPLY))
        self.assertIn(DESCRIPTOR, body.replace(SUBSTITUTE, b"", 1))
        stripped = staged.replace(DECLARATIONS, b"", 1).replace(LOCALS, b"", 1)
        stripped = stripped.replace(b"    skyrim_racemenu_preview_base = eax;\n", b"", 1)
        for chunk in (SUBSTITUTE, BEFORE_APPLY, *[part for part in COMMIT_SNIPPETS.values() if part != COMMIT_STATE]):
            self.assertEqual(stripped.count(chunk), 1)
            stripped = stripped.replace(chunk, b"", 1)
        self.assertEqual(stripped, original)
        with tempfile.TemporaryDirectory(prefix="racemenu-stage-") as tmp:
            path = Path(tmp) / SLIDER_UNIT
            path.write_bytes(staged)
            subprocess.run([self.compiler, "-std=gnu11", "-fsyntax-only", "-w",
                            "-I", gen, "-I", runtime, path], check=True, timeout=60)
        self.assertEqual((gen / SLIDER_UNIT).read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
