"""Exercise the isolated Classic Papyrus registration contract with an authored VM fixture."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_pe_image import find_tool

HERE = Path(__file__).resolve().parent


class PapyrusRegistrationTrial(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = find_tool(os.environ.get("CC", "cc"), "clang", "clang-18", "cl")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        scratch = tempfile.TemporaryDirectory(prefix="classic-papyrus-trial-")
        cls.addClassCleanup(scratch.cleanup)
        build = Path(scratch.name)
        cls.binary = build / ("fixture.exe" if os.name == "nt" else "fixture")
        sources = [HERE / "papyrus_trial.c", HERE / "test_papyrus_trial.c"]
        if Path(cc).stem.lower() == "cl":
            command = [cc, "/nologo", "/std:c11", "/MT", "/W4", "/WX", "/O2", *sources, "/Fefixture.exe"]
        else:
            flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.name == "nt":
                flags.extend(["-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"])
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags.extend(["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"])
            command = [cc, *flags, *sources, "-o", cls.binary]
        subprocess.run(command, cwd=build, check=True, timeout=60)

    def run_case(self, name):
        subprocess.run([self.binary, name], check=True, timeout=10)

    def test_classic_v1_table(self):
        self.run_case("interface")

    def test_owner_and_epoch_are_explicit(self):
        self.run_case("owners")

    def test_full_and_empty_epochs(self):
        self.run_case("limit")

    def test_validate_all_callbacks_before_invoking(self):
        self.run_case("mapping")

    def test_fault_stops_dispatch(self):
        self.run_case("fault")

    def test_read_bool_in_al(self):
        self.run_case("declined")
        self.run_case("bad-bool")

    def test_capture_and_run_owned_ifunction_twice(self):
        self.run_case("native")

    def test_check_descriptor_and_frame_before_run(self):
        self.run_case("native-guard")

    def test_later_registration_failure_disables_captured_function(self):
        self.run_case("native-registration-fault")

    def test_native_failure_keeps_result_unchanged_and_closes_epoch(self):
        self.run_case("native-fault")
        self.run_case("native-declined")
        self.run_case("native-type")


if __name__ == "__main__":
    unittest.main()
