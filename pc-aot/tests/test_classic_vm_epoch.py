"""Classic VM generation with authored threads, including ASan/UBSan and TSan runs."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


@unittest.skipUnless(sys.platform in ("darwin", "linux") and shutil.which(os.environ.get("CC", "cc")),
                     "Unix native compiler required")
class VmEpochTests(unittest.TestCase):
    def build(self, directory, sanitizer):
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                 "-pthread", "-I", str(PORT)]
        mode = os.environ.get("SKYRIM_TEST_SANITIZERS", "")
        if sanitizer == "thread" or mode == "thread":
            flags += ["-fsanitize=thread"]
        elif mode == "1":
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        binary = Path(directory) / f"epoch-{sanitizer or 'plain'}"
        subprocess.run([*flags, PORT / "classic_vm_epoch.c", ROOT / "tests/native/test_classic_vm_epoch.c",
                        "-o", binary], check=True)
        return binary

    def test_sequence_and_stress(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = self.build(tmp, None)
            subprocess.run([binary, "sequence"], check=True, timeout=60)
            run = subprocess.run([binary, "stress"], check=True, timeout=120, capture_output=True, text=True)
            self.assertRegex(run.stdout, r"accepted=\d+ refused=\d+")

    def test_thread_sanitizer(self):
        if os.environ.get("SKYRIM_TEST_SANITIZERS") not in ("thread", "1"):
            self.skipTest("Set SKYRIM_TEST_SANITIZERS=thread (or 1) for the TSan run")
        with tempfile.TemporaryDirectory() as tmp:
            try:
                binary = self.build(tmp, "thread")
            except subprocess.CalledProcessError:
                self.skipTest("This compiler cannot build with -fsanitize=thread")
            env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1")
            for mode in ("sequence", "stress"):
                subprocess.run([binary, mode], check=True, timeout=300, env=env)


@unittest.skipUnless((Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream")) / "runtime").is_dir(),
                     "Pinned Skydream checkout required (MOD_FIRST_WAVE_PLAN.md section 3.1)")
class RegistrationStageTests(unittest.TestCase):
    def test_registration_reports_the_registry_only_in_hook_builds(self):
        sys.path.insert(0, str(PORT))
        import prepare_classic_skse as classic
        vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            for name in (*classic.PINNED, "skse_translation.c", classic.PRESENT_FILE, classic.INPUT_FILE):
                shutil.copyfile(vendor / "runtime" / name, stage / name)
            classic.prepare(vendor, stage)
            text = (stage / "skse_compat.c").read_text(encoding="utf-8")
        registration = text.index("void skse_register_natives(CPU *c, uint32_t registry) {\n")
        body = text[registration:text.index("\n}\n", registration)]
        self.assertTrue(body.split("\n", 1)[1].startswith(
            "#ifdef SKYRIM_IOS_MOD_HOOKS\n    skyrim_vm_epoch_registered(registry);\n#endif\n"))
        self.assertEqual(text.count("skyrim_vm_epoch_registered("), 1)
        self.assertLess(text.index('#ifdef SKYRIM_IOS_MOD_HOOKS\n#include "classic_vm_epoch.h"\n#endif\n'),
                        registration)


if __name__ == "__main__":
    unittest.main()
