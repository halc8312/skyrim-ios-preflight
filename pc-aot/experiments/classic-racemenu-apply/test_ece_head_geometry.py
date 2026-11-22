"""Compare the read-only face geometry selection to the fixed original lookup."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from stage_slider import function_span


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation


UNITS = {
    "exe_275.c": ("c980cb667ac15d7f1bbe6df18de61cb550dd8319719eac53e5f74da4cd33a716", ("AAE1D0", "AAF450")),
    "exe_347.c": ("a6c8a2d10b61c322393674fd99519913d537d8d1e5a72e6d2d52ff3fc653290f", ("C707B0",)),
    "exe_39.c": ("1620dbc2dc87c500b7a76f067771ce31d847c8447c44f2f30683ad790751acbe", ("4F8CA0",)),
    "exe_260.c": ("9d9ae9b0e211456c682c9bcc78f4ae7d5a95a6f9f4f9940157cab79e21e03c73", ("A50AC0",)),
    "exe_78.c": ("cecbb2e1a27d705caee9b09a4e635d8f4a3038dbc51bea0fc5573a521ac211bb", ("5EADD0",)),
}


class EceHeadGeometryTests(unittest.TestCase):
    def test_original_name_order_and_trigeometry_cast(self):
        source = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not source or not compiler:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a host compiler")
        generation = Path(source)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        self.assertEqual(state["mode"], "original-guest-bodies")
        self.assertIs(state["generation_finished"], True)
        self.assertIs(state["inputs_unchanged"], True)
        self.assertIs(state["fixed_native_replacements"], False)
        gen = generation / "gen"
        runtime = generation / "source/runtime"
        bodies = [b'#include "lift_prelude.h"\nvoid f_AAF450(CPU *);\n']
        for name, (digest, addresses) in UNITS.items():
            raw = (gen / name).read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), digest, "Expected the reviewed original Classic lift")
            for address in addresses:
                start, end = function_span(raw, address)
                bodies.append(raw[start:end])
        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Generated sources must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ece-geometry-", dir=private) as tmp:
            directory = Path(tmp)
            lifted = directory / "lifted.inc"
            lifted.write_bytes(b"".join(bodies))
            src = directory / "entry.c"
            src.write_text("#include <lifted.inc>\n", encoding="ascii")
            binary = directory / ("geometry.exe" if os.name == "nt" else "geometry")
            flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                if sys.platform.startswith("linux"):
                    flags += ["-fno-pie", "-no-pie"]
            subprocess.run([compiler, *flags, "-isystem", directory, "-isystem", gen, "-isystem", runtime,
                            src, HERE / "test_ece_head_geometry.c", HERE / "ece_head_geometry.c",
                            "-o", binary], check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
