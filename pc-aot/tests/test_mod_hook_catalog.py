"""Plan X5: describe recorded DLL code patch requests (authored requests)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "C compiler required")
class HookCatalogTests(unittest.TestCase):
    def test_catalog(self):
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(PORT)]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "catalog"
            subprocess.run([*flags, ROOT / "tests/native/test_mod_hook_catalog.c", PORT / "mod_hook_catalog.c",
                            "-o", binary], check=True)
            subprocess.run([binary], check=True)

    def test_sites_have_evidence_in_the_reviewed_stages(self):
        package = (ROOT / "experiments/classic-package-decision/stage.py").read_text()
        for marker in ("M32(esp) = 0x6A9E95u", "M32(esp) = 0x8CAB59u", "L_6F02FF:"):
            self.assertIn(marker, package)
        precache = (ROOT / "experiments/classic-chargen-precache/test_chargen_precache.py").read_text()
        self.assertIn("M32(esp) = 0x886AD0u; SPILL(); f_8868C0(c);", precache)
        self.assertIn("M32(esp) = 0x886ACBu; SPILL(); f_880EA0(c);", precache)


if __name__ == "__main__":
    unittest.main()
