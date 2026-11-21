# SPDX-License-Identifier: GPL-3.0-only
"""Stage one Classic slot read and exercise its original lifted call path."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_fresh_load import HEADERS, check_unit, function_span, validate_generation

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SLOT_UNIT = "exe_102.c"
SLOT_SHA = "51c73b53b9acd3a11be7e1994272d45dcac547c8aebe0dcd2dd8cfd6bea47e43"
READER_UNIT = "exe_101.c"
READER_SHA = "8ec90cddea6c81f16123a5f9fbc7ae7f9059295712fac1383565ce2abdeb697b"
READ_BLOCK_SHA = "3c739fdc2680ea502efda1e87a399a774b0391e9c606c48169f02901d3b34b4e"
RETURN_MARKER = b"0x67B875u"
READ_CALL = b"f_677990(c);"
INCLUDES = b'#include <errno.h>\n#include "fresh_load.h"\n'
OBSERVED_CALL = b"""{
        const uint32_t stream = c->r[1];
        f_677990(c);
        const uint32_t bytes = c->r[0];
        if (bytes && bytes <= CLASSIC_FRESH_SLOT_READ_BYTES) {
            const int original_errno = errno;
            classic_fresh_slot_read_observed(c, stream, bytes);
            errno = original_errno;
        }
    }"""


def stage_slot_read(unit: bytes) -> tuple[bytes, bytes, bytes]:
    check_unit(unit, SLOT_SHA)
    start, end = function_span(unit, "f_67B720")
    lines = unit[start:end].splitlines(keepends=True)
    matches = [index for index, line in enumerate(lines) if RETURN_MARKER in line and READ_CALL in line]
    if len(matches) != 1 or matches[0] < 4:
        raise ValueError("Expected one reviewed slot read")
    index = matches[0]
    original = b"".join(lines[index - 4:index + 1])
    if hashlib.sha256(original).hexdigest() != READ_BLOCK_SHA:
        raise ValueError("Slot read block differs from the reviewed input")
    if unit.count(original) != 1:
        raise ValueError("Slot read block is not unique")
    observed = original.replace(READ_CALL, OBSERVED_CALL, 1)
    return INCLUDES + unit.replace(original, observed, 1), original, observed


def fixture_function(name: str, block: bytes) -> bytes:
    return (f"void {name}(CPU *c) {{\n".encode("ascii")
            + b"    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f;\n"
              b"    RELOAD();\n"
            + block + b"    SPILL();\n}\n")


class LogicalSlotReadTests(unittest.TestCase):
    def test_rejects_unreviewed_unit(self):
        with self.assertRaises(ValueError):
            stage_slot_read(b"authored input")

    def test_pinned_logical_read(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not compiler or not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a C compiler")
        generation = Path(generation)
        state = json.loads((generation / "generation.json").read_bytes())
        validate_generation(state)
        self.assertEqual(state["mode"], "original-guest-bodies")
        self.assertIs(state["generation_finished"], True)
        self.assertIs(state["inputs_unchanged"], True)
        self.assertIs(state["fixed_native_replacements"], False)

        runtime = generation / "source" / "runtime"
        for name, digest in HEADERS.items():
            self.assertEqual(hashlib.sha256((runtime / name).read_bytes()).hexdigest(), digest, name)
        slot_path = generation / "gen" / SLOT_UNIT
        reader_path = generation / "gen" / READER_UNIT
        slot = slot_path.read_bytes()
        reader = reader_path.read_bytes()
        check_unit(reader, READER_SHA)
        start, end = function_span(reader, "f_677990")
        staged, original_block, observed_block = stage_slot_read(slot)
        self.assertEqual(staged.removeprefix(INCLUDES).replace(observed_block, original_block, 1), slot)
        with self.assertRaises(ValueError):
            stage_slot_read(staged)

        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Generated test sources must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="classic-slot-read-", dir=private) as scratch:
            output = Path(scratch)
            includes = ["-isystem", str(generation / "gen"), "-isystem", str(runtime), "-I", str(HERE)]
            full_unit = output / SLOT_UNIT
            full_unit.write_bytes(staged)
            subprocess.run([compiler, "-std=gnu11", "-w", "-fsyntax-only", *includes, full_unit],
                           check=True, timeout=60)

            fixture = output / "lifted.c"
            fixture.write_bytes(INCLUDES + b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
                                + reader[start:end] + fixture_function("baseline_slot_read", original_block)
                                + fixture_function("observed_slot_read", observed_block))
            flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
                if sys.platform.startswith("linux"):
                    flags += ["-fno-pie", "-no-pie"]
            binary = output / "slot-read"
            subprocess.run([compiler, *flags, *includes, fixture, HERE / "test_slot_read.c", "-o", binary],
                           check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=20)

        self.assertEqual(slot_path.read_bytes(), slot)
        self.assertEqual(reader_path.read_bytes(), reader)


if __name__ == "__main__":
    unittest.main()
