"""Exercise the optional cache warm routine in the reviewed LE lift."""
import hashlib
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

MENU_UNIT = "exe_186.c"
MENU_HASH = "bf380ee42a2aaed3e1614e03051c99cc66cfea8eb122fa0eb2b3b6da5c271add"
READINESS_UNIT = "exe_185.c"
READINESS_HASH = "4b53b0aa8cf4974dbfd01654ad1e519d4d053b3b933e414c9d6decad9ce32a10"
SETUP = b"    esp -= 4; M32(esp) = 0x886ACBu; SPILL(); f_880EA0(c); RELOAD();\n"
WARM = b"    esp -= 4; M32(esp) = 0x886AD0u; SPILL(); f_8868C0(c); RELOAD();\n"
FOLLOWUP = b"    esp -= 4; M32(esp) = 0x886ADFu; SPILL(); f_878DB0(c); RELOAD();\n"
WARM_ENTRY = b"\nvoid f_8868C0(CPU *c) {\n"
SKIP_WARM = (b"    if (skyrim_chargen_probe_skip_open_warm()) {\n"
             b"        c->r[4] += 4u;\n"
             b"        return;\n"
             b"    }\n")
PRELUDE = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
HOOK = b"#include <stdbool.h>\nbool skyrim_chargen_probe_skip_open_warm(void);\n"
RUNTIME_HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}


def verify_digest(contents, digest):
    if hashlib.sha256(contents).hexdigest() != digest:
        raise ValueError("Input differs from the reviewed Classic lift.")


def select_function(unit, name):
    marker = f"\nvoid {name}(CPU *c) {{\n".encode("ascii")
    if unit.count(marker) != 1:
        raise ValueError(f"Expected one original definition of {name}.")
    start = unit.index(marker)
    end = unit.index(b"\n}\n", start) + len(b"\n}\n")
    return unit[start:end]


def stage_open(unit):
    verify_digest(unit, MENU_HASH)
    menu = select_function(unit, "f_886930")
    if unit.count(WARM_ENTRY) != 1 or menu.count(SETUP + WARM) != 1 or menu.count(FOLLOWUP) != 1:
        raise ValueError("The first-open warm call is missing or ambiguous.")
    if not unit.startswith(PRELUDE):
        raise ValueError("The reviewed unit's prelude is missing.")
    # The LE 1.3 package returns before the warm routine's prologue.
    # Leave the original calls and the flags untouched in that mode.
    return PRELUDE + HOOK + unit[len(PRELUDE):].replace(WARM_ENTRY, WARM_ENTRY + SKIP_WARM, 1)


def staged_fixture(staged, readiness):
    menu = select_function(staged, "f_886930")
    instructions = menu[menu.index(SETUP):menu.index(FOLLOWUP) + len(FOLLOWUP)]
    locals_ = menu[menu.index(b"    uint32_t eax"):menu.index(b"L_886930:")]
    return (PRELUDE + HOOK + b"void skyrim_chargen_probe_open_calls(CPU *c) {\n" + locals_
            + instructions + b"    SPILL();\n}\n" + select_function(staged, "f_8868C0")
            + select_function(readiness, "f_87EAA0")
            + select_function(staged, "f_886B50"))


class ChargenPrecacheTests(unittest.TestCase):
    def test_unreviewed_lift_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "reviewed Classic lift"):
            stage_open(b"authored menu stub\n" + SETUP + WARM + FOLLOWUP)

    def test_reviewed_first_open_and_cache_readiness(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation.")
        generation = Path(generation)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        if (state.get("schema") != 1 or state.get("mode") != "original-guest-bodies"
                or state.get("fixed_native_replacements") is not False
                or state.get("generation_finished") is not True or state.get("inputs_unchanged") is not True):
            raise ValueError("Requires a completed, unchanged original-body generation.")
        generated = generation / "gen"
        runtime = generation / "source" / "runtime"
        for header, digest in RUNTIME_HEADERS.items():
            verify_digest((runtime / header).read_bytes(), digest)
        source = generated / MENU_UNIT
        original = source.read_bytes()
        staged = stage_open(original)
        self.assertEqual(staged.replace(HOOK, b"", 1).replace(SKIP_WARM, b"", 1), original)
        readiness = (generated / READINESS_UNIT).read_bytes()
        verify_digest(readiness, READINESS_HASH)

        windows = sys.platform == "win32"
        compiler = shutil.which(os.environ.get("CC", "cc"))
        llvm = Path(os.environ.get("SKYRIM_LLVM_BIN", "C:/Program Files/LLVM/bin")) / "clang.exe"
        if windows and not compiler and llvm.is_file():
            compiler = str(llvm)
        if not compiler:
            self.skipTest("A C compiler is required.")
        headers = ["-isystem", str(generated), "-isystem", str(runtime)]
        args = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", *headers]
        if windows:
            args.append("-fuse-ld=lld")
        if sys.platform.startswith("linux"):
            args.append("-D_DEFAULT_SOURCE")
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                args.extend(["-fno-pie", "-no-pie"])
        native = ROOT / "native"
        with tempfile.TemporaryDirectory(prefix="classic-chargen-") as scratch:
            temporary = Path(scratch)
            menu = temporary / MENU_UNIT
            menu.write_bytes(staged)
            subprocess.run([compiler, "-std=c11", "-fsyntax-only", *headers, str(menu)],
                           check=True, timeout=60)
            private = temporary / "reviewed_fixture.c"
            private.write_bytes(staged_fixture(staged, readiness))
            binary = temporary / ("chargen.exe" if windows else "chargen")
            subprocess.run([*args, f"-I{native}", str(HERE / "test_chargen_precache.c"), str(private),
                            str(native / "guest_memory.c"), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
