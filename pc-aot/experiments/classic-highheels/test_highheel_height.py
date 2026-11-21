"""Run the authored host scenarios for the Classic high-heels height model."""

from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import stage_item_delivery
import stage_task_queue


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation


RUNTIME_HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}


class ClassicHighHeelHeightTest(unittest.TestCase):
    def test_height_transitions(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler unavailable")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "highheel_height"
            subprocess.run(
                [cc, *flags, HERE / "highheel_height.c", HERE / "test_highheel_height.c", "-o", binary],
                check=True, timeout=60,
            )
            result = subprocess.check_output([binary], text=True, timeout=10)
            self.assertEqual(result.strip(), "6 Classic HighHeels host scenarios passed")

    def test_unreviewed_lift_is_refused(self):
        for stage in (stage_task_queue, stage_item_delivery):
            with self.subTest(unit=stage.UNIT):
                with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
                    stage.private_fixture(b"authored stand-in")

    def test_reviewed_task_queue_owner_and_original_state(self):
        self.run_reviewed_fixture(
            stage_task_queue, "test_task_queue_lift.c",
            "Classic task queue observation preserved the original guest state and owner",
        )

    def test_primary_item_delivery_preserves_borrowed_actor_and_state(self):
        self.run_reviewed_fixture(
            stage_item_delivery, "test_item_delivery_lift.c",
            "Classic primary item equip and unequip delivery preserves the borrowed reference and guest state",
        )

    def run_reviewed_fixture(self, stage, test, expected_output):
        supplied_generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not (supplied_generation and cc and os.name == "posix"):
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a POSIX host C compiler")
        generation = Path(supplied_generation)
        state = json.loads((generation / "generation.json").read_bytes())
        validate_generation(state)
        if (state.get("mode") != "original-guest-bodies" or state.get("fixed_native_replacements") is not False
                or state.get("generation_finished") is not True or state.get("inputs_unchanged") is not True):
            raise ValueError("Requires a completed, unchanged original-body generation")
        runtime = generation / "source/runtime"
        for name, digest in RUNTIME_HEADERS.items():
            self.assertEqual(sha256((runtime / name).read_bytes()).hexdigest(), digest, name)
        source = generation / "gen" / stage.UNIT
        original = source.read_bytes()
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-g", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        native = ROOT / "native"
        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Lifted task-queue fixtures must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="classic-highheel-queue-", dir=private) as directory:
            scratch = Path(directory)
            staged = scratch / "lifted.c"
            staged.write_bytes(stage.private_fixture(original))
            binary = scratch / "classic_fixture"
            subprocess.run(
                [cc, *flags, "-isystem", generation / "gen", "-isystem", runtime, "-I", native,
                 staged, HERE / test, native / "guest_memory.c", "-o", binary],
                check=True, timeout=60,
            )
            result = subprocess.check_output([binary], text=True, timeout=10)
            self.assertEqual(result.strip(), expected_output)
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
