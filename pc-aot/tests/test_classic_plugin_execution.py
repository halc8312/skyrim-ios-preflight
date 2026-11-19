"""Exercise the closed Windows / statically translated Classic plugin comparison."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("fixture_runner", ROOT / "experiments/classic-plugin/run.py")
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)
SER_SPEC = importlib.util.spec_from_file_location("serial_fixture_runner", Path(SPEC.origin).with_name("run_serialization.py"))
serial = importlib.util.module_from_spec(SER_SPEC)
with mock.patch.dict(sys.modules, {"run": fixture}):
    SER_SPEC.loader.exec_module(serial)


class ClosedInput(unittest.TestCase):
    def test_altered_foreign_owner_and_wrong_plugin_state_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "authored.skse"
            data = serial.expected_bytes(16)
            source.write_bytes(data)
            self.assertEqual(serial.checked_file(source, 16), data)
            for bad in (data.replace(b"opaque", b"differ"), data[:-1], serial.expected_bytes(21)):
                source.write_bytes(bad)
                with self.assertRaises(ValueError):
                    serial.checked_file(source, 16)

    def test_substituted_dll_rejected_before_creating_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / fixture.DLL_NAME).write_bytes(b"MZ other input")
            with self.assertRaisesRegex(ValueError, "authored Classic test DLL"):
                fixture.compare(root, root, root / "results")
            self.assertFalse((root / "results").exists())

    def test_missing_metadata_wrong_callback_and_missing_case_are_detected(self):
        valid = "\n".join(fixture.ROWS).encode()
        self.assertEqual(fixture.rows(valid), fixture.ROWS)
        for changed in (valid.replace(b"ClassicProbe", b"-", 1),
                        valid.replace(b"ClassicProbe 1 0", b"ClassicProbe 1 1", 1), valid.split(b"\n", 1)[1]):
            with self.subTest(output=changed), self.assertRaises(ValueError):
                fixture.rows(changed)

    def test_apple_check_distinguishes_device_from_simulator_and_wrong_cpu(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.o"
            header = struct.pack("<8I", 0xfeedfacf, 0x100000c, 0, 1, 1, 24, 0, 0)
            device = header + struct.pack("<6I", 0x32, 24, 2, 18 << 16, 0, 0)
            path.write_bytes(device)
            fixture.verify_ios_object(path)
            for bad in (device[:31], device[:-1], device[:40] + struct.pack("<I", 7) + device[44:],
                        device[:4] + struct.pack("<I", 0x1000007) + device[8:]):
                path.write_bytes(bad)
                with self.assertRaises(ValueError):
                    fixture.verify_ios_object(path)


class Execution(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "win32", "native Windows i386 loader required")
    def test_native_serialization_save_and_returned_aot_file(self):
        llvm = Path(os.environ.get("CLASSIC_PLUGIN_LLVM_BIN", "C:/Program Files/LLVM/bin")).resolve()
        if not all((llvm / tool).is_file() for tool in ("clang.exe", "lld-link.exe")):
            self.skipTest("Windows LLVM with i386 MSVC toolchain required")
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder) / "native"
            self.assertEqual(serial.windows(work, llvm)["windows_save"], "11 0 16 0 0")
            if exchange := os.environ.get("CLASSIC_SERIAL_EXCHANGE"):
                result = serial.finish(work, Path(exchange).resolve(), Path(folder) / "return", llvm)
                self.assertEqual(result["windows_restore_and_save"], "99 21 26 2 1")
                self.assertTrue(result["foreign_section_preserved"])

    def test_translated_serialization_callbacks_restore_the_windows_record(self):
        if not os.environ.get("CLASSIC_SERIAL_WINDOWS_WORK") or not os.environ.get("CLASSIC_PLUGIN_SKYDREAM"):
            self.skipTest("set CLASSIC_SERIAL_WINDOWS_WORK and CLASSIC_PLUGIN_SKYDREAM for co-save exchange")
        with tempfile.TemporaryDirectory() as folder:
            headers = os.environ.get("CLASSIC_PLUGIN_DARWIN_HEADERS")
            result = serial.exchange(Path(os.environ["CLASSIC_SERIAL_WINDOWS_WORK"]).resolve(),
                                     Path(os.environ["CLASSIC_PLUGIN_SKYDREAM"]).resolve(), Path(folder) / "aot",
                                     os.environ.get("CC", "clang"), Path(headers) if headers else None)
            self.assertEqual(result["translated_save"], "77 16 21 2 1")
            self.assertEqual(len(result["ios_device_objects"]), 6 if headers else 0)

    @unittest.skipUnless(sys.platform == "win32", "native Windows i386 loader required")
    def test_original_authored_plugin_on_windows(self):
        llvm = Path(os.environ.get("CLASSIC_PLUGIN_LLVM_BIN", "C:/Program Files/LLVM/bin")).resolve()
        if not all((llvm / tool).is_file() for tool in ("clang.exe", "lld-link.exe")):
            self.skipTest("Windows LLVM with i386 MSVC toolchain required")
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder) / "native"
            observed = fixture.windows(work, llvm)
            self.assertEqual(observed["dll_sha256"], fixture.FIXTURE_SHA256)
            self.assertEqual(len(observed["rows"]), 10)
            self.assertEqual(json.loads((work / "windows.json").read_text()), observed)

    def test_same_windows_dll_after_static_translation(self):
        if not os.environ.get("CLASSIC_PLUGIN_WINDOWS_WORK") or not os.environ.get("CLASSIC_PLUGIN_SKYDREAM"):
            self.skipTest("set CLASSIC_PLUGIN_WINDOWS_WORK and CLASSIC_PLUGIN_SKYDREAM for translation")
        work = Path(os.environ["CLASSIC_PLUGIN_WINDOWS_WORK"]).resolve()
        skydream = Path(os.environ["CLASSIC_PLUGIN_SKYDREAM"]).resolve()
        headers = os.environ.get("CLASSIC_PLUGIN_DARWIN_HEADERS")
        with tempfile.TemporaryDirectory() as folder:
            result = fixture.compare(work, skydream, Path(folder) / "aot",
                                     os.environ.get("CC", "clang"), Path(headers) if headers else None)
            self.assertEqual(result["matching_cases"], 10)
            self.assertEqual(result["dll_sha256"], fixture.FIXTURE_SHA256)
            self.assertEqual(len(result["ios_device_objects"]), 2 if headers else 0)
            self.assertFalse(result["iphone_execution_verified"])


if __name__ == "__main__":
    unittest.main()
