# SPDX-License-Identifier: GPL-3.0-only
"""Compare the original allocator dispatch and the reviewed Safety Load branch."""
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from stage_allocator import BYPASS, DECLARATION, ONE_SHOT, UNIT, allocator_region, stage_allocator

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from mod_hooks_guest import validate_generation


class AllocatorTests(unittest.TestCase):
    def test_refuses_unreviewed_unit(self):
        with self.assertRaisesRegex(ValueError, "differs from the reviewed"):
            stage_allocator(b"authored input")

    def test_real_crt_fallback_with_skydream_heap(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not generation or not compiler:
            self.skipTest("A fixed-version Classic lift and a native C compiler are required")
        lift = Path(generation).resolve()
        state = json.loads((lift / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state, lift)
        crt = (lift / "gen/exe_467.c").read_bytes()
        runtime = lift / "source/runtime"
        heap = runtime / "heap.c"
        self.assertEqual(sha256(crt).hexdigest(), "e40f124913665af49895b591147fbaa3cc11cae5b5534aa5b6c0b89d33253769")
        self.assertEqual(sha256(heap.read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
                         "6b913b89b09846adfe86338dfe1e6f576db7f33c0ada2b6afbb44fe8a878bc92")
        bodies = []
        for function in ("f_F568FA", "f_F5872D", "f_F587F5"):
            anchor = f"\nvoid {function}(CPU *c) {{\n".encode()
            self.assertEqual(crt.count(anchor), 1)
            start = crt.index(anchor)
            end = crt.index(b"\n}\n", start) + 3
            bodies.append(crt[start:end])
        flags = ["-std=gnu11", "-O2", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        with tempfile.TemporaryDirectory(prefix="classic-crt-fallback-") as directory:
            directory = Path(directory)
            fixture = directory / "crt.c"
            fixture.write_bytes(b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n' + b"".join(bodies))
            binary = directory / "crt-test"
            subprocess.run([compiler, *flags, "-isystem", str(runtime), "-isystem", str(lift / "gen"),
                            "-I", str(ROOT / "native"), fixture, HERE / "test_crt_fallback.c", heap,
                            ROOT / "native/guest_memory.c", "-o", binary], check=True, timeout=90)
            subprocess.run([binary], check=True, timeout=30)

    def test_owned_allocator_continuation(self):
        lift = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not lift or not compiler:
            self.skipTest("A fixed-version Classic lift and a native C compiler are required")
        lift = Path(lift).resolve()
        state = json.loads((lift / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state, lift)
        source = lift / "gen" / UNIT
        original = source.read_bytes()
        staged = stage_allocator(original)
        self.assertEqual(staged.count(BYPASS), 1)
        self.assertEqual(staged.removeprefix(DECLARATION).replace(BYPASS, b"", 1).replace(ONE_SHOT, b"", 1),
                         original)

        def fixture(variant, body):
            start, end = allocator_region(body)
            finish = body.index(b"L_A48F0F:\n", end)
            return (f"void {variant}_allocator(CPU *c) {{\n".encode() +
                    (ONE_SHOT if variant == "staged" else b"") +
                    b"    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; Flags f;\n"
                    b"    RELOAD();\n    goto L_A48E9C;\n" + body[start:finish] +
                    b"L_A48DD2:\n    goto L_A48E9C;\nL_A48F0F:\n    SPILL();\n}\n")

        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        includes = ["-isystem", str(lift / "source/runtime"), "-isystem", str(lift / "gen"),
                    "-I", str(ROOT / "native")]
        with tempfile.TemporaryDirectory(prefix="classic-allocator-") as directory:
            directory = Path(directory)
            composed = directory / UNIT
            composed.write_bytes(staged)
            subprocess.run([compiler, "-std=gnu11", "-w", "-fsyntax-only", *includes, composed],
                           check=True, timeout=90)
            fragment = directory / "allocator_fragment.c"
            fragment.write_bytes(DECLARATION + b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n' +
                                 fixture("baseline", original) + fixture("staged", staged))
            binary = directory / "allocator-test"
            subprocess.run([compiler, *flags, *includes, fragment, HERE / "test_allocator.c",
                            ROOT / "native/guest_memory.c", "-o", binary], check=True, timeout=90)
            subprocess.run([binary], check=True, timeout=30)
        self.assertEqual(sha256(source.read_bytes()).digest(), sha256(original).digest())


if __name__ == "__main__":
    unittest.main()
