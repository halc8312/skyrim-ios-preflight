"""Exercise the Classic active plugin catalogue with authored memory and optional pinned accessors."""
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
COSAVE = ROOT / "experiments/skse-cosave"
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation

PINNED_UNITS = (
    ("exe_09.c", "316b04f74d5c93286c8e602565e6807f2880602846dfa6a5e5642c4a7590bc2e",
     (b"f_43B650", b"f_43B660")),
    ("exe_10.c", "9145663db850b664809d720c852e19eaaa65cf309d5e51e6c0f88c6ad88d9221",
     (b"f_444830",)),
)


def pinned_accessors(generation, intermediate):
    state = json.loads((generation / "generation.json").read_bytes())
    validate_generation(state)
    if not state.get("generation_finished") or not state.get("inputs_unchanged"):
        raise ValueError("The pinned generation is incomplete or changed")
    source = b'#include "lift_prelude.h"\n'
    for name, digest, methods in PINNED_UNITS:
        data = (generation / "gen" / name).read_bytes()
        if sha256(data).hexdigest() != digest:
            raise ValueError(f"{name} differs from the reviewed Classic generation")
        for method in methods:
            start = data.index(b"\nvoid " + method + b"(CPU *c) {\n") + 1
            end = data.index(b"\n}\n", start) + 3
            source += data[start:end]
    intermediate.write_bytes(source)


@unittest.skipUnless(os.name == "posix", "Requires a POSIX C build toolchain")
class ClassicModCatalogTests(unittest.TestCase):
    def build_and_run(self, work, generation=None):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler unavailable")
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-I", PORT, "-I", COSAVE]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        source = [ROOT / "tests/native/test_classic_mod_catalog.c", PORT / "classic_mod_catalog.c"]
        if generation:
            lifted = work / "pinned-accessors.c"
            pinned_accessors(generation, lifted)
            flags += ["-Wno-unused-label", "-DCLASSIC_TEST_PINNED", "-DGAME_SKYRIM", "-isystem",
                      generation / "source/runtime"]
            source.append(lifted)
        binary = work / "classic-mod-catalog"
        subprocess.run([cc, *flags, *source, "-o", binary], check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=10)

    def test_exact_copied_rows_bounds_and_observable_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            self.build_and_run(Path(folder))

    def test_owned_accessors_assign_the_indices_seen_in_the_catalogue(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        if not generation:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private generation")
        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Generated sources must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="classic-mod-catalog-", dir=private) as folder:
            self.build_and_run(Path(folder), Path(generation).resolve())


if __name__ == "__main__":
    unittest.main()
