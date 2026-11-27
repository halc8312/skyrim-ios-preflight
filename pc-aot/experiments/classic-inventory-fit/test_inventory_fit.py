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

from stage_inventory_fit import (CALLER_UNITS, DECLARATION, FIT, LOAD_ENTRY, MENU_CALLERS, START, UNIT,
                                 function_body, mode_call, reviewed_caller_slice, stage_inventory_fit)

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

        def fixture(name, body):
            return (f"void {name}_fit(CPU *c) {{\n".encode()
                    + b"    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f;\n"
                    + b"    RELOAD();\n    goto L_A60D2F;\n" + body + b"    SPILL();\n}\n")

        def viewport_path(unit):
            body = function_body(unit, LOAD_ENTRY)
            anchor = b"0xA60ED5u; SPILL(); f_844B40(c); RELOAD();"
            self.assertEqual(body.count(anchor), 1)
            end = body.index(b"\n", body.index(anchor)) + 1
            return body[:end]

        baseline_path, staged_path = viewport_path(original), viewport_path(staged)
        self.assertEqual(staged_path.replace(FIT, b"", 1), baseline_path)
        # Use the actual loader prologue and argument offsets. Movie acquisition is
        # stubbed by the C fixture. The trace stops after the viewport setter returns.
        loader = LOAD_ENTRY + staged_path.replace(b"L_A60C80:\n", b"", 1)
        loader += b"    SPILL();\n    return;\nL_A60F53:\n    assert(0);\n}\n"
        cases = []
        for index, (unit, entry, returned, name, digest) in enumerate(MENU_CALLERS):
            cases.append(f"    case {index}u:\n".encode() + reviewed_caller_slice(callers[unit], entry, returned, name, digest)
                         + b"        break;\n")
        callers_fixture = (b"\nvoid run_menu_loader(CPU *c, unsigned index) {\n"
                           b"    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f;\n"
                           b"    RELOAD();\n    switch (index) {\n" + b"".join(cases)
                           + b"    default: assert(0);\n    }\n    SPILL();\n}\n")
        viewport_unit = (lift / "gen/exe_175.c").read_bytes()
        viewport_entry = b"\nvoid f_844B40(CPU *c) {\n"
        viewport = viewport_entry + function_body(viewport_unit, viewport_entry) + b"\n}\n"

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
            fragment.write_bytes(DECLARATION + b'#include <assert.h>\n#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
                                 + viewport + fixture("baseline", mode_call(original))
                                 + fixture("staged", mode_call(staged))
                                 + fixture("baseline_viewport", baseline_path[baseline_path.index(START):])
                                 + fixture("staged_viewport", staged_path[staged_path.index(START):])
                                 + loader + callers_fixture)
            binary = directory / "inventory-test"
            expected = ",".join(f"0x{row[3]}u" for row in MENU_CALLERS)
            subprocess.run([compiler, *flags, *includes, f"-DREVIEWED_MENU_POINTERS={expected}",
                            fragment, HERE / "test_inventory_fit.c",
                            ROOT / "native/guest_memory.c", "-lm", "-o", binary], check=True, timeout=90)
            subprocess.run([binary], check=True, timeout=30)
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
