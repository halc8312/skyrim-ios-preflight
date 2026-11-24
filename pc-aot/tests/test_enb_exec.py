"""ENB effect execution on a real D3D9 device (native DXVK on a Vulkan driver), compared pixel by pixel.

Needs a built experiments/enb-exec binary (see its README.md). Set SKYRIM_ENB_EXEC_TEST to that
enb_exec_test binary; Vulkan needs a driver (for example VK_ICD_FILENAMES for lavapipe) and a display
(xvfb-run). Without the variable the check is skipped.
"""
import os
from pathlib import Path
import shutil
import subprocess
import unittest


class EnbExecTests(unittest.TestCase):
    def test_effects_render_expected_pixels(self):
        binary = os.environ.get("SKYRIM_ENB_EXEC_TEST")
        if not binary or not Path(binary).is_file():
            self.skipTest("Set SKYRIM_ENB_EXEC_TEST to the built enb_exec_test (experiments/enb-exec/README.md).")
        command = [binary]
        if not os.environ.get("DISPLAY") and shutil.which("xvfb-run"):
            command = ["xvfb-run", "-a", binary]
        env = dict(os.environ, SDL_VIDEODRIVER=os.environ.get("SDL_VIDEODRIVER", "x11"))
        run = subprocess.run(command, capture_output=True, text=True, timeout=600, env=env)
        self.assertEqual(run.returncode, 0, run.stdout[-2000:] + run.stderr[-2000:])
        self.assertIn("Verified 17 ENB effect checks", run.stdout)


if __name__ == "__main__":
    unittest.main()
