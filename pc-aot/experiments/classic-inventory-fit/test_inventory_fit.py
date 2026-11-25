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

from stage_inventory_fit import CALLER_UNITS, DECLARATION, FIT, MENU_CALLERS, UNIT, mode_call, stage_inventory_fit

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from mod_hooks_guest import validate_generation


class InventoryFitTests(unittest.TestCase):
    def test_refuses_other_source(self):
        with self.assertRaisesRegex(ValueError, "differ from the reviewed"):
            stage_inventory_fit(b"authored loader", {})

    def test_owned_supported_menu_mode_calls(self):
        lift = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not lift or not compiler:
            self.skipTest("A fixed-version Classic lift and a native C compiler are required")
        lift = Path(lift).resolve()
        state = json.loads((lift / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state, lift)
        source = lift / "gen" / UNIT
        original = source.read_bytes()
        callers = {name: (lift / "gen" / name).read_bytes() for name in CALLER_UNITS}
        staged = stage_inventory_fit(original, callers)
        self.assertEqual(staged.count(FIT), 1)
        self.assertEqual(staged.removeprefix(DECLARATION).replace(FIT, b"", 1), original)
        bad_magic = callers["exe_183.c"].replace(
            b"{ uint32_t v_ = 0x10E6584u;", b"{ uint32_t v_ = 0x10E6588u;")
        self.assertTrue(bad_magic != callers["exe_183.c"])
        with self.assertRaises(ValueError):
            stage_inventory_fit(original, {**callers, "exe_183.c": bad_magic})

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
            expected = ",".join(f"0x{row[3]}u" for row in MENU_CALLERS)
            subprocess.run([compiler, *flags, *includes, f"-DREVIEWED_MENU_POINTERS={expected}",
                            fragment, HERE / "test_inventory_fit.c",
                            ROOT / "native/guest_memory.c", "-o", binary], check=True, timeout=90)
            subprocess.run([binary], check=True, timeout=30)
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
