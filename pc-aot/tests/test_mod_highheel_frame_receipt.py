"""Check the frame-phase receipt and its pinned wait call against the original."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_mod_hooks_binding import write_fixture_headers

ROOT = Path(__file__).resolve().parents[1]
ABI = ROOT / "ios/private-abi"
HIGHHEELS = ROOT / "experiments/classic-highheels"
NATIVE = ROOT / "native"
FIXTURE = ROOT / "tests/native/test_highheel_frame_receipt.c"


@unittest.skipUnless(os.name == "posix", "Requires a POSIX C compiler")
class FrameReceiptTests(unittest.TestCase):
    def setUp(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler unavailable")
        self.compile = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-O2",
                        "-DPC_GUEST_MOD_HOOKS_COMPILED=65536u", "-I", ABI,
                        "-I", ROOT / "experiments/fuz-voice"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            self.compile += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                self.compile += ["-fno-pie", "-no-pie"]

    def run_fixture(self, directory, *args):
        binary = Path(directory) / "heel-frame-receipt"
        subprocess.run([*self.compile, *args, FIXTURE, ABI / "pc_guest_mod_hooks_library.c", "-o", binary],
                       check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=10)

    def test_receipt_is_scoped_to_phase_cpu_tls_and_thread(self):
        with tempfile.TemporaryDirectory() as directory:
            write_fixture_headers(Path(directory))
            self.run_fixture(directory, "-I", directory)

    def test_owned_wait_receipt_preserves_original_guest_state(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the completed original-body generation")
        sys.path.extend((str(HIGHHEELS), str(ROOT / "scripts")))
        import stage_scene_app
        from stage_effect_magnitude import PRELUDE, function
        from classic_skse_guest import validate_generation

        generation = Path(generation)
        state = json.loads((generation / "generation.json").read_bytes())
        validate_generation(state)
        self.assertEqual(state.get("mode"), "original-guest-bodies")
        self.assertIs(state.get("fixed_native_replacements"), False)
        self.assertIs(state.get("generation_finished"), True)
        self.assertIs(state.get("inputs_unchanged"), True)
        unit_path = generation / "gen/exe_108.c"
        unit = unit_path.read_bytes()
        original = function(unit, 0x6A08F0).replace(b"void f_6A08F0(", b"void fixture_original_wait(", 1)
        observed = function(stage_scene_app.apply(unit_path.name, unit, unit), 0x6A08F0)
        frame_path = generation / "gen/exe_106.c"
        frame = frame_path.read_bytes()
        wrapper = function(stage_scene_app.apply(frame_path.name, frame, frame), 0x694770)
        # Test the prefix through the camera call. The stock updates after it are independent.
        tail = b"    eax = (uint32_t)(TL32((uint32_t)(0x1B4ADE0u)));\n"
        self.assertEqual(wrapper.count(tail), 1)
        prefix = wrapper[:wrapper.index(tail)].replace(b"void f_694770(", b"void fixture_frame_prefix(", 1)
        prefix += b"    SPILL(); return;\n}\n"
        private = ROOT / "private"
        self.assertEqual(private.resolve(), private)
        with tempfile.TemporaryDirectory(prefix="classic-highheel-wait-", dir=private) as directory:
            staged = Path(directory) / "lifted.c"
            staged.write_bytes(stage_scene_app.HEADER + PRELUDE + original + observed + prefix)
            self.run_fixture(directory, "-DSKYRIM_TEST_OWNED_FRAME_WAIT", "-isystem", generation / "gen",
                             "-isystem", generation / "source/runtime", "-I", NATIVE,
                             staged, NATIVE / "guest_memory.c")
        self.assertEqual(unit_path.read_bytes(), unit)
        self.assertEqual(frame_path.read_bytes(), frame)
