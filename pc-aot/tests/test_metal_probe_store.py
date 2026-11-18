"""Real Foundation record-store checks; unsupported hosts skip explicitly."""
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "METAL_PROBE_STORE cases=13 foundation_verified=1 metal_api_verified=0 physical_iphone_verified=0"


@unittest.skipUnless(platform.system() == "Darwin", "requires real Apple Foundation; not a portable substitute")
class ProbeStoreTests(unittest.TestCase):
    def test_atomic_records_recovery_and_path_confinement(self):
        parent = ROOT / "local" / "metal-probe-store"
        parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
        report = {"scope": "authored Foundation result store", "status": "failed",
                  "physical_iphone_verified": False, "metal_api_verified": False, "variants": []}
        try:
            for variant, flags in (("normal", []), ("asan-ubsan", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
                binary = output / ("store-" + variant)
                command = ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-fobjc-arc",
                           "-Wall", "-Wextra", "-Werror", "-mmacosx-version-min=14.0", "-g", "-O1",
                           "-I", str(ROOT / "experiments/metal-compile/ios"),
                           str(ROOT / "tests/native/test_metal_probe_store.mm"),
                           str(ROOT / "experiments/metal-compile/ios/ProbeRunStore.mm"),
                           "-framework", "Foundation", *flags, "-o", str(binary)]
                with (output / (variant + "-compile.stdout.log")).open("wb") as stdout, \
                     (output / (variant + "-compile.stderr.log")).open("wb") as stderr:
                    subprocess.run(command, stdout=stdout, stderr=stderr, check=True, timeout=60)
                env = os.environ.copy()
                env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
                result = subprocess.run([str(binary), str(output / (variant + "-records"))],
                                        capture_output=True, text=True, env=env, timeout=30)
                (output / (variant + "-run.stdout.log")).write_text(result.stdout)
                (output / (variant + "-run.stderr.log")).write_text(result.stderr)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), EXPECTED)
                self.assertEqual(result.stderr, "")
                report["variants"].append({"variant": variant, "status": "passed"})
            report["status"] = "passed"
        finally:
            (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    unittest.main()
