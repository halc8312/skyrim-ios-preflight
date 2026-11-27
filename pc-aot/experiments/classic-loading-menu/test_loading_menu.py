# SPDX-License-Identifier: GPL-3.0-only
"""Exercise exact menu dispatch call anchors from the owner-supplied LE lift."""
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation
from stage_loading_menu import DECLARATION, UNIT, dispatch_lines, observer_call, stage_loading_menu
import stage_menu_focus

RUNTIME_HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}


class LoadingMenuTests(unittest.TestCase):
    def test_host_touch_state_with_nested_menus(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler required")
        app = ROOT / "ios/runtime-port"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                flags.extend(["-fno-pie", "-no-pie"])
        with tempfile.TemporaryDirectory(prefix="classic-touch-menus-") as tmp:
            binary = Path(tmp) / "touch-menus"
            subprocess.run([compiler, *flags, "-I", HERE, "-I", app, HERE / "test_menu_touch.c",
                            HERE / "loading_menu.c", app / "mod_loading_menu_host.c", "-o", binary], check=True)
            subprocess.run([binary], check=True)

    def test_only_reviewed_unit(self):
        with self.assertRaisesRegex(ValueError, "differs from the reviewed"):
            stage_loading_menu(b"authored input")

    def owned_runtime(self):
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
        return compiler, generation, runtime

    def test_pointer_coordinates_reach_the_reviewed_game_message_queue(self):
        compiler, generation, runtime = self.owned_runtime()
        functions = {
            (263, b"A62020"): "5ac768bf4d445f1d4a08b632f8ae8a5609f38ee467421f689ac49c991a96ddfc",
            (263, b"A64CA0"): "f8ebf4158505d7b2e644588434568f278c4a5ab997e0cc26b322135888224cd1",
            (262, b"A5C210"): "b253902507999a75811751fb5221496ac20b9c787e876711e334ea87802b519d",
            (262, b"A5C900"): "5359f367ba8f86b788feef6b49d53b4446cc05a8db2cbf7172e652ca1575efe0",
            (262, b"A5D370"): "dde214035018c239ee6b4156911f74f04bfb4d4eb41900a08d646d30530cef3c",
            (179, b"859A70"): "a0421b2da19e3d72b28748021272883f96ce46b020194a036543feaed794d9da",
            (182, b"86BA80"): "e4c2c50ca11eaa431ec2156f7273658082eadabfcd4630fc98e3521b8e69f3a5",
            (183, b"875E40"): "d5b82430d559482d8a300971178add2e4a28a2c6e7283133b1bd32d65d69ae88",
            (263, b"A64940"): "ea74ffd820d9fd7a879282ad513690a0380182550e650f104d2581961409033d",
            (264, b"A68F20"): "df4c735b0a1934b442397a41a433429bfbcb0fbe6a3cc3e77b11f82e9ead4308",
        }
        bodies = []
        for (unit_number, address), digest in functions.items():
            unit = (generation / "gen" / f"exe_{unit_number}.c").read_bytes()
            entry = b"void f_" + address + b"(CPU *c) {\n"
            self.assertEqual(unit.count(entry), 1)
            start = unit.index(entry)
            code = unit[start:unit.index(b"\n}\n", start) + 3]
            self.assertEqual(sha256(code).hexdigest(), digest)
            if address == b"A5D370":
                # Use the dispatcher's generic-message route with its real menu-stack lookup.
                # The fixture supplies the previously matched named menu and an authored stack.
                start = code.index(b"\nL_A5D75E:\n")
                end = code.index(b"\nL_A5D7EE:\n", start)
                code = (b"void fixture_route(CPU *c) {\n"
                        b"uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f; RELOAD();\n" +
                        code[start:end] + b"\nL_A5DB14: L_A5DB18: SPILL(); return;\n}\n")
            bodies.append(code)
        calls = set(re.findall(rb"\bf_([0-9A-F]+)\(c\)", b"\n".join(bodies)))
        implemented = {address for _, address in functions} | {
            b"547A00", b"431B00", b"401710", b"A66EC0", b"A66400", b"6EBC10", b"A698A0",
        }
        for address in sorted(calls - implemented):
            bodies.append(b"void f_" + address + b"(CPU *c) {(void)c; abort();}\n")
        advance = stage_menu_focus.staged_advance((generation / "gen" / stage_menu_focus.UNIT).read_bytes())
        original_advance = advance.replace(stage_menu_focus.CALL, b"", 1)
        self.assertEqual(sha256(original_advance).hexdigest(), stage_menu_focus.BODY_SHA)
        bodies += [stage_menu_focus.DECL, advance,
                   original_advance.replace(b"f_A64980(", b"baseline_advance(", 1)]
        # Use the real Classic bridge's state-preserving thiscall entry for immediate delivery.
        for name, entry, digest in (
            ("dispatch.c", b"uint32_t rt_invoke(", "db5da85c65ad7e6339b7ffe09ff84c6365f0c04fb2c7d99d67c7e90bd591f7ff"),
            ("skse_compat.c", b"uint32_t skse_member(", "1a9a66750791979b0c1a76a34d51b834ff86cb217527ab40bb1e4b00ad949050")
        ):
            source = (runtime / name).read_bytes()
            start = source.index(entry)
            body = source[start:source.index(b"\n}\n", start) + 3]
            self.assertEqual(sha256(body).hexdigest(), digest)
            bodies.append(body)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        candidate = Path(state["candidate"])
        if not candidate.is_file():
            self.skipTest("The original executable named in the owned generation is required for its UI tables")
        pe = candidate.read_bytes()
        self.assertEqual(sha256(pe).hexdigest(), state["candidate_sha256"])
        pe_offset, = struct.unpack_from("<I", pe, 60)
        section_count, = struct.unpack_from("<H", pe, pe_offset + 6)
        optional_size, = struct.unpack_from("<H", pe, pe_offset + 20)
        image_base, = struct.unpack_from("<I", pe, pe_offset + 52)
        sections = [struct.unpack_from("<IIII", pe, pe_offset + 24 + optional_size + i * 40 + 8)
                    for i in range(section_count)]
        # Owned vtable entries, menu switch tables and the cursor-name literal.
        ranges = [(0x10E4BD4, 20), (0x10E5B90, 20), (0x10E6594, 20),
                  (0x86C01C, 41), (0x876544, 36), (0x110DF94, 12)]
        tables = bytearray()
        for address, length in ranges:
            rva = address - image_base
            offset, = [raw + rva - va for _, va, size, raw in sections if va <= rva <= va + size - length]
            tables += struct.pack("<II", address, length) + pe[offset:offset + length]
        flags = ["-std=gnu11", "-O2", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                flags.extend(["-fno-pie", "-no-pie"])
        app = ROOT / "ios/runtime-port"
        with tempfile.TemporaryDirectory(prefix="classic-menu-pointer-") as temporary:
            fixture = Path(temporary) / "pointer_guest.c"
            fixture.write_bytes(b'#include <stdlib.h>\n#include "funcs_exe.h"\n#include "lift_prelude.h"\n' +
                                b"\n".join(bodies))
            tables_file = fixture.with_suffix(".tables")
            tables_file.write_bytes(tables)
            exe = fixture.with_suffix("")
            subprocess.run([compiler, *flags, "-isystem", generation / "gen", "-isystem", runtime,
                            "-I", ROOT / "native", "-I", app, fixture, HERE / "test_menu_pointer.c",
                            HERE / "menu_focus.c", app / "mobile_core.c", ROOT / "native/guest_memory.c",
                            "-lm", "-o", exe],
                           check=True, timeout=60)
            subprocess.run([exe, tables_file], check=True, timeout=30)

    def test_reviewed_open_and_close_dispatch(self):
        compiler, generation, runtime = self.owned_runtime()
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
                                                       "IOS_LOADING_MENU opening=0 ts_ns=1234567",
                                                       "IOS_SAFETY_LOAD accepted=2",
                                                       "IOS_CHARGEN_MENU opening=1 ts_ns=1234567",
                                                       "IOS_CHARGEN_MENU opening=0 ts_ns=1234567"])
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
