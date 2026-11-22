"""Verify the private, read-only ECE face witness against the fixed Classic lift."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
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


def read_owned_nose_ordinals(data):
    """Read the ordinary morph table of the exact LE ECE 1.59 head TRI."""
    path = data
    for part in ("meshes", "actors", "character", "character assets", "femaleheadchargen.tri"):
        matches = [item for item in path.iterdir() if item.name.lower() == part]
        if len(matches) != 1 or matches[0].is_symlink():
            raise ValueError(f"Expected one regular owner TRI path component: {part}")
        path = matches[0]
    payload = path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != (
        "39f940fcf6f2b3cb3f9ffefca09e5bc82ea676ec12251e4e61cf1abd5df09475"
    ):
        raise ValueError("Expected the audited default ECE 1.59 female head TRI")
    header = struct.Struct("<8s14I")
    if header.unpack_from(payload) != (b"FRTRI003", 996, 1706, 0, 0, 0, 898, 1, 124, 0, 0, 0, 0, 0, 0):
        raise ValueError("Unexpected default ECE TRI header")
    vertices, triangles, uv, morphs = 996, 1706, 898, 124
    offset = header.size + vertices * 12 + triangles * 12 + uv * 8 + triangles * 12
    found = {}
    for index in range(morphs):
        size, = struct.unpack_from("<I", payload, offset)
        offset += 4
        name = payload[offset:offset + size]
        offset += size + 4 + vertices * 6  # Name, delta scale and packed vertex deltas.
        if name in (b"NoseUp\0", b"NoseDown\0"):
            if name in found:
                raise ValueError("Duplicate stock nose morph record")
            found[name] = index
    if offset != len(payload) or len(found) != 2:
        raise ValueError("Expected both nose records in the ordinary ECE morph table")
    return found[b"NoseUp\0"], found[b"NoseDown\0"]


class EceNoseWitnessTests(unittest.TestCase):
    def test_unreviewed_units_rejected(self):
        for unit in (*DIGESTS, "other.c"):
            with self.assertRaisesRegex(ValueError, "reviewed original Classic lift"):
                stage(unit, b"authored")

    def test_original_anchors_and_read_only_witness(self):
        self.run_original_fixture()

    def test_owned_default_tri_lookup_only(self):
        owner_data = os.environ.get("SKYRIM_TEST_ECE_DEFAULT_DATA")
        if not owner_data:
            self.skipTest("Set SKYRIM_TEST_ECE_DEFAULT_DATA to the owner ECE 1.59 00 Main directory")
        up, down = read_owned_nose_ordinals(Path(owner_data))
        self.assertEqual((up, down), (13, 12))
        self.run_original_fixture(str(up), str(down))

    def run_original_fixture(self, *arguments):
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
            fixture += b"void test_lookup_probe(CPU *, uint32_t);\n"
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
                for function in {"exe_66.c": ("5A0710", "5A0660"), "exe_67.c": ("5A4870",),
                                 "exe_68.c": ("5A7570", "5A75F0")}.get(name, ()):
                    start, end = function_span(staged, function)
                    fixture += staged[start:end]
                if name == "exe_67.c":
                    start, end = function_span(staged, "5A5B70")
                    point = staged.index(b"    ece_nose_lookup_end(", start, end)
                    point = staged.index(b"\n", point, end)
                    fixture += staged[start:point].replace(b"void f_5A5B70(", b"void test_original_lookup(", 1)
                    fixture += b"\n    SPILL(); return;\n}\n"
                    # Rejoin the untouched release branch. The stock context swap
                    # and geometry mutators between these anchors never run.
                    release = staged.index(b"\nL_5A5CAD:\n", point, end)
                    fixture += staged[start:point].replace(b"void f_5A5B70(", b"void test_original_lookup_only(", 1)
                    fixture += (b"\n    SPILL();\n    test_lookup_probe(c, eax == 0 ? M32(esp + 0xCu) : 0);\n"
                                b"    RELOAD();\n    goto L_5A5CAD;\n")
                    fixture += staged[release:end]
            selectors = (gen / "exe_285.c").read_bytes()
            self.assertEqual(hashlib.sha256(selectors).hexdigest(),
                             "d44f89f39fca50b0f56aac0fbdd8bd394c6c75f2489327038acc6bcfb2f1d075")
            for function in ("AEE130", "AEE3D0"):
                start, end = function_span(selectors, function)
                fixture += selectors[start:end]
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
                            HERE / "ece_nose_witness.c", HERE / "ece_head_geometry.c",
                            HERE / "test_ece_nose_witness.c", "-o", binary],
                           check=True, timeout=60)
            subprocess.run([binary, *arguments], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
