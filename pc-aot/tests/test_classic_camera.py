"""Camera stager and transition equivalence, with only the owner lift on disk."""
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
CAMERA = ROOT / "experiments/classic-camera"
sys.path[:0] = [str(CAMERA), str(ROOT / "scripts")]
import stage_camera as camera
from classic_skse_guest import validate_generation


class CameraStageTests(unittest.TestCase):
    def test_patch_is_local_and_hash_bound(self):
        header = b'#include "funcs_exe.h"\n'
        body = camera.ENTRY + b"    " + camera.EXIT + b"\n}\n"
        unit = header + b"\n" + body
        with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
            camera.stage_camera(unit)
        with mock.patch.object(camera, "CAMERA_SHA256", sha256(unit).hexdigest()):
            staged = camera.stage_camera(unit)
        self.assertTrue(staged.startswith(camera.DECLARATION + header))
        self.assertIn(b"\n" + camera.ENTRY + camera.CAPTURE, staged)
        self.assertEqual(staged.removeprefix(camera.DECLARATION).replace(camera.CAPTURE, b"", 1)
                         .replace(camera.OBSERVED_EXIT, camera.EXIT, 1), unit)
        duplicate_exit = unit.replace(camera.EXIT, camera.EXIT + b"\n    return;")
        with mock.patch.object(camera, "CAMERA_SHA256", sha256(duplicate_exit).hexdigest()):
            with self.assertRaisesRegex(ValueError, "ambiguous"):
                camera.stage_camera(duplicate_exit)

    def test_camera_state_reader(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as d:
            self.compile_and_run(Path(d), compiler, [CAMERA / "test_camera_state.c"])

    def test_reviewed_transition_and_original_callbacks(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not generation or not compiler:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a C compiler")
        generation = Path(generation)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        if (state.get("mode") != "original-guest-bodies" or state.get("fixed_native_replacements") is not False
                or state.get("generation_finished") is not True or state.get("inputs_unchanged") is not True):
            raise ValueError("The camera probe requires the reviewed unmodified original-body generation")
        runtime = generation / "source/runtime"
        pinned_headers = {
            "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
            "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
            "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
        }
        for name, expected in pinned_headers.items():
            self.assertEqual(sha256((runtime / name).read_bytes()).hexdigest(), expected, name)
        source = generation / "gen" / camera.CAMERA_UNIT
        original = source.read_bytes()
        staged = camera.stage_camera(original)
        headers = ["-isystem", str(generation / "gen"),
                   "-isystem", str(runtime), "-I", str(ROOT / "native")]
        with tempfile.TemporaryDirectory(prefix="classic-camera-") as d:
            directory = Path(d)
            unit = directory / camera.CAMERA_UNIT
            unit.write_bytes(staged)
            subprocess.run([compiler, "-std=gnu11", "-w", "-fsyntax-only", *headers, unit],
                           check=True, timeout=60)
            fixture = directory / "camera_fixture.c"
            baseline = camera.camera_function(original).replace(b"f_6533D0(", b"unobserved_camera_transition(", 1)
            fixture.write_bytes(b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
                                + camera.DECLARATION + baseline + camera.camera_function(staged))
            self.compile_and_run(directory, compiler,
                                 [fixture, CAMERA / "test_camera_lift.c", ROOT / "native/guest_memory.c"], headers)
        self.assertEqual(source.read_bytes(), original)

    def compile_and_run(self, root, compiler, files, headers=()):
        binary = root / "test_camera"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(CAMERA)]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        subprocess.run([compiler, *flags, *headers, CAMERA / "camera_state.c", *files, "-o", binary],
                       check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
