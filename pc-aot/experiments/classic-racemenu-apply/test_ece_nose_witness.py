"""Verify the private, read-only ECE face witness against the fixed Classic lift."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from stage_ece_nose_witness import DIGESTS, DECLARATION, stage
from stage_slider import function_span

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation


class EceNoseWitnessTests(unittest.TestCase):
    def test_unreviewed_units_rejected(self):
        for unit in (*DIGESTS, "other.c"):
            with self.assertRaisesRegex(ValueError, "reviewed original Classic lift"):
                stage(unit, b"authored")

    def test_original_anchors_and_read_only_witness(self):
        owner = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not owner or not compiler:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a host compiler")
        generation = Path(owner)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        self.assertEqual(state["mode"], "original-guest-bodies")
        self.assertIs(state["generation_finished"], True)
        self.assertIs(state["inputs_unchanged"], True)
        self.assertIs(state["fixed_native_replacements"], False)
        gen = generation / "gen"
        runtime = generation / "source/runtime"

        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Generated sources must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ece-nose-witness-", dir=private) as tmp:
            directory = Path(tmp)
            fixture = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n' + DECLARATION
            for name in DIGESTS:
                original = (gen / name).read_bytes()
                staged = stage(name, original)
                lines = iter(staged.splitlines(keepends=True))
                for line in original.splitlines(keepends=True):
                    self.assertIn(line, lines, name)
                staged_file = directory / name
                staged_file.write_bytes(staged)
                subprocess.run([compiler, "-std=gnu11", "-fsyntax-only", "-w",
                                "-I", HERE, "-I", gen, "-I", runtime, staged_file], check=True, timeout=60)
                # Leave the menu guards and geometry flow in their full, checked
                # units. Run the pinned fresh-parse wrapper and morph selector
                # against the same authored memory/geometry fixture.
                for function in {"exe_66.c": ("5A0710",), "exe_68.c": ("5A7570", "5A75F0")}.get(name, ()):
                    start, end = function_span(staged, function)
                    fixture += staged[start:end]
                if name == "exe_67.c":
                    start, end = function_span(staged, "5A5B70")
                    point = staged.index(b"    ece_nose_lookup_end(", start, end)
                    point = staged.index(b"\n", point, end)
                    fixture += staged[start:point].replace(b"void f_5A5B70(", b"void test_original_lookup(", 1)
                    fixture += b"\n    SPILL(); return;\n}\n"
            (directory / "ece_nose_lift.inc").write_bytes(fixture)
            entry = directory / "ece_nose_lift.c"
            entry.write_text("#include <ece_nose_lift.inc>\n", encoding="ascii")
            binary = directory / "test_ece_nose_witness"
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                if sys.platform.startswith("linux"):
                    flags += ["-fno-pie", "-no-pie"]
            subprocess.run([compiler, *flags, "-I", HERE, "-isystem", directory,
                            "-isystem", gen, "-isystem", runtime, entry,
                            HERE / "ece_nose_witness.c", HERE / "test_ece_nose_witness.c", "-o", binary],
                           check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
