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

import stage_effect_call
import stage_effect_event
import stage_effect_magnitude
import stage_item_delivery
import stage_scene_refresh
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
EFFECT_PEX_SHA256 = "01e4816a1ea6c5bcb6a4c3bf62c9cdca6574600ddc441b95341b4942df3bdf03"


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
        for stage in (stage_task_queue, stage_item_delivery, stage_effect_magnitude,
                      stage_effect_call, stage_effect_event, stage_scene_refresh):
            with self.subTest(unit=stage.UNIT):
                with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
                    unit_count = 1 + len(getattr(stage, "DEPENDENCIES", ()))
                    stage.private_fixture(*([b"authored stand-in"] * unit_count))

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

    def test_owned_effect_pex_declarations_and_update_calls(self):
        supplied_pex = os.environ.get("SKYRIM_TEST_OWNED_HDT_EFFECT_PEX")
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not (supplied_pex and cc and os.name == "posix"):
            self.skipTest("Set SKYRIM_TEST_OWNED_HDT_EFFECT_PEX and use a POSIX host C compiler")
        pex = Path(supplied_pex)
        with pex.open("rb") as stream:
            contents = stream.read(8192)
        self.assertEqual(sha256(contents).hexdigest(), EFFECT_PEX_SHA256,
                         "Use the unchanged hdtHighHeelEffect.pex from HDT HighHeels beta 0.5 LE")
        parser = ROOT / "experiments/skse-plugin-vm"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-g", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        with tempfile.TemporaryDirectory(prefix="classic-highheel-pex-") as directory:
            binary = Path(directory) / "effect_pex"
            subprocess.run(
                [cc, *flags, "-I", parser, parser / "classic_pex.c", HERE / "test_owned_effect_pex.c",
                 "-o", binary],
                check=True, timeout=60,
            )
            result = subprocess.check_output([binary], input=contents, timeout=10)
        self.assertIn(b"matches the typed declarations and update call arities", result)
        self.assertEqual(pex.read_bytes(), contents)


    def test_reviewed_active_effect_lookup_captures_magnitude_at_match(self):
        self.run_reviewed_fixture(
            stage_effect_magnitude, "test_effect_magnitude_lift.c",
            "Classic typed effect lookup captures magnitude only in the matching visitor",
        )

    def test_reviewed_native_invoke_lends_nearest_typed_script_self(self):
        self.run_reviewed_fixture(
            stage_effect_call, "test_effect_call_lift.c",
            "Classic ordinary native invocation lends typed script self and update arguments",
        )

    def test_reviewed_effect_event_retains_target_through_native_dispatch(self):
        self.run_reviewed_fixture(
            stage_effect_event, "test_effect_event_lift.c",
            "Classic effect events lend the target actor through synchronous native dispatch",
        )

    def test_reviewed_scene_refresh_recomposes_owned_adjustment_and_undo(self):
        self.run_reviewed_fixture(
            stage_scene_refresh, "test_scene_refresh_lift.c",
            "Classic scene recomposes owned local height and undo in the authored NiNode hierarchy",
            extra_sources=(HERE / "highheel_height.c",),
        )

    def test_owned_effect_native_argument_boundaries(self):
        supplied = os.environ.get("SKYRIM_TEST_OWNED_HDT_EFFECT_DLL")
        objdump = shutil.which("objdump")
        if not (supplied and objdump and sys.platform.startswith("linux")):
            self.skipTest("Set SKYRIM_TEST_OWNED_HDT_EFFECT_DLL and use GNU objdump")
        library = Path(supplied)
        with library.open("rb") as stream:
            contents = stream.read(1 << 20)
        self.assertEqual(sha256(contents).hexdigest(),
                         "f966cf895c9a66bcc53ce381047ef1f67e148087c01435593ec013448e3a25eb")

        def instruction_at(start, end):
            text = subprocess.check_output([
                objdump, "-d", "-M", "intel", f"--start-address={start:#x}",
                f"--stop-address={end:#x}", library,
            ], text=True, timeout=10, env=dict(os.environ, LC_ALL="C"))
            instructions = {}
            for line in text.splitlines():
                fields = line.strip().split("\t")
                if len(fields) == 3 and fields[0].endswith(":"):
                    instructions[int(fields[0][:-1], 16)] = " ".join(fields[-1].lower().split())
            return instructions

        instance = instruction_at(0x1000B0D0, 0x1000B12A)
        getter = instruction_at(0x1000A770, 0x1000A77B)
        self.assertEqual(instance[0x1000B0E6], "mov edx,0x8b")
        self.assertEqual(instance[0x1000B0FC], "mov eax,dword ptr [esi+0x2c]")
        self.assertEqual(getter[0x1000A776], "fld dword ptr [eax+0x50]")
        keep = instruction_at(0x1000B220, 0x1000B2C0)
        self.assertEqual(keep[0x1000B22E], "mov eax,0xc3a620")
        self.assertEqual(keep[0x1000B23A], "push 0x0")
        self.assertEqual(keep[0x1000B23D], "mov eax,0xc3ac40")
        self.assertEqual(keep[0x1000B244], "mov edx,0x3d")
        self.assertEqual(keep[0x1000B253], "push 0x1")
        self.assertEqual(keep[0x1000B259], "mov eax,0xc3ac40")
        self.assertEqual(keep[0x1000B260], "mov ecx,dword ptr [eax]")
        self.assertEqual(keep[0x1000B262], "sub ecx,0x3")
        self.assertEqual(keep[0x1000B267], "dec ecx")
        self.assertEqual(keep[0x1000B268], "je 0x1000b27c")
        self.assertEqual(keep[0x1000B27C], "movss xmm0,dword ptr [eax+0x4]")
        self.assertEqual(keep[0x1000B294], "push dword ptr [ebp+0x18]")
        self.assertEqual(keep[0x1000B297], "push 0x0")
        stop = instruction_at(0x1000B3B0, 0x1000B409)
        self.assertEqual(stop[0x1000B3BD], "mov eax,0xc3a620")
        self.assertEqual(stop[0x1000B3C7], "push 0x0")
        self.assertEqual(stop[0x1000B3CA], "mov eax,0xc3ac40")
        self.assertEqual(stop[0x1000B3D1], "mov edx,0x3d")
        self.assertEqual(stop[0x1000B3DD], "push eax")
        self.assertEqual(stop[0x1000B3E1], "push 0x0")
        find_node = instruction_at(0x1000A27B, 0x1000A2AA)
        self.assertEqual(find_node[0x1000A28F], "mov ecx,dword ptr [ecx+0x20]")
        self.assertEqual(find_node[0x1000A2A0], "mov eax,dword ptr [eax+0x9c]")
        update = instruction_at(0x1000A568, 0x1000A58E)
        self.assertEqual(update[0x1000A56B], "subss xmm0,dword ptr [edi+0xc]")
        self.assertEqual(update[0x1000A577], "addss xmm0,dword ptr [edx+0x4c]")
        self.assertEqual(update[0x1000A57C], "movss dword ptr [edx+0x4c],xmm0")
        self.assertEqual(update[0x1000A588], "call dword ptr [eax+0xb4]")
        undo = instruction_at(0x1000A70F, 0x1000A72A)
        self.assertEqual(undo[0x1000A714], "subss xmm0,dword ptr [esi+0xc]")
        self.assertEqual(undo[0x1000A71F], "movss dword ptr [ecx+0x4c],xmm0")
        self.assertEqual(undo[0x1000A724], "call dword ptr [eax+0xb4]")
        self.assertEqual(library.read_bytes(), contents)


    def run_reviewed_fixture(self, stage, test, expected_output, *, extra_sources=()):
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
        sources = [generation / "gen" / name for name in (stage.UNIT, *getattr(stage, "DEPENDENCIES", ()))]
        originals = [source.read_bytes() for source in sources]
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
            staged.write_bytes(stage.private_fixture(*originals))
            binary = scratch / "classic_fixture"
            subprocess.run(
                [cc, *flags, "-isystem", generation / "gen", "-isystem", runtime, "-I", native,
                 staged, HERE / test, *extra_sources, native / "guest_memory.c", "-o", binary],
                check=True, timeout=60,
            )
            result = subprocess.check_output([binary], text=True, timeout=10)
            self.assertEqual(result.strip(), expected_output)
        for source, original in zip(sources, originals):
            self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
