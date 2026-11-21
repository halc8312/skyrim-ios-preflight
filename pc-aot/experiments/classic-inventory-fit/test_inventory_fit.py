# SPDX-License-Identifier: GPL-3.0-only
"""Test the mode argument reaching the pinned Classic inventory MovieView."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from stage_inventory_fit import DECLARATION, FIT, INVENTORY_UNIT, UNIT, mode_call, stage_inventory_fit

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from mod_hooks_guest import validate_generation


class InventoryFitTests(unittest.TestCase):
    def test_refuses_other_source(self):
        with self.assertRaisesRegex(ValueError, "differ from the reviewed"):
            stage_inventory_fit(b"authored loader", b"authored inventory")

    def test_owned_inventory_mode_call(self):
        lift = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not lift or not compiler:
            self.skipTest("A fixed-version Classic lift and a native C compiler are required")
        lift = Path(lift).resolve()
        state = json.loads((lift / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state, lift)
        source = lift / "gen" / UNIT
        original = source.read_bytes()
        staged = stage_inventory_fit(original, (lift / "gen" / INVENTORY_UNIT).read_bytes())
        self.assertEqual(staged.count(FIT), 1)
        self.assertEqual(staged.removeprefix(DECLARATION).replace(FIT, b"", 1), original)

        def fixture(name, unit):
            return (f"void {name}_fit(CPU *c) {{\n".encode()
                    + b"    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f;\n"
                    + b"    RELOAD();\n    goto L_A60D2F;\n" + mode_call(unit) + b"    SPILL();\n}\n")

        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        includes = ["-isystem", str(lift / "source/runtime"), "-isystem", str(lift / "gen"),
                    "-I", str(ROOT / "native")]
        with tempfile.TemporaryDirectory(prefix="classic-inventory-fit-") as directory:
            directory = Path(directory)
            composed = directory / UNIT
            composed.write_bytes(staged)
            subprocess.run([compiler, "-std=gnu11", "-w", "-fsyntax-only", *includes, composed],
                           check=True, timeout=90)
            fragment = directory / "inventory_fragment.c"
            fragment.write_bytes(DECLARATION + b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
                                 + fixture("baseline", original) + fixture("staged", staged))
            binary = directory / "inventory-test"
            subprocess.run([compiler, *flags, *includes, fragment, HERE / "test_inventory_fit.c",
                            ROOT / "native/guest_memory.c", "-o", binary], check=True, timeout=90)
            subprocess.run([binary], check=True, timeout=30)
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
