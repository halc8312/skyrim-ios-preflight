"""Exercise ECE slot admission across the original Classic head traversal."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from stage_ece_nose_witness import DIGESTS
from stage_slider import function_span
from test_ece_nose_witness import read_owned_nose_ordinals

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PORT = ROOT / "ios" / "runtime-port"
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation


UNITS = {
    "exe_05.c": ("69a36b000a0edbdd5763929b761229bf19e079e4ae89e414127c20c168a3f360", ("420270",)),
    "exe_56.c": ("e68df5295941a1ccb5d202ac12342239cb2fda01b0b330b8241a7655b3cafdc7",
                 ("5610F0", "561180", "5611D0")),
    "exe_65.c": ("08847762a9424c74d92b5c26860c30c9f36b808974600227ae900bc48591e4f7", ("5997B0",)),
    "exe_67.c": (DIGESTS["exe_67.c"], ("5A4870", "5A4A40")),
    "exe_68.c": (DIGESTS["exe_68.c"], ("5A7570",)),
    "exe_185.c": (DIGESTS["exe_185.c"], ("87F6E0",)),
    "exe_285.c": ("d44f89f39fca50b0f56aac0fbdd8bd394c6c75f2489327038acc6bcfb2f1d075", ("AEE130", "AEE3D0")),
}


def dry_slider(body):
    # The fixture part callback prevents vertex writes. At the stock
    # persistence tail, bypass both the FaceMorphs and menu-float stores
    # in the first pass and keep the original interned-name cleanup.
    tail = b"L_87F9F0:\n"
    cleanup = (b"    TS32((uint32_t)(ecx + edi + 0x12Cu), (uint32_t)(edx));\n"
               b"    ecx = (uint32_t)((uint32_t)(esp + 0x30u));\n")
    if body.count(tail) != 1 or body.count(cleanup) != 1:
        raise ValueError("Expected one reviewed slider persistence tail")
    return body.replace(tail, tail + b"    if (test_dry_run(c)) goto test_cleanup;\n").replace(
        cleanup, cleanup.replace(b"    ecx =", b"test_cleanup:\n    ecx =", 1))


def fixture_sources(gen):
    result = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n#include <stdbool.h>\nbool test_dry_run(CPU *);\n'
    for name, (digest, functions) in UNITS.items():
        source = (gen / name).read_bytes()
        if hashlib.sha256(source).hexdigest() != digest:
            raise ValueError(f"Expected reviewed original Classic lift for {name}")
        for address in functions:
            start, end = function_span(source, address)
            body = source[start:end]
            result += dry_slider(body) if address == "87F6E0" else body
    return result


class EceHeadPreflightTests(unittest.TestCase):
    def test_stock_head_preflight(self):
        owner = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not owner or not compiler:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and use a host compiler")
        generation = Path(owner)
        state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
        validate_generation(state)
        self.assertEqual(state["mode"], "original-guest-bodies")
        self.assertIs(state["generation_finished"], True)
        self.assertIs(state["inputs_unchanged"], True)
        self.assertIs(state["fixed_native_replacements"], False)

        gen = generation / "gen"
        runtime = generation / "source/runtime"
        private = ROOT / "private"
        if private.resolve() != private:
            raise ValueError("Generated sources must stay in private/")
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ece-head-preflight-", dir=private) as tmp:
            directory = Path(tmp)
            entry = directory / "ece_head_preflight.c"
            (directory / "ece_head_preflight.inc").write_bytes(fixture_sources(gen))
            entry.write_text("#include <ece_head_preflight.inc>\n", encoding="ascii")
            binary = directory / ("ece-head-preflight.exe" if os.name == "nt" else "ece-head-preflight")
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                if sys.platform.startswith("linux"):
                    flags += ["-fno-pie", "-no-pie"]
            subprocess.run([compiler, *flags, "-I", PORT, "-isystem", directory, "-isystem", gen,
                            "-isystem", runtime, entry, HERE / "test_ece_head_preflight.c",
                            PORT / "mod_ece_slot.c", "-lm", "-o", binary], check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=30)
            owner_data = os.environ.get("SKYRIM_TEST_ECE_DEFAULT_DATA")
            if owner_data:
                up, down = read_owned_nose_ordinals(Path(owner_data))
                self.assertEqual((up, down), (13, 12))
                subprocess.run([binary, str(up), str(down)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
