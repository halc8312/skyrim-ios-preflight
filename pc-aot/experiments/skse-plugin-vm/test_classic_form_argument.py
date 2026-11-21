"""Private original-body check for a Classic NativeFunctionBase Form argument."""

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
PACKAGE_FIXTURE = HERE.parent / "classic-package-decision"
sys.path.insert(0, str(PACKAGE_FIXTURE))
from stage import check_unit, function_span, validate_generation
from test_package_decision import HEADERS


UNITS = (
    ("exe_204.c", "ed35406810b26a5e990e8220f251b499e3be63ad19260be10101643f97969646", (0x8F8120,)),
    ("exe_339.c", "d62bb38e7b58b6db2f9999cd6dcac6fa5cbf524f0375b250a20e101b1e7bca94",
     (0xC3A620, 0xC3A830, 0xC3AC40)),
    ("exe_199.c", "f550390f7023b2375da3746cfd6b99be32bfd4fecabff2ce5396b73e2e18bf3c",
     (0x8D9070, 0x8D90E0)),
    ("exe_337.c", "bc5a71831af1bc743116b91a79eff10f48fd7350e2ec5af8792b4fbdbd5460fa",
     (0xC307C0, 0xC322E0)),
    ("exe_193.c", "fc151c85a6652f425fef73929585d366ab5c809486d5f1b360b9203016916218",
     (0x8B3980, 0x8B3B20, 0x8B3D60, 0x8B3E30, 0x8B4300)),
    ("exe_12.c", "c3043212335f627e2674517e7610619355eef51fd376297c4e2563413f1dbb34", (0x451A30,)),
    ("exe_259.c", "842247dde6c84c461377e75ba002b3b7dc1779bd33524489176820c9477b02f0",
     (0xA4A570, 0xA4AEE0, 0xA4B0F0)),
    ("exe_205.c", "9028925984d30f7d5f1dcada7a20a2a5f6bf211aace5c823de176241bbaaba70", (0x8FCD50,)),
)
PRELUDE = (
    b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
    b"void fixture_unpack_form(CPU *);\n"
    b"void fixture_form_handle(const CPU *, uint32_t, uint32_t);\n"
    b"void fixture_form_entry(const CPU *, uint32_t, uint32_t, uint32_t);\n"
)


def insert_at(body, original, replacement):
    if body.count(original) != 1:
        raise ValueError("Expected one specific fixture boundary")
    return body.replace(original, replacement)


def private_fixture(sources):
    parts = [PRELUDE]
    for _, digest, addresses in UNITS:
        unit = sources[digest]
        check_unit(unit, digest)
        for address in addresses:
            start, end = function_span(unit, f"f_{address:X}")
            body = unit[start:end]
            if address == 0x8F8120:
                body = insert_at(body, b"f_8D90E0(c);", b"fixture_unpack_form(c);")
            elif address == 0xC307C0:
                unlock = b"    ST_FENCE();\n    TS32((uint32_t)(edi), (uint32_t)(esi));\n"
                body = insert_at(body, unlock, b"    fixture_form_handle(c, eax, edx);\n" + unlock)
            elif address == 0x451A30:
                load = b"L_451A93:\n    esi = (uint32_t)(TL32((uint32_t)(eax + 0x4u)));\n"
                body = insert_at(body, load, load + b"    fixture_form_entry(c, M32(esp + 0xcu), edi, esi);\n")
            parts.append(body)
    return b"".join(parts)


class ClassicFormArgumentTest(unittest.TestCase):
    def test_refuses_a_modified_original(self):
        with self.assertRaisesRegex(ValueError, "differs from the reviewed input"):
            private_fixture({UNITS[0][1]: b"stand-in"})

    def test_stock_form_argument_and_bound_id_snapshot(self):
        owned = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not (owned and cc and os.name == "posix"):
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a POSIX host C compiler")
        generation = Path(owned)
        manifest = json.loads((generation / "generation.json").read_bytes())
        validate_generation(manifest)
        if (manifest.get("mode") != "original-guest-bodies" or manifest.get("fixed_native_replacements") is not False
                or manifest.get("generation_finished") is not True or manifest.get("inputs_unchanged") is not True):
            raise ValueError("Requires a completed, unchanged original-body generation")
        runtime = generation / "source/runtime"
        for name, digest in HEADERS.items():
            self.assertEqual(sha256((runtime / name).read_bytes()).hexdigest(), digest, name)
        paths = {digest: generation / "gen" / name for name, digest, _ in UNITS}
        sources = {digest: path.read_bytes() for digest, path in paths.items()}
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-g", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Lifted fixtures must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="classic-form-argument-", dir=private) as directory:
            stage = Path(directory) / "originals.c"
            stage.write_bytes(private_fixture(sources))
            binary = stage.with_suffix("")
            native = ROOT / "native"
            subprocess.run(
                [cc, *flags, "-isystem", generation / "gen", "-isystem", runtime, "-I", native,
                 "-I", PACKAGE_FIXTURE, stage, HERE / "test_classic_form_argument.c",
                 native / "guest_memory.c", PACKAGE_FIXTURE / "form_view.c", "-o", binary],
                check=True, timeout=60,
            )
            output = subprocess.check_output([binary], text=True, timeout=10)
            self.assertEqual(output.strip(), "Classic Form argument distinguishes None and verifies live Actor FormID")
        for digest, path in paths.items():
            self.assertEqual(path.read_bytes(), sources[digest])


if __name__ == "__main__":
    unittest.main()
