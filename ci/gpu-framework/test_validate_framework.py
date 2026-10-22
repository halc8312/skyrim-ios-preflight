"""Hand-authored Mach-O metadata fixtures only; no Apple SDK or game input."""
import struct
import unittest

from validate_framework import INSTALL_NAME, exported_names, inspect


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def dylib(command, name):
    encoded = name.encode() + b"\0"
    size = (24 + len(encoded) + 7) & ~7
    return words(command, size, 24, 0, 0, 0) + encoded + bytes(size - 24 - len(encoded))


def trie(name="_wgpuGetVersion", flags=0):
    edge = name.encode() + b"\0"
    return b"\0\1" + edge + bytes([3 + len(edge), 2, flags, 1, 0])


def binary(platform=2, name="_wgpuGetVersion", load="/usr/lib/libSystem.B.dylib", flags=0x84):
    exports = trie(name)
    commands = words(0x32, 24, platform, 26 << 16, 26 << 16, 0) + dylib(0xd, INSTALL_NAME) + dylib(0xc, load)
    commands += words(0x80000033, 16, 32 + len(commands) + 16, len(exports))
    return words(0xFEEDFACF, 0x100000c, 0, 6, 4, len(commands), flags, 0) + commands + exports


class FrameworkMetadataTests(unittest.TestCase):
    def test_actual_export_trie_and_expected_thin_metadata(self):
        result = inspect(binary(), 2, 26 << 16, {"_wgpuGetVersion"})
        self.assertEqual(result["c_api_exports"], ["_wgpuGetVersion"])
        self.assertFalse(result["gpu_runtime_verified"])
        self.assertTrue(result["two_level_namespace"])

    def test_platform_and_minimum_os_reject(self):
        with self.assertRaises(ValueError):
            inspect(binary(), 7, 26 << 16, set())
        with self.assertRaises(ValueError):
            inspect(binary(), 2, 25 << 16, set())
        self.assertEqual(inspect(binary(platform=7), 7, 26 << 16, set())["platform"], 7)

    def test_flat_namespace_private_dependency_and_rust_export_reject(self):
        for value in [binary(flags=4), binary(load="@rpath/Other.dylib"), binary(name="_rust_eh_personality")]:
            with self.assertRaises(ValueError):
                inspect(value, 2, 26 << 16, set())

    def test_truncated_and_invalid_command_bounds_reject(self):
        for value in [b"", binary()[:31], binary()[:-1]]:
            with self.assertRaises(ValueError):
                inspect(value, 2, 26 << 16, set())
        invalid = bytearray(binary())
        invalid[36:40] = words(0)
        with self.assertRaises(ValueError):
            inspect(bytes(invalid), 2, 26 << 16, set())

    def test_missing_symbols_and_unsupported_export_kinds_reject(self):
        with self.assertRaises(ValueError):
            inspect(binary(), 2, 26 << 16, {"_wgpuMissing"})
        for flags in (1, 2, 8, 16):
            with self.assertRaises(ValueError):
                exported_names(trie(flags=flags))

    def test_cyclic_out_of_bounds_and_unterminated_trie_reject(self):
        for data in [b"", b"\0\1x\0\0", b"\0\1x\0\x7f", b"\0\1x", b"\x80"]:
            with self.assertRaises(ValueError):
                exported_names(data)


if __name__ == "__main__":
    unittest.main()
