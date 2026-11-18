import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("steam_inputs", Path(__file__).parents[1] / "scripts" / "steam_inputs.py")
steam = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(steam)


class SteamInputTests(unittest.TestCase):
    def test_vdf_real_shape_escapes_comments_and_legacy(self):
        value = steam.parse_vdf('\ufeff"libraryfolders" { // comment\n "0" {"path" "F:\\\\SteamLibrary" "apps" {"72850" "10"}} "1" "G:\\\\Steam"}')
        self.assertEqual(value["libraryfolders"]["0"]["path"], "F:\\SteamLibrary")
        self.assertEqual(value["libraryfolders"]["1"], "G:\\Steam")

    def test_vdf_rejects_truncation(self):
        for source in ('"a" {', '"a"', '"a" "unterminated', '}', '"a" }'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                steam.parse_vdf(source)

    def test_pe_architecture_and_malformed_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.exe"
            for machine, magic, optional_size, bits, architecture in ((0x14C, 0x10B, 224, 32, "x86"), (0x8664, 0x20B, 240, 64, "x86_64")):
                data = bytearray(512)
                data[:2] = b"MZ"
                struct.pack_into("<I", data, 60, 64)
                data[64:68] = b"PE\0\0"
                struct.pack_into("<HH", data, 68, machine, 1)
                struct.pack_into("<H", data, 84, optional_size)
                struct.pack_into("<H", data, 88, magic)
                path.write_bytes(data)
                result = steam.pe_metadata(path)
                self.assertEqual((result["bits"], result["architecture"]), (bits, architecture))
                struct.pack_into("<I", data, 60, 0xFFFFFFFF)
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    steam.pe_metadata(path)

    def test_manifest_path_traversal_is_reported_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "steamapps").mkdir()
            acf = root / "steamapps" / "appmanifest_72850.acf"
            acf.write_text('"AppState" {"appid" "72850" "installdir" "..\\\\private"}', encoding="utf-8")
            with patch.object(steam, "registry_roots", return_value=[]):
                result = steam.discover([root], fallback=False)
            self.assertEqual(len(result["errors"]), 1)
            self.assertEqual(result["games"][0]["installations"], [])

    def test_output_is_append_only(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "manifest.json"
            output.write_text("preserve", encoding="utf-8")
            with patch.object(steam, "discover", return_value={}):
                with self.assertRaises(FileExistsError):
                    steam.main(["--output", str(output)])
            self.assertEqual(output.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
