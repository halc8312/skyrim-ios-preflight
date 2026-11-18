"""Authored reports: real wire/parser checks, plus actual Foundation sender on Mac.

The native executable includes the production client. Only bundle configuration
and the NSURLSession configuration factory are substituted; NSURLProtocol handles
all transport without networking. A non-Mac skip is not an Apple client pass.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
FIXTURE = ROOT / "tests/native/diagnostic_upload_fixture.json"


def receiver():
    name = "diagnostic_upload_receiver_fixture"
    definition = importlib.util.spec_from_file_location(name, ROOT / "scripts/receive_private_diagnostics.py")
    module = importlib.util.module_from_spec(definition)
    sys.modules[name] = module
    definition.loader.exec_module(module)
    return module


class DiagnosticUploadTests(unittest.TestCase):
    def test_authored_allowlist_preserves_actual_parser_records(self):
        module = receiver()
        schema = module.Schema()
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        for line in fixture["allowed"]:
            with self.subTest(prefix=line.split(" ", 1)[0]):
                schema.line(line)
        result = schema.timing.summarize(fixture["allowed"])
        self.assertEqual(result["invalid_records"], 0)
        self.assertEqual(result["kinds"]["frame_gap"]["over_100ms"], 1)
        self.assertEqual(len(result["work_contexts"]), 2)
        self.assertEqual({row["kind"] for row in result["work_contexts"]},
                         {"mvk_msl_wait", "graphics_state_wait"})
        self.assertEqual(result["work_contexts"][0]["over_100ms"], 1)
        self.assertEqual(result['shader_lead']['observations'], 4)
        self.assertEqual(result['shader_lead']['results']['hit_verified_prefetch']['count'], 2)
        self.assertEqual(result['shader_lead']['results']['hit_producer_unobserved']['count'], 1)
        self.assertEqual((result['shader_lead']['dropped_busy'], result['shader_lead']['dropped_full']), (1, 2))
        self.assertEqual({row['seq'] for row in result['shader_lead']['longest_waits']}, {1, 2, 3, 4})
        # Actual receiver contract also accepts separately identified old-build
        # settings. A current build must never relabel the previous session.
        report = {"schema": 1, "report_id": "1" * 32, "app_build": 12, "sessions": [
            {"slot": "current", "capture": "live_prefix", "build": 12, "snapshot_bytes": 10000,
             "excluded_lines": 3, "lines": fixture["allowed"]},
            {"slot": "previous", "capture": "closed", "build": 8, "snapshot_bytes": 200,
             "excluded_lines": 1, "lines": ["IOS_DIAG_SETTING key=0 value=30",
                                          "IOS_SHADER_OPTION option=1 enabled=1"]}
        ]}
        self.assertEqual(schema.bundle(json.dumps(report).encode()), report)

    def test_raw_text_and_unbounded_numeric_lines_are_not_wire_records(self):
        module = receiver()
        schema = module.Schema()
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        for line in fixture["excluded"]:
            with self.subTest(line=repr(line[:60])):
                with self.assertRaises(module.Rejected):
                    schema.line(line)
        # Numeric-only is insufficient: the explicit shared length cap applies.
        with self.assertRaises(module.Rejected):
            schema.line("IOS_STALL schema=1 threshold_ns=50000000 count=1 dropped_busy=0 "
                        "dropped_full=0 fields=kind,thread,start_ns,end_ns,failed,estimated_end spans=" + "1" * 9000)
        codes = re.findall(r'^\s*E\("(?:[^"\\]|\\.)*", ([0-9]+)\)', (PORT / "diagnostic_schema.h").read_text(), re.M)
        self.assertTrue(codes)
        for code in codes:
            schema.line("IOS_DIAG_ERROR code=" + code)

    @unittest.skipUnless(sys.platform == "darwin", "Actual Foundation capture/NSURLSession fixture requires Apple host")
    def test_actual_foundation_capture_and_mocked_transport(self):
        clang = shutil.which("clang")
        self.assertIsNotNone(clang, "Mac validation requires clang")
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="diagnostic-upload-apple-") as tmp:
            root = Path(tmp)
            binary = root / "diagnostic-upload"
            flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-fobjc-arc", "-fblocks"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            # Native source includes the actual production .m; do not compile a
            # second copy or replace parser/transport methods with a model.
            subprocess.run([clang, *flags, "-I", str(PORT),
                            str(ROOT / "tests/native/test_diagnostic_upload.m"),
                            "-framework", "Foundation", "-o", str(binary)],
                           check=True, timeout=90)
            result = subprocess.run([str(binary), str(root), str(FIXTURE)],
                                    capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for message in (
                "pairing exact HTTPS endpoint and token PASS",
                "actual capture allowlist partial UTF8 prior-build settings PASS",
                "read session line byte encode bounds and symlink rejection PASS",
                "actual NSURLSession sender receipt busy immutable retry main-completion PASS",
                "DIAGNOSTIC_UPLOAD actual Foundation and mocked transport PASS",
            ):
                self.assertIn(message, result.stdout)
            raw = (root / "captured-report.json").read_bytes()
            module = receiver()
            bundle = module.Schema().bundle(raw)
            self.assertEqual([s["build"] for s in bundle["sessions"]], [12, 8])
            now, previous = bundle["sessions"]
            for line in fixture["allowed"]:
                self.assertIn(line, now["lines"])
            self.assertEqual(previous["lines"], ["IOS_DIAG_SETTING key=0 value=30",
                                                "IOS_SHADER_OPTION option=1 enabled=1"])
            parsed = module.Schema().timing.summarize(now["lines"])
            self.assertEqual(parsed["invalid_records"], 0)
            self.assertEqual(parsed["kinds"]["frame_gap"]["over_100ms"], 1)
            self.assertEqual(parsed['shader_lead']['observations'], 4)


if __name__ == "__main__":
    unittest.main()
