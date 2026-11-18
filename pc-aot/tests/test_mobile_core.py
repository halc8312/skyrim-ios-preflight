"""Exercise touch timing, gamepad composition and frame limits without UIKit."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class MobileCoreTests(unittest.TestCase):
    def test_actual_input_expiry_pacing_and_concurrent_snapshots(self):
        cc=shutil.which(os.environ.get("CC","cc"))
        if not cc: self.skipTest("C compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="mobile-core-") as tmp:
            binary=Path(tmp)/"core"
            flags=["-std=c11","-D_POSIX_C_SOURCE=200809L","-Wall","-Wextra","-Werror","-O2","-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS")=="1":
                flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
            subprocess.run([cc,*flags,"-I",str(ROOT/"ios/runtime-port"),
                str(ROOT/"tests/native/test_mobile_core.c"),str(ROOT/"ios/runtime-port/mobile_core.c"),
                "-lm","-o",str(binary)],check=True,timeout=60)
            result=subprocess.run([str(binary)],check=True,text=True,capture_output=True,timeout=15)
            self.assertIn("concurrent snapshots PASS",result.stdout)


if __name__=="__main__": unittest.main()
