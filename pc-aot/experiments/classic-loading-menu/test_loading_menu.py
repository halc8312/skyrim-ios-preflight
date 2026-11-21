# SPDX-License-Identifier: GPL-3.0-only
"""Exercise exact menu dispatch call anchors from the owner-supplied LE lift."""
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation
from stage_loading_menu import DECLARATION, UNIT, dispatch_lines, observer_call, stage_loading_menu

RUNTIME_HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}


class LoadingMenuTests(unittest.TestCase):
    def test_only_reviewed_unit(self):
        with self.assertRaisesRegex(ValueError, "differs from the reviewed"):
            stage_loading_menu(b"authored input")

    def test_reviewed_open_and_close_dispatch(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation or not compiler:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a C compiler")
        generation = Path(generation)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        self.assertEqual(state["mode"], "original-guest-bodies")
        self.assertIs(state["fixed_native_replacements"], False)
        self.assertIs(state["generation_finished"], True)
        self.assertIs(state["inputs_unchanged"], True)
        runtime = generation / "source/runtime"
        for name, digest in RUNTIME_HEADERS.items():
            self.assertEqual(sha256((runtime / name).read_bytes()).hexdigest(), digest, name)
        source = generation / "gen" / UNIT
        original = source.read_bytes()
        staged = stage_loading_menu(original)
        self.assertTrue(staged.startswith(DECLARATION))
        unstaged = staged.removeprefix(DECLARATION)
        for site in dispatch_lines(original):
            self.assertEqual(unstaged.count(observer_call(site)), 1)
            unstaged = unstaged.replace(observer_call(site), b"", 1)
        self.assertEqual(unstaged, original)

        app = ROOT / "ios/runtime-port"
        includes = ["-isystem", str(generation / "gen"), "-isystem", str(runtime),
                    "-I", str(ROOT / "native"), "-I", str(app), "-I", str(HERE)]
        with tempfile.TemporaryDirectory(prefix="classic-loading-menu-") as temporary:
            directory = Path(temporary)
            full_unit = directory / UNIT
            full_unit.write_bytes(staged)
            subprocess.run([compiler, "-std=gnu11", "-w", "-fsyntax-only", *includes, full_unit],
                           check=True, timeout=60)
            fixture = directory / "menu_dispatch_fixture.c"
            wrappers = [DECLARATION, b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n']
            for variant, lines in (("baseline", dispatch_lines(original)), ("observed", dispatch_lines(staged, True))):
                for direction, line in zip(("close", "open"), lines.values(), strict=True):
                    wrappers.extend((f"void {variant}_{direction}(CPU *c) {{\n".encode("ascii"),
                                     b"    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f;\n"
                                     b"    RELOAD();\n", line, b"    SPILL();\n}\n"))
            fixture.write_bytes(b"".join(wrappers))
            flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
                if sys.platform.startswith("linux"):
                    flags.extend(["-fno-pie", "-no-pie"])
            binary = directory / "loading-menu"
            subprocess.run([compiler, *flags, *includes, fixture, HERE / "test_loading_menu.c",
                            HERE / "loading_menu.c", app / "mod_loading_menu_host.c",
                            ROOT / "native/guest_memory.c", "-o", binary],
                           check=True, timeout=60)
            run = subprocess.run([binary], check=True, timeout=30, text=True, capture_output=True)
            self.assertEqual(run.stderr.splitlines(), ["IOS_LOADING_MENU opening=1 ts_ns=1234567",
                                                       "IOS_LOADING_MENU opening=0 ts_ns=1234567"])
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
