"""Real CPU oracle/order controls and Apple-only MRC/legacy GPU controls."""
from pathlib import Path
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENT = ROOT / "experiments/metal-compile"
CXX = shutil.which(os.environ.get("CXX", "c++"))
CLANGXX = shutil.which(os.environ.get("CLANGXX", "clang++")) or shutil.which("clang++-18")


class SourceIdentityControls(unittest.TestCase):
    def test_frozen_semantic_panels_are_disjoint_from_each_other_and_build4(self):
        base = (EXPERIMENT / "phone_fixture.metal").read_bytes()
        self.assertEqual(hashlib.sha256(base).hexdigest(),
                         "26ed56a8f4d264965eb624911d42cbf925f1329f0f103d58f683fe1c52d07ad6")
        implementation = (EXPERIMENT / "phone_contention.mm").read_text(encoding="utf-8")
        matches = re.findall(r'static NSString\* const Kernel = @R"MSL\((.*?)\)MSL";', implementation, re.S)
        self.assertEqual(len(matches), 1)
        all_hashes = set()
        expected = ((1, 8, "7811f2498dbfcb761c80a5f530c2f398451142b81b1fa781a6002c1b0404b225"),
                    (9, 40, "40da8a562c60436107a1e2651131cd8dc2eaca2a7358bd59811cc15a8862319e"),
                    (41, 72, "de0a9f8ff8a26a60837622be1944528165cb6c100b4cd71102001b453de1c053"))
        for first, last, frozen in expected:
            hashes = []
            for group in range(first, last + 1):
                for role in range(3):
                    salt = 0x31415927 ^ ((group * 0x9E3779B9) & 0xFFFFFFFF) ^ ((role * 0x85EBCA6B) & 0xFFFFFFFF)
                    source = base + (f"\n#define CONTENTION_SALT 0x{salt:08x}u\n" + matches[0]).encode("utf-8")
                    digest = hashlib.sha256(source).hexdigest()
                    self.assertNotIn(digest, all_hashes)
                    all_hashes.add(digest)
                    hashes.append(digest)
            self.assertEqual(hashlib.sha256("".join(hashes).encode("ascii")).hexdigest(), frozen)
        self.assertEqual(len(all_hashes), 216)


class Logged(unittest.TestCase):
    def directory(self, kind):
        directory = ROOT / "local/tests" / f"phone-contention-{kind}-{time.time_ns()}-{os.getpid()}"
        directory.mkdir(parents=True, exist_ok=False)
        return directory

    def execute(self, directory, label, command, timeout=180):
        with (directory / f"{label}.stdout.log").open("wb") as out, \
                (directory / f"{label}.stderr.log").open("wb") as err:
            result = subprocess.run(command, stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                    timeout=timeout, check=False)
        self.assertEqual(result.returncode, 0, (directory / f"{label}.stderr.log").read_text(errors="replace"))
        self.assertEqual((directory / f"{label}.stderr.log").stat().st_size, 0)
        return (directory / f"{label}.stdout.log").read_text().strip()


@unittest.skipUnless(CLANGXX, "requires Clang Objective-C++ parser")
class HeaderNullabilityControls(Logged):
    def test_objc_and_portable_header_with_nullability_warnings_as_errors(self):
        directory = self.directory("header-nullability")
        foundation = directory / "Foundation"
        foundation.mkdir()
        # Declaration-only shim: exercise Clang's real nullability diagnostics
        # without pretending to compile Foundation/Metal or run an Apple API.
        (foundation / "Foundation.h").write_text(
            '#pragma once\n'
            '#define FOUNDATION_EXPORT extern\n'
            '#define NS_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")\n'
            '#define NS_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")\n'
            '#define NS_RETURNS_NOT_RETAINED __attribute__((ns_returns_not_retained))\n'
            'typedef signed char BOOL;\n@class NSDictionary, NSString;\n', encoding="utf-8")
        source = directory / "header.mm"
        source.write_text('#include "phone_contention.h"\n', encoding="utf-8")
        common = [CLANGXX, "-std=c++17", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                  "-Wnullability-completeness", "-I", str(directory), "-I", str(EXPERIMENT)]
        self.execute(directory, "objc-header", common + ["-x", "objective-c++", "-fblocks", str(source)])
        self.execute(directory, "portable-header", common + ["-x", "c++", str(source)])
        control = directory / "missing-annotation.mm"
        # Completeness diagnostics apply to included headers, not ordinary
        # implementation-file declarations. Keep the negative control there.
        (directory / "missing-annotation.h").write_text(
            'const char* _Nullable annotated();\nconst char* missing();\n', encoding="utf-8")
        control.write_text('#include "missing-annotation.h"\n', encoding="utf-8")
        with (directory / "negative.stdout.log").open("wb") as out, \
                (directory / "negative.stderr.log").open("wb") as err:
            result = subprocess.run(common + ["-x", "objective-c++", str(control)], stdout=out, stderr=err,
                                    stdin=subprocess.DEVNULL, timeout=30, check=False)
        self.assertNotEqual(result.returncode, 0, "compiler did not enforce nullability completeness")
        self.assertIn("-Wnullability-completeness", (directory / "negative.stderr.log").read_text(errors="replace"))


@unittest.skipUnless(os.name == "posix" and CXX, "requires a POSIX C++17 compiler")
class CPUControls(Logged):
    def test_frozen_oracle_and_actual_schedule_both_orders(self):
        for mode, flags in (("normal", []), ("sanitized", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
            with self.subTest(mode=mode):
                directory = self.directory(mode)
                exe = directory / "cpu-controls"
                self.execute(directory, "compile", [CXX, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    *flags, "-I", str(EXPERIMENT), str(ROOT / "tests/native/test_metal_phone_contention.cpp"), "-o", str(exe)])
                self.assertEqual(self.execute(directory, "run", [str(exe)]),
                    "PHONE_CONTENTION_CPU golden_vectors=216 panels=2 balanced_trials=32 requests=96 unique_per_panel=96 gpu_execution_verified=0")


@unittest.skipUnless(platform.system() == "Darwin", "requires Apple SDK, Foundation, and actual Metal GPU")
class MacMRCControls(Logged):
    def test_manual_ownership_success_error_partial_stop_and_legacy_gpu(self):
        directory = self.directory("mac-mrc")
        exe = directory / "mrc-controls"
        report = directory / "gpu-report.json"
        self.execute(directory, "compile", ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
            "-fno-objc-arc", "-fblocks", "-fexceptions", "-fobjc-exceptions", "-mmacosx-version-min=26.0",
            "-Wall", "-Wextra", "-Werror", "-I", str(EXPERIMENT),
            str(ROOT / "tests/native/test_metal_phone_contention.mm"),
            "-framework", "Foundation", "-framework", "Metal", "-o", str(exe)])
        self.assertEqual(self.execute(directory, "run", [str(exe), str(EXPERIMENT / "phone_fixture.metal"), str(report)]),
            "PHONE_CONTENTION_MAC_MRC boundaries=3 readbacks=6 compiler_errors=1 stop_after_submit=2 physical_iphone_verified=0 metal4_task_verified=0")
        data = json.loads(report.read_text())
        self.assertEqual(data["status"], "passed")
        self.assertFalse(data["physical_iphone_verified"])
        self.assertFalse(data["metal4_task_lifetime_verified"])
        self.assertTrue(data["mac_legacy_mrc_verified"])
        self.assertEqual(len(data["trials"]), 2)
        self.assertEqual(len(data["error_controls"]), 1)
        self.assertEqual(len(data["partial_stop"]), 2)
        self.assertEqual([p["count"] for p in data["source_identity_panels"]], [24, 96, 96])
        for trial in data["trials"]:
            self.assertEqual(trial["planned_overlap"], trial["mode"] == "contended")
            self.assertTrue(trial["actual_overlap_evaluated"])
            self.assertEqual(len(trial["jobs"]), 3)
            for job in trial["jobs"]:
                self.assertTrue(job["oracle_verified"])
                self.assertEqual(len(job["compute_words"]), 64)
                self.assertTrue(job["owned_input_snapshots_verified"])
                self.assertTrue(job["adapter_input_reference_released_after_drain"])
                self.assertFalse(job["metal4_task_joined"])


if __name__ == "__main__":
    unittest.main()
