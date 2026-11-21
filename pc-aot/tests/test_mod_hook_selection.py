"""Launch selection of guest MOD hook groups per feature."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "C compiler required")
class HookSelectionTests(unittest.TestCase):
    def test_plan(self):
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(PORT)]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "plan"
            subprocess.run([*flags, ROOT / "tests/native/test_mod_hook_selection.c", PORT / "mod_hook_selection.c",
                            "-o", binary], check=True)
            subprocess.run([binary], check=True)

    def test_bits_match_the_abi(self):
        header = (ROOT / "ios/private-abi/PcGuestModHooksABI.h").read_text()
        source = (PORT / "mod_hook_selection.c").read_text()
        for name, value in (("FUZ_MENU", 1), ("FUZ_WORLD", 2), ("VM_LIFECYCLE", 4), ("CHARGEN_PRECACHE", 8)):
            self.assertIn(f"PC_GUEST_MOD_HOOK_{name} = {value}u", header)
            self.assertIn(f"{name} = {value}u", source)


if __name__ == "__main__":
    unittest.main()
