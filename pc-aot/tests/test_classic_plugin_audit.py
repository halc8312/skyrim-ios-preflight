"""Check the read-only classic DLL inventory using authored PE fixtures."""
from pathlib import Path
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import audit_classic_plugin as audit


def image(wide=False):
    data = bytearray(0x600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    optional = 0x98
    size = 240 if wide else 224
    struct.pack_into("<HH", data, 0x84, 0x8664 if wide else 0x14C, 2)
    struct.pack_into("<HH", data, 0x94, size, 0x2002)
    struct.pack_into("<H", data, optional, 0x20B if wide else 0x10B)
    struct.pack_into("<II", data, optional + 56, 0x3000, 0x200)
    directories = optional + (112 if wide else 96)
    struct.pack_into("<III", data, directories - 4, 16, 0x2000, 0xC0)
    sections = optional + size
    for i, (name, rva, offset, flags) in enumerate(
        [(b".text", 0x1000, 0x200, 0x60000020), (b".rdata", 0x2000, 0x400, 0x40000040)]
    ):
        pos = sections + i * 40
        data[pos:pos + len(name)] = name
        struct.pack_into("<IIII", data, pos + 8, 0x180, rva, 0x200, offset)
        struct.pack_into("<I", data, pos + 36, flags)
    # Sorted export names map to reversed ordinal slots, as a real DLL may do.
    struct.pack_into("<IIIIIII", data, 0x40C, 0x20A0, 1, 2, 2, 0x2028, 0x2030, 0x2038)
    struct.pack_into("<IIIIHH", data, 0x428, 0x1010, 0x1020, 0x2040, 0x2060, 1, 0)
    for offset, value in ((0x440, b"SKSEPlugin_Load"), (0x460, b"SKSEPlugin_Query"),
                          (0x490, b"helper.Entry"), (0x4A0, b"fixture.dll")):
        data[offset:offset + len(value)] = value
    return data


def inspect(data):
    return audit.inspect(io.BytesIO(data), len(data))


def llvm_tool(name):
    found = shutil.which(name)
    if found:
        return found
    if sys.platform == "win32":
        standard = Path("C:/Program Files/LLVM/bin") / (name + ".exe")
        if standard.is_file():
            return str(standard)
    return None


class ClassicPluginAudit(unittest.TestCase):
    def test_i386_and_pe32plus_and_non_dll(self):
        report = inspect(image())
        self.assertEqual((report["format"], report["machine"], report["dll"]), ("PE32", "i386", True))
        self.assertEqual(report["exports"], dict.fromkeys(audit.ENTRY_NAMES, "direct_code"))
        self.assertTrue(report["classic_test_candidate"])
        self.assertEqual(report["iphone_compatibility"], "untested")
        wide = inspect(image(wide=True))
        self.assertEqual((wide["format"], wide["machine"]), ("PE32+", "AMD64"))
        self.assertFalse(wide["classic_test_candidate"])
        non_dll = image()
        struct.pack_into("<H", non_dll, 0x96, 2)
        self.assertFalse(inspect(non_dll)["classic_test_candidate"])

    def test_missing_wrong_case_or_absent_directory(self):
        renamed = image()
        renamed[0x460 + 11] = ord("q")
        report = inspect(renamed)
        self.assertEqual(report["exports"]["SKSEPlugin_Query"], "absent")
        self.assertFalse(report["classic_test_candidate"])
        for offset, size in ((0xF8, 8), (0xF4, 4)):
            with self.subTest(offset=offset):
                empty = image()
                empty[offset:offset + size] = bytes(size)
                self.assertEqual(inspect(empty)["exports"], dict.fromkeys(audit.ENTRY_NAMES, "absent"))

    def test_direct_code_forwarded_data_and_empty_slot(self):
        for target, kind in ((0x2090, "forwarded"), (0x2110, "non_code"), (0, "null")):
            with self.subTest(kind=kind):
                data = image()
                struct.pack_into("<I", data, 0x428, target)
                report = inspect(data)
                self.assertEqual(report["exports"], {"SKSEPlugin_Query": kind, "SKSEPlugin_Load": "direct_code"})
                self.assertFalse(report["classic_test_candidate"])

    def test_rejects_truncation_bad_rvas_limits_and_forwarders(self):
        sample = image()
        for count in (0, 63, 0x90, 0x17F, 0x400, len(sample) - 1):
            with self.subTest(count=count), self.assertRaises(ValueError):
                inspect(sample[:count])
        for offset, fmt, value in (
            (0xF4, "I", 65535), (0xFC, "I", 0x1F0), (0x414, "I", 0xFFFFFFFF),
            (0x420, "I", 0x217E), (0x438, "H", 2), (0x430, "I", 0x2500),
            (0x428, "I", 0x3500), (0x428, "I", 0x20BF),
        ):
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                data = image()
                struct.pack_into("<" + fmt, data, offset, value)
                inspect(data)

    def test_cli_is_read_only_and_rejects_invalid_images(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "authored.dll"
            original = bytes(image())
            path.write_bytes(original)
            command = [sys.executable, str(ROOT / "scripts/audit_classic_plugin.py"), str(path)]
            done = subprocess.run(command, capture_output=True, text=True, check=True)
            self.assertTrue(json.loads(done.stdout)["classic_test_candidate"])
            self.assertEqual(path.read_bytes(), original)
            path.write_bytes(original[:90])
            bad = subprocess.run(command, capture_output=True)
            self.assertEqual(bad.returncode, 2)
            self.assertFalse(bad.stdout)
            self.assertNotIn(b"Traceback", bad.stderr)

    def test_real_authored_i386_dll_if_llvm_installed(self):
        clang, linker = llvm_tool("clang"), llvm_tool("lld-link")
        if not clang or not linker:
            self.skipTest("optional Windows PE fixture requires clang and lld-link")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "test.c").write_text(
                "__declspec(dllexport) int SKSEPlugin_Query(void) { return 3; }\n"
                "__declspec(dllexport) int SKSEPlugin_Load(void) { return 7; }\n", encoding="ascii")
            subprocess.run([clang, "--target=i686-pc-windows-msvc", "-c", "test.c", "-o", "test.obj"],
                           cwd=root, check=True, capture_output=True)
            subprocess.run([linker, "/dll", "/noentry", "/nodefaultlib", "/machine:x86",
                            "/out:test.dll", "test.obj"], cwd=root, check=True, capture_output=True)
            report = audit.audit(root / "test.dll")
            self.assertEqual(report["exports"], dict.fromkeys(audit.ENTRY_NAMES, "direct_code"))
            self.assertTrue(report["classic_test_candidate"])


if __name__ == "__main__":
    unittest.main()
