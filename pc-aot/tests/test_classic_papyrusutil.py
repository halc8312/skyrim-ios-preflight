"""PapyrusUtil JsonUtil publication: VM generation checks with an authored settings backend.

The settings scope itself (other profiles, interrupted writes, restarts and the
owner's DLL) is covered by experiments/skse-plugin-vm (settings_session) and
test_classic_papyrus_int0; the owner-input runs need SKYRIM_TEST_OWNED_PAPYRUS_ZIP
and SKYRIM_TEST_OWNED_LE_EXE.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


@unittest.skipUnless(sys.platform in ("darwin", "linux") and shutil.which(os.environ.get("CC", "cc")),
                     "POSIX compiler required")
class PapyrusUtilTests(unittest.TestCase):
    def test_generation_guarded_publication(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / "x86.h").write_text("#pragma once\n#include <stdint.h>\n"
                                         "typedef struct CPU { uint32_t r[8]; } CPU;\nextern uint8_t *MEM;\n")
            flags = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-pthread",
                     "-I", str(stage), "-I", str(PORT)]
            mode = os.environ.get("SKYRIM_TEST_SANITIZERS")
            if mode == "1":
                flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
            elif mode == "thread":
                flags += ["-fsanitize=thread"]
            binary = stage / "papyrusutil"
            subprocess.run([*flags, ROOT / "tests/native/test_classic_papyrusutil.c", PORT / "classic_papyrusutil.c",
                            PORT / "classic_vm_epoch.c", "-o", binary], check=True)
            subprocess.run([binary], check=True, timeout=60)

    def test_function_table_matches_the_settings_methods(self):
        source = (PORT / "classic_papyrusutil.c").read_text()
        settings = (ROOT / "experiments/skse-plugin-vm/classic_settings.c.in").read_text()
        ours = re.findall(r'\{"JsonUtil", "(\w+)", (\d+), (\d), (\d), \{([\d, ]+)\}\}', source)
        theirs = re.findall(r'\[SKSE_SETTINGS_\w+\] = \{"(\w+)", (\d), (\d), \{([\d, ]+)\}\}', settings)
        self.assertEqual(len(ours), 12)
        self.assertEqual([(n, r, c, a) for n, _id, r, c, a in ours], theirs)
        self.assertEqual([int(i) for _n, i, *_ in ours], list(range(1, 13)))


if __name__ == "__main__":
    unittest.main()
