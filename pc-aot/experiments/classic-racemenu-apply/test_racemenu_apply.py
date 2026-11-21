"""Verify the one-shot policy and the owned Classic slider stage."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
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

# Only the FaceMorphs part of the stock change form is exercised. Each
# projection enters the original flag check with the normal live registers
# and bypasses head/skin fields outside that subrecord.
def face_morph_slice(data, address, name, gate, head, morphs, done, absent):
    start, end = function_span(data, address)
    body = data[start:end]
    entry, gate, head, morphs, done = (
        f"L_{label}:\n".encode() for label in (address, gate, head, morphs, done)
    )
    prologue = body[:body.index(entry)].replace(f"f_{address}(".encode(), name + b"(", 1)
    return (prologue + b"    goto " + gate[:-2] + b";\n" +
            body[body.index(gate):body.index(head)] + head +
            b"    goto " + morphs[:-2] + b";\n" +
            body[body.index(morphs):body.index(done)] + done +
            f"L_{absent}:\n    SPILL(); return;\n}}\n".encode())


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

    def test_pinned_native_face_morph_save_and_load(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation.")
        gen = Path(generation) / "gen"
        runtime = Path(generation) / "source/runtime"
        save = (gen / "exe_56.c").read_bytes()
        load = (gen / "exe_58.c").read_bytes()
        self.assertEqual(hashlib.sha256(save).hexdigest(),
                         "e68df5295941a1ccb5d202ac12342239cb2fda01b0b330b8241a7655b3cafdc7")
        self.assertEqual(hashlib.sha256(load).hexdigest(),
                         "2378552ae761e8b2bc612914f21c8a740dcd17dfd318a69c8c6927b9ee85466d")
        fixture = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
        for function in ("560150", "561180"):
            start, end = function_span(save, function)
            fixture += save[start:end]
        fixture += face_morph_slice(save, "560B10", b"test_save_morphs",
                                    "560B9A", "560BA8", "560C6F", "560D1E", "560D1F")
        fixture += face_morph_slice(load, "56A170", b"test_load_morphs",
                                    "56A27B", "56A28A", "56A54B", "56A660", "56A69E")
        private = HERE.parents[1] / "private"
        if private.resolve() != private:
            raise ValueError("Generated sources must stay in private/")
        private.mkdir(exist_ok=True)
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if os.name == "posix" and sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        with tempfile.TemporaryDirectory(prefix="racemenu-save-", dir=private) as tmp:
            directory = Path(tmp)
            (directory / "face_morph_save.inc").write_bytes(fixture)
            entry = directory / "face_morph_save.c"
            entry.write_text("#include <face_morph_save.inc>\n", encoding="ascii")
            binary = directory / ("face-morph-save.exe" if os.name == "nt" else "face-morph-save")
            subprocess.run([self.compiler, *flags, "-isystem", directory, "-isystem", gen, "-isystem", runtime,
                            entry, HERE / "test_face_morph_save.c", "-o", binary], check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=30)

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
