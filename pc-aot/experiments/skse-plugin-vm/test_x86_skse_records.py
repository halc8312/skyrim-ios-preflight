"""Exercise the bounded host-side record stream with synthetic bytes and allocation failures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class ClassicRecordStreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cl" if os.name == "nt" else "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        build = Path(temporary.name)
        cls.binary = build / ("fixture.exe" if os.name == "nt" else "fixture")
        source, test = HERE / "x86_skse_records.c", HERE / "test_x86_skse_records.c"
        if Path(cc).name.lower() in ("cl.exe", "cl"):
            flags = [cc, "/nologo", "/std:c11", "/MT", "/W4", "/WX", "/O2"]
            subprocess.run([*flags, "/Drealloc=skse_test_realloc", "/c", source, "/Forecord.obj"],
                           cwd=build, check=True)
            subprocess.run([*flags, test, "record.obj", "/Fefixture.exe"], cwd=build, check=True)
        else:
            flags = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
            if os.environ.get("SKSE_PROBE_SANITIZE") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([*flags, "-Drealloc=skse_test_realloc", "-c", source, "-o", "record.o"],
                           cwd=build, check=True)
            subprocess.run([*flags, test, "record.o", "-o", cls.binary], cwd=build, check=True)

    def run_case(self, case):
        return subprocess.run([self.binary, case], check=True, capture_output=True, text=True,
                              timeout=5).stdout

    def test_insertion_order_empty_duplicate_partial_and_skipped_records(self):
        rows = [line.split() for line in self.run_case("trace").splitlines()]
        self.assertEqual(rows, [
            ["0", "0", "0", "5", "2", "0011"], ["1", "0", "0", "0", "0"],
            ["2", "4294967295", "7", "4", "4", "800061ff"],
            ["3", "4294967295", "7", "2", "1", "51"],
            ["4", "1073741825", "4275878552", "0", "0"],
        ])

    def test_phase_rewind_eof_and_invalid_buffers(self):
        self.run_case("phases")

    def test_seventeenth_record_rejected_with_no_side_effects(self):
        self.run_case("record-limit")

    def test_aggregate_limit_not_per_record_and_no_integer_wrap(self):
        self.run_case("byte-limit")

    def test_allocation_failure_restores_the_same_open_record(self):
        self.run_case("oom")

    def test_two_callers_can_write_read_and_close_independently(self):
        self.run_case("owners")

    def test_owned_copy_rewinds_without_moving_source_and_can_outlive_it(self):
        self.run_case("copy")

if __name__ == "__main__":
    unittest.main()
