"""Run the authored host scenarios for the Classic high-heels height model."""

from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import stage_class_lookup
import stage_effect_call
import stage_effect_event
import stage_effect_magnitude
import stage_item_delivery
import stage_pex_proof
import stage_scene_app
import stage_scene_refresh
import stage_scene_witness
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
                [cc, *flags, HERE / "highheel_height.c", HERE / "test_highheel_height.c", "-lm", "-o", binary],
                check=True, timeout=60,
            )
            result = subprocess.check_output([binary], text=True, timeout=10)
            self.assertEqual(result.strip(), "9 Classic HighHeels host scenarios passed")

    def test_unreviewed_lift_is_refused(self):
        for stage in (stage_task_queue, stage_item_delivery, stage_effect_magnitude, stage_class_lookup,
                      stage_effect_call, stage_effect_event, stage_scene_refresh, stage_pex_proof,
                      stage_scene_witness, stage_scene_app):
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
            extra_sources=(HERE / "highheel_lifetimes.c",),
        )

    def test_reviewed_pex_reader_and_linked_class_witness(self):
        supplied = os.environ.get("SKYRIM_TEST_OWNED_HDT_EFFECT_PEX")
        if not supplied:
            self.skipTest("Set SKYRIM_TEST_OWNED_HDT_EFFECT_PEX to the unchanged HDT HighHeels beta 0.5 LE PEX")
        pex = Path(supplied)
        with pex.open("rb") as stream:
            contents = stream.read(5876)
        self.assertEqual(len(contents), 5875)
        self.assertEqual(sha256(contents).hexdigest(), EFFECT_PEX_SHA256)
        self.run_reviewed_fixture(
            stage_pex_proof, "test_pex_proof_lift.c",
            "Classic HighHeels PEX witness joins fresh Store reads, successful parsing and linked class state",
            runs=(((pex,), None),),
        )
        self.assertEqual(pex.read_bytes(), contents)

    def test_reviewed_native_invoke_lends_nearest_typed_script_self(self):
        self.run_reviewed_fixture(
            stage_effect_call, "test_effect_call_lift.c",
            "Classic ordinary native invocation lends typed script self and update arguments",
            extra_sources=(HERE / "highheel_native_frame.c",),
        )

    def test_reviewed_class_lookup_returns_function_slots(self):
        self.run_reviewed_fixture(
            stage_class_lookup, "test_class_lookup_lift.c",
            "Classic class lookups return slots from separate global and member tables",
        )

    def test_reviewed_effect_event_retains_target_through_native_dispatch(self):
        self.run_reviewed_fixture(
            stage_effect_event, "test_effect_event_lift.c",
            "Classic effect events lend the target and preserve effect identity and magnitude through native dispatch",
            extra_sources=(HERE / "highheel_effect.c",),
        )

    def test_owned_esm_pins_script_effect_identity(self):
        supplied = os.environ.get("SKYRIM_TEST_OWNED_HDT_EFFECT_ESM")
        if not supplied:
            self.skipTest("Set SKYRIM_TEST_OWNED_HDT_EFFECT_ESM to the unchanged HDT HighHeels beta 0.5 LE ESM")
        esm = Path(supplied)
        with esm.open("rb") as stream:
            contents = stream.read(4096)
        self.assertEqual(len(contents), 1934)
        self.assertEqual(sha256(contents).hexdigest(),
                         "7c7b6bb6ba4585e9f55a5b0459bb275a2f6d7f37542f3f12239f5b1c32c52c32")
        # File-local high byte 1 selects the file itself, after its sole Skyrim master.
        self.assertEqual(contents[56:73], b"MAST\x0b\0Skyrim.esm\0")
        self.assertEqual(contents.count(b"MAST"), 1)
        self.assertEqual(struct.unpack_from("<4sIII", contents, 200), (b"MGEF", 531, 0, 0x01000800))
        self.assertEqual(contents[230:248], b"hdtHighHeelEffect\0")
        self.assertEqual(struct.unpack_from("<HHHH", contents, 254), (5, 2, 1, 17))
        self.assertEqual(contents[262:279], b"hdtHighHeelEffect")
        self.assertEqual(struct.unpack_from("<I", contents, 572)[0], 0x08200210)
        self.assertEqual(struct.unpack_from("<I", contents, 572 + 8)[0], 0)  # No legacy script form.
        self.assertEqual(struct.unpack_from("<I", contents, 572 + 64)[0], 1)  # Script archetype.
        self.assertEqual(esm.read_bytes(), contents)

    def test_reviewed_scene_refresh_recomposes_owned_adjustment_and_undo(self):
        self.run_reviewed_fixture(
            stage_scene_refresh, "test_scene_refresh_lift.c",
            "Classic scene recomposes owned local height and undo in the authored NiNode hierarchy",
            extra_sources=(HERE / "highheel_height.c",),
        )

    def test_reviewed_scene_composes_a_temporary_local_height(self):
        self.run_reviewed_fixture(
            stage_scene_refresh, "test_scene_refresh_lift.c",
            "Classic scene recomposes owned local height and undo in the authored NiNode hierarchy",
            extra_sources=(HERE / "highheel_height.c",), extra_flags=("-DSKYRIM_TEST_HEEL_LOCAL_ARGUMENT",),
            generate=stage_scene_refresh.private_composition_fixture,
        )

    def test_reviewed_third_person_scene_witness_follows_original_edges(self):
        self.run_reviewed_fixture(
            stage_scene_witness, "test_scene_witness_lift.c",
            "Classic third-person traversal witnesses five actual named nodes without changing the scene",
        )

    def test_reviewed_app_scene_diagnostics_are_passive_and_one_shot(self):
        port = ROOT / "ios/runtime-port"
        abi = ROOT / "ios/private-abi"
        ordinary = "IOS_CLASSIC_HIGHHEEL_SCENE schema=1 root={} names={} duplicate={} limited={}\n"
        pose = "IOS_CLASSIC_HIGHHEEL_POSE schema=1 captured={} valid={}\n"
        frame = "IOS_CLASSIC_HIGHHEEL_FRAME schema=1 phase={} composed={} same_z={}\n"
        actor = "IOS_CLASSIC_HIGHHEEL_ACTOR schema=1 current={} swimming={} smooth_state={}\n"
        cases = ((1, 31, 0, 0, 1), (2, 31, 0, 0, 1), (1, 15, 0, 0, 0), (2, 15, 0, 0, 0),
                 (1, 15, 0, 1, 0), (1, 15, 1, 0, 0), (1, 31, 0, 1, 0), None, None,
                 (1, 31, 0, 0, 0))
        receipts = ((2, 1), (2, 1), (1, 1), (0, 1), (2, 0), (2, 0), (2, 0), None, None, (2, 0))
        actors = ((1, 0, 1), (1, 1, 0), (1, 0, 1), (1, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0),
                  None, None, (1, 0, 1))
        self.run_reviewed_fixture(
            stage_scene_app, "test_scene_witness_lift.c",
            "Classic third-person traversal witnesses five actual named nodes without changing the scene",
            extra_sources=(port / "mod_highheel_scene_host.c", abi / "pc_guest_mod_hooks_library.c",
                           HERE / "highheel_height.c"),
            extra_flags=("-I", HERE, "-I", port, "-I", abi, "-I", ROOT / "experiments/fuz-voice",
                         "-DSKYRIM_TEST_SCENE_RUNTIME=1", "-DPC_GUEST_MOD_HOOKS_COMPILED=65536u"),
            runs=tuple(((str(i),), ordinary.format(*fields[:4]) + pose.format(fields[1], fields[4]) +
                        frame.format(*receipt, receipt[1]) + actor.format(*state) if fields else "")
                       for i, (fields, receipt, state) in enumerate(zip(cases, receipts, actors))),
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
        names = instruction_at(0x1002B6F0, 0x1002B761)
        for site, name, pointer in ((0x1002B6F0, 0x10034318, 0x1003B168), (0x1002B710, 0x1003432C, 0x1003B178),
                                    (0x1002B730, 0x10034340, 0x1003B198), (0x1002B750, 0x10034354, 0x1003B16C)):
            self.assertEqual(names[site], f"push {name:#x}")
            self.assertEqual(names[site + 10], f"mov ecx,{pointer:#x}")
        pose = instruction_at(0x1000A3D9, 0x1000A568)
        for address, instruction in {
            0x1000A425: "subps xmm4,xmm3", 0x1000A43C: "haddps xmm4,xmm4",
            0x1000A444: "sqrtps xmm6,xmm4", 0x1000A468: "subps xmm4,xmm3",
            0x1000A492: "divss xmm1,xmm6", 0x1000A49E: "sqrtps xmm0,xmm4",
            0x1000A4A1: "divss xmm2,xmm0", 0x1000A4B2: "comiss xmm3,xmm5",
            0x1000A4C4: "addss xmm0,xmm6", 0x1000A4D6: "divss xmm0,dword ptr ds:0x10034b40",
            0x1000A4E1: "mulss xmm0,dword ptr [edx+0x50]", 0x1000A4EC: "divss xmm0,dword ptr [edx+0x84]",
            0x1000A4F4: "and eax,0x400", 0x1000A4FC: "mulss xmm0,dword ptr [edi+0x8]",
            0x1000A501: "mulss xmm0,xmm1", 0x1000A505: "addss xmm0,dword ptr ds:0x100396ec",
            0x1000A527: "and ecx,0x20", 0x1000A532: "subss xmm1,xmm2", 0x1000A543: "addss xmm2,xmm4",
        }.items():
            self.assertEqual(pose[address], instruction)
        data = subprocess.check_output([objdump, "-s", "--start-address=0x10034b40",
                                        "--stop-address=0x10034b44", library], text=True, timeout=10)
        self.assertRegex(data, r"10034b40\s+fc985f42\b")
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


    def run_reviewed_fixture(self, stage, test, expected_output, *, extra_sources=(), extra_flags=(),
                             runs=(((), None),), generate=None):
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
            staged.write_bytes((generate or stage.private_fixture)(*originals))
            binary = scratch / "classic_fixture"
            subprocess.run(
                [cc, *flags, *extra_flags, "-isystem", generation / "gen", "-isystem", runtime, "-I", native,
                 staged, HERE / test, *extra_sources, native / "guest_memory.c", "-lm", "-o", binary],
                check=True, timeout=60,
            )
            for arguments, expected_stderr in runs:
                result = subprocess.run([binary, *arguments], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected_output)
                if expected_stderr is not None:
                    self.assertEqual(result.stderr, expected_stderr)
        for source, original in zip(sources, originals):
            self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
