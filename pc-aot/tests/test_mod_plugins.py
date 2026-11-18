"""Real MOD plugin header/order code; authored headers only, no game assets or UIKit claims."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class ModPluginTests(unittest.TestCase):
    def test_real_header_parsing_order_and_plugin_list(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="mod-plugins-") as tmp:
            binary = Path(tmp) / "mod_plugins"
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([cc, *flags, "-I", str(PORT), str(PORT / "mod_plugins.c"),
                str(ROOT / "tests/native/test_mod_plugins.c"), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary), tmp], check=True, text=True, capture_output=True, timeout=30)
            for line in ("header parsing PASS", "names PASS", "ordering PASS",
                         "plugins.txt PASS", "case-insensitive paths PASS"):
                self.assertIn(line, result.stdout)

    def test_guest_reads_plugins_txt_from_the_profile_documents_folder(self):
        """The guest's SHGetFolderPathA returns Documents for every CSIDL, so the
        classic engine's LOCAL_APPDATA\\Skyrim\\plugins.txt lands under the profile."""
        misc = ROOT / "vendor/skydream/runtime/misc.c"
        if not misc.exists():
            self.skipTest("vendor/skydream checkout required")
        text = misc.read_text(encoding="utf-8")
        shim = text[text.index("SHIM(SHGetFolderPathA)"):text.index("SHIM(SHCreateDirectoryExA)")]
        self.assertIn('"C:\\\\Users\\\\Player\\\\Documents"', shim)
        self.assertNotIn("ARG(1)", shim.split("*/", 1)[1])  # csidl is ignored
        ui = (PORT / "mobile_ui.m").read_text(encoding="utf-8")
        self.assertIn('@"Documents"', ui)
        self.assertIn('"Skyrim"', ui)
        self.assertIn('"plugins.txt"', ui)


if __name__ == "__main__":
    unittest.main()
