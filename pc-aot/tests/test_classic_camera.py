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
import stage_camera_render as render
from classic_skse_guest import validate_generation

LIFT_HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}
LIFT_REGISTERS = (b"    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], "
                  b"esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];\n"
                  b"    Flags f = c->flags;\n")


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

    def test_render_handoff_staging(self):
        body = render.ENTRY + render.CALL + b"}\n"
        unit = b'#include "funcs_exe.h"\n\n' + body
        with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
            render.stage_camera_render(unit)
        with mock.patch.object(render, "RENDER_SHA256", sha256(unit).hexdigest()):
            staged = render.stage_camera_render(unit)
        self.assertIn(render.OBSERVE + render.CALL, staged)
        self.assertEqual(staged.removeprefix(render.DECLARATION).replace(render.CAPTURE, b"", 1)
                         .replace(render.OBSERVE, b"", 1), unit)

    def test_reviewed_render_handoff_stages_and_compiles(self):
        generation, headers, compiler = self.owned_lift()
        source = generation / "gen" / render.RENDER_UNIT
        original = source.read_bytes()
        staged = render.stage_camera_render(original)
        self.assertEqual(staged.removeprefix(render.DECLARATION).replace(render.CAPTURE, b"", 1)
                         .replace(render.OBSERVE, b"", 1), original)
        with tempfile.TemporaryDirectory(prefix="classic-camera-render-") as d:
            unit = Path(d) / render.RENDER_UNIT
            unit.write_bytes(staged)
            subprocess.run([compiler, "-std=gnu11", "-w", "-fsyntax-only", *headers, unit],
                           check=True, timeout=60)
        self.assertEqual(source.read_bytes(), original)

    def test_camera_render_reader(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as d:
            self.compile_and_run(Path(d), compiler, [CAMERA / "test_camera_render.c"])

    def test_camera_state_reader(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as d:
            self.compile_and_run(Path(d), compiler, [CAMERA / "test_camera_state.c"])

    def test_app_reports_only_valid_changes(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler unavailable")
        runtime = ROOT / "ios/runtime-port"
        with tempfile.TemporaryDirectory() as d:
            result = self.compile_and_run(Path(d), compiler,
                                          [runtime / "mod_camera_host.c", ROOT / "tests/native/test_mod_camera_host.c"],
                                          ["-I", str(runtime)])
        self.assertEqual(result.stderr.splitlines(), [
            "IOS_CAMERA_STATE slot=0",
            "IOS_CAMERA_BODY schema=1 slot=0 arms=4 body=5",
            "IOS_CAMERA_BODY schema=1 slot=0 arms=4 body=4",
            "IOS_CAMERA_RENDER schema=1 first=1 second=0",
            "IOS_CAMERA_RENDER schema=1 first=0 second=2",
            "IOS_CAMERA_STATE slot=12",
        ])

    def test_reviewed_transition_and_original_callbacks(self):
        generation, headers, compiler = self.owned_lift()
        source = generation / "gen" / camera.CAMERA_UNIT
        original = source.read_bytes()
        staged = camera.stage_camera(original)
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

    def owned_lift(self):
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
        for name, expected in LIFT_HEADERS.items():
            self.assertEqual(sha256((runtime / name).read_bytes()).hexdigest(), expected, name)
        return generation, ["-isystem", str(generation / "gen"),
                            "-isystem", str(runtime), "-I", str(ROOT / "native")], compiler

    def test_reviewed_visibility_and_arm_root_release_blocks(self):
        generation, headers, compiler = self.owned_lift()
        visibility_source = generation / "gen/exe_135.c"
        release_source = generation / "gen/exe_133.c"
        visibility, release = visibility_source.read_bytes(), release_source.read_bytes()
        self.assertEqual(sha256(visibility).hexdigest(),
                         "ce9edc51a226cc1279efaaf7380a649b3be9db8672625f34f9fff3df86f70a6e")
        self.assertEqual(sha256(release).hexdigest(),
                         "7390b8e9b03ec4bbc8b1040b5ecb31149389ee3084547876a569ef69e9557142")

        # Run the reviewed blocks against authored roots. Exits stop before
        # unrelated transforms or teardown work and never run the game.
        visibility_start = visibility.index(b"\nL_748835:\n", visibility.index(b"\nvoid f_7487B0(")) + 1
        visibility_end = visibility.index(b"\nL_7488AE:\n", visibility_start)
        release_start = release.index(b"    edi = ", release.index(b"\nL_73A689:\n"))
        release_end = release.index(b"\nL_73A6BB:\n", release_start)
        with tempfile.TemporaryDirectory(prefix="classic-camera-roots-") as d:
            directory = Path(d)
            fixture = directory / "root_blocks.c"
            fixture.write_bytes(
                b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
                b"int classic_camera_visibility_block(CPU *c) {\n" + LIFT_REGISTERS +
                b"    goto L_748835;\n" + visibility[visibility_start:visibility_end] +
                b"\nL_7488AE: return 1;\nL_7489B6: return 0;\n}\n"
                b"void classic_camera_release_block(CPU *c) {\n" + LIFT_REGISTERS +
                release[release_start:release_end] +
                b"\nL_73A6BB: SPILL(); return;\n}\n"
            )
            self.compile_and_run(directory, compiler, [fixture, CAMERA / "test_camera_root_lift.c"], headers)
        self.assertEqual(visibility_source.read_bytes(), visibility)
        self.assertEqual(release_source.read_bytes(), release)

    def test_reviewed_node_updates_and_render_arms_scope(self):
        generation, headers, compiler = self.owned_lift()
        pinned = {
            "exe_129.c": "930fff20445acd19639369a300f6fb7725721defa106108aa4d91497f1a734c9",
            "exe_132.c": "ad21bbdf90ee77aa0747db86732ae10210c736778013e4966e56f4ac15b7b0df",
            "exe_275.c": "c980cb667ac15d7f1bbe6df18de61cb550dd8319719eac53e5f74da4cd33a716",
            "exe_107.c": "c0a140d6fe5a3c86e3f0c970e669c8911f787ce14a72ff7da352e1c0c82e85d7",
        }
        units = {name: (generation / "gen" / name).read_bytes() for name in pinned}
        for name, digest in pinned.items():
            self.assertEqual(sha256(units[name]).hexdigest(), digest, name)

        def excerpt(unit, begin, end):
            start = unit.index(begin) + len(begin)
            return unit[start:unit.index(end, start)]

        body = excerpt(units["exe_129.c"], b"\nL_724244:\n",
                       b"\n    edx = (uint32_t)((uint32_t)(ebp + 0xCu));")
        arms = excerpt(units["exe_132.c"], b"\nL_73500F:\n", b"\nL_73502D:")
        render = excerpt(units["exe_107.c"], b"\nL_69B930:\n", b"\n    (void)sub8(")
        update_entry = b"void f_AAF320(CPU *c) {\n"
        update = update_entry + excerpt(units["exe_275.c"], b"\n" + update_entry, b"\n}\n") + b"\n}\n"

        with tempfile.TemporaryDirectory(prefix="classic-camera-update-") as d:
            directory = Path(d)
            fixture = directory / "update_blocks.c"
            wrapper = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n' + update
            for name, block in ((b"body_update", body), (b"arms_update", arms), (b"render_arms", render)):
                wrapper += (b"void classic_camera_" + name + b"(CPU *c) {\n" + LIFT_REGISTERS + block
                            + b"\n    SPILL();\n}\n")
            fixture.write_bytes(wrapper)
            self.compile_and_run(directory, compiler, [fixture, CAMERA / "test_camera_update_lift.c"], headers)

    def compile_and_run(self, root, compiler, files, headers=()):
        binary = root / "test_camera"
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(CAMERA)]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        subprocess.run([compiler, *flags, *headers, CAMERA / "camera_state.c", *files, "-o", binary],
                       check=True, timeout=60)
        return subprocess.run([binary], check=True, timeout=30, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
