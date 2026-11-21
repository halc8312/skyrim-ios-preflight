#!/usr/bin/env python3
"""Run the hosted LE missing-voice decision fixture."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import stage_lift
import stage_menu_lift
import stage_resource_lift
import stage_world_lift

HERE = Path(__file__).resolve().parent
GUEST_HELPERS = HERE.parent / "skse-plugin-vm"


class FuzVoiceFallbackTests(unittest.TestCase):
    def compiler_args(self):
        windows = sys.platform == "win32"
        compiler = shutil.which(os.environ.get("CC", "cc"))
        llvm = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin")) / "clang.exe"
        if windows and not compiler and llvm.is_file():
            compiler = str(llvm)
        if not compiler:
            self.skipTest("A C compiler is required.")

        args = [compiler, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2"]
        if windows:
            args.append("-fuse-ld=lld")
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                args.extend(["-fno-pie", "-no-pie"])
        return args

    def run_fixture(self, temporary, test_source, extra_args=(), helpers=()):
        binary = temporary / ("voice.exe" if sys.platform == "win32" else "voice")
        sources = [HERE / "voice_fallback.c", HERE / test_source, *helpers]
        subprocess.run([*self.compiler_args(), *extra_args, *map(str, sources), "-o", str(binary)],
                       check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)

    def run_lift_fixture(self, temporary, use_private_site=False):
        args = [f"-I{GUEST_HELPERS}", f"-I{temporary}"]
        if use_private_site:
            args.append("-DSKYRIM_FUZ_STAGED_SITE")
        self.run_fixture(
            temporary, "test_lift_voice_seam.c", args,
            [HERE / "lift_voice_seam.c", GUEST_HELPERS / "x86_winfile.c"],
        )

    def test_voice_decisions(self):
        with tempfile.TemporaryDirectory(prefix="fuz-voice-") as scratch:
            self.run_fixture(Path(scratch), "test_voice_fallback.c")

    def test_hosted_lift_seam(self):
        with tempfile.TemporaryDirectory(prefix="fuz-lift-host-") as scratch:
            self.run_lift_fixture(Path(scratch))

    def test_stager_rejects_unreviewed_input(self):
        with tempfile.TemporaryDirectory(prefix="fuz-lift-rejected-") as scratch:
            root = Path(scratch)
            (root / "generation.json").write_text("{}", encoding="utf-8")
            destination = root / "staged.c"
            with self.assertRaisesRegex(ValueError, "private generation"):
                stage_lift.stage_lift(root, destination)
            with self.assertRaisesRegex(ValueError, "private generation"):
                stage_resource_lift.prepare_resources(root)
            with self.assertRaisesRegex(ValueError, "private generation"):
                stage_menu_lift.prepare_menu(root)
            with self.assertRaisesRegex(ValueError, "private generation"):
                stage_world_lift.prepare_world(root)
            self.assertFalse(destination.exists())
            (root / "generation.json").write_text(json.dumps(stage_lift.GENERATION), encoding="utf-8")
            generated = root / "gen"
            generated.mkdir()
            first_resource_unit = next(iter(stage_resource_lift.RESOURCE_UNITS))
            (generated / first_resource_unit).write_bytes(b"authored unreviewed resource")
            with self.assertRaisesRegex(ValueError, "reviewed resource lift"):
                stage_resource_lift.prepare_resources(root)
            first_menu_unit = next(iter(stage_menu_lift.MENU_UNITS))
            (generated / first_menu_unit).write_bytes(b"authored unreviewed menu")
            with self.assertRaisesRegex(ValueError, "reviewed Classic menu lift"):
                stage_menu_lift.prepare_menu(root)
            first_world_unit = next(iter(stage_world_lift.WORLD_UNITS))
            (generated / first_world_unit).write_bytes(b"authored unreviewed world")
            with self.assertRaisesRegex(ValueError, "reviewed Classic world lift"):
                stage_world_lift.prepare_world(root)
        with self.assertRaisesRegex(ValueError, "reviewed unit"):
            stage_lift.patch_unit(b"authored unreviewed translation")

    def reviewed_generation(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation.")
        return Path(generation)

    def test_reviewed_private_call_site(self):
        root = self.reviewed_generation()
        source = (root / stage_lift.UNIT).read_bytes()
        _, excerpt = stage_lift.prepare_lift(root)
        with tempfile.TemporaryDirectory(prefix="fuz-lift-reviewed-") as scratch:
            temporary = Path(scratch)
            staged = temporary / "staged.c"
            stage_lift.stage_lift(root, staged)
            self.assertEqual(
                source,
                staged.read_bytes().removeprefix(stage_lift.DECLARATION).replace(stage_lift.INTERCEPT, b"", 1),
            )
            with self.assertRaises(FileExistsError):
                stage_lift.stage_lift(root, staged)
            subprocess.run([
                self.compiler_args()[0], "-std=c11", "-fsyntax-only", f"-I{HERE}",
                f"-I{root / 'gen'}", f"-I{root / 'source' / 'runtime'}", str(staged),
            ], check=True, timeout=60)
            (temporary / "reviewed_voice_site.inc").write_bytes(excerpt)
            self.run_lift_fixture(temporary, use_private_site=True)
        self.assertEqual((root / stage_lift.UNIT).read_bytes(), source)

    def test_reviewed_resource_open_and_disposal(self):
        root = self.reviewed_generation()
        native = HERE.parents[1] / "native"
        with tempfile.TemporaryDirectory(prefix="fuz-resource-lift-") as scratch:
            temporary = Path(scratch)
            staged = temporary / "resources.c"
            staged.write_bytes(stage_resource_lift.prepare_resources(root))
            args = [f"-I{native}", "-isystem", str(root / "gen"), "-isystem", str(root / "source" / "runtime")]
            if sys.platform.startswith("linux"):
                args.append("-D_DEFAULT_SOURCE")
            self.run_fixture(temporary, "test_resource_lift.c", args, [staged, native / "guest_memory.c"])


    def run_queue_fixture(self, root, staged, fixture, test_source, helpers=()):
        native = HERE.parents[1] / "native"
        original_hashes = {name: (root / "gen" / name).read_bytes() for name in staged}
        with tempfile.TemporaryDirectory(prefix="fuz-queue-lift-") as scratch:
            temporary = Path(scratch)
            paths = []
            for name, contents in staged.items():
                path = temporary / name
                path.write_bytes(contents)
                paths.append(path)
            subprocess.run([
                self.compiler_args()[0], "-std=c11", "-fsyntax-only", f"-I{HERE}",
                f"-I{root / 'gen'}", f"-I{root / 'source' / 'runtime'}", *map(str, paths),
            ], check=True, timeout=60)
            excerpt = temporary / "reviewed_queue.c"
            excerpt.write_bytes(fixture)
            args = [f"-I{HERE}", f"-I{native}", f"-I{GUEST_HELPERS}", "-Wno-unused-label",
                    "-isystem", str(root / "gen"), "-isystem", str(root / "source" / "runtime")]
            if sys.platform.startswith("linux"):
                args.append("-D_DEFAULT_SOURCE")
            self.run_fixture(temporary, test_source, args, [
                HERE / "lift_menu_seam.c", HERE / "lift_voice_seam.c", GUEST_HELPERS / "x86_winfile.c",
                native / "guest_memory.c", excerpt, *helpers,
            ])
        for name, contents in original_hashes.items():
            self.assertEqual((root / "gen" / name).read_bytes(), contents)

    def test_reviewed_selected_response_and_owned_queue_text(self):
        root = self.reviewed_generation()
        self.run_queue_fixture(root, *stage_menu_lift.prepare_menu(root), "test_menu_lift.c")

    def test_reviewed_world_copied_title_and_range(self):
        root = self.reviewed_generation()
        self.run_queue_fixture(
            root, *stage_world_lift.prepare_world(root), "test_world_lift.c", (HERE / "lift_world_seam.c",),
        )


if __name__ == "__main__":
    unittest.main()
