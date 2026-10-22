import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("public_fixture_validator", Path(__file__).with_name("validate-owned-host-fixture.py"))
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def string_command(kind, text):
    payload = text.encode() + b"\0"
    size = (24 + len(payload) + 7) & ~7
    return words(kind, size, 24, 0, 0x150000, 0x10000) + payload + bytes(size - 24 - len(payload))


def segment(name, vm, fileoff, filesize, protection, section=False):
    size = 152 if section else 72
    command = words(0x19, size) + name.encode().ljust(16, b"\0")
    command += struct.pack("<4Q4I", vm, 0x1000, fileoff, filesize, 7, protection, int(section), 0)
    if section:
        command += b"__text".ljust(16, b"\0") + name.encode().ljust(16, b"\0")
        command += struct.pack("<2Q8I", vm + 0x400, 16, 0x400, 2, 0, 0, 0x80000400, 0, 0, 0)
    return command


def fixture(kind=6, platform=2, extras=(), load_kind=0xC, system=gate.SYSTEM):
    commands = [segment("__TEXT", 0x100000000, 0, 0x800, 5, True),
                segment("__LINKEDIT", 0x100001000, 0x800, 0x100, 1),
                words(0x32, 24, platform, gate.MIN_OS, gate.SDK, 0),
                words(0x1B, 24) + bytes(range(16)),
                string_command(load_kind, system),
                words(0x2, 24, 0x800, 1, 0x810, 8),
                words(0xB, 80, 0, 0, 0, 1, 1, 0, *([0] * 12))]
    if kind == 6:
        commands.append(string_command(0xD, "@rpath/PrivateGuest.framework/PrivateGuest"))
    else:
        path = b"/usr/lib/dyld\0"
        commands.append(words(0xE, 32, 12) + path + bytes(20 - len(path)))
        commands.append(words(0x80000028, 24) + struct.pack("<2Q", 0x400, 0))
    commands.extend(extras)
    data = bytearray(0x900)
    header = words(0xFEEDFACF, 0x100000C, 0, kind, len(commands), sum(map(len, commands)), 0x81, 0)
    data[:32] = header
    at = 32
    for command in commands:
        data[at:at + len(command)] = command
        at += len(command)
    assert at < 0x400
    data[0x400:0x410] = bytes(range(16))
    data[0x800:0x810] = struct.pack("<IBBHQ", 1, 0xF, 1, 0, 0x100000400)
    data[0x810:0x818] = b"\0_fn\0\0\0\0"
    return data


def command_at(data, wanted):
    at = 32
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        kind, size = struct.unpack_from("<2I", data, at)
        if kind == wanted:
            return at
        at += size
    raise AssertionError("self-authored command absent")


class PublicFixtureMetadataTests(unittest.TestCase):
    def verify(self, data, probe=None, platform=2):
        return gate.verify(data, fixture(kind=2, platform=platform) if probe is None else probe, platform)

    def test_device_and_simulator_exact_sdk_versions_with_false_execution_flags(self):
        for platform in [2, 7]:
            report = self.verify(fixture(platform=platform), platform=platform)
            self.assertTrue(report["sdk_system_versions_match"])
            self.assertEqual(report["raw_undefined_symbols"], 0)
            self.assertFalse(report["game_executed"])
            self.assertFalse(report["private_framework_verified"])
            changed = fixture(platform=platform)
            struct.pack_into("<I", changed, command_at(changed, 0xC) + 16, 0x10000)
            with self.assertRaisesRegex(gate.Rejected, "versions"):
                self.verify(changed, platform=platform)

    def test_header_command_and_unknown_shape_fail_closed(self):
        valid = fixture()
        for size in [0, 31, 80]:
            with self.assertRaises(gate.Rejected):
                self.verify(valid[:size])
        for offset, value in [(4, 0x1000007), (8, 2), (12, 2), (16, 0xFFFFFFFF), (36, 0)]:
            changed = valid.copy()
            struct.pack_into("<I", changed, offset, value)
            with self.assertRaises(gate.Rejected):
                self.verify(changed)
        with self.assertRaisesRegex(gate.Rejected, "unrecognized"):
            self.verify(fixture(extras=[words(0x12345678, 8)]))

    def test_platform_minimum_sdk_and_writable_code_refuse(self):
        for field, value in [(8, 7), (12, gate.MIN_OS + 0x100), (16, gate.SDK + 0x100)]:
            changed = fixture()
            struct.pack_into("<I", changed, command_at(changed, 0x32) + field, value)
            with self.assertRaises(gate.Rejected):
                self.verify(changed)
        changed = fixture()
        struct.pack_into("<I", changed, 32 + 60, 7)
        with self.assertRaises(gate.Rejected):
            self.verify(changed)

    def test_strong_system_only_and_signed_command_even_zero_size_refuse(self):
        for kind in [0x80000018, 0x8000001F, 0x80000023, 0x20]:
            with self.assertRaises(gate.Rejected):
                self.verify(fixture(load_kind=kind))
        with self.assertRaises(gate.Rejected):
            self.verify(fixture(system="/usr/lib/libOther.dylib"))
        with self.assertRaises(gate.Rejected):
            self.verify(fixture(extras=[string_command(0xC, gate.SYSTEM)]))
        with self.assertRaisesRegex(gate.Rejected, "code_signature"):
            self.verify(fixture(extras=[words(0x1D, 16, 0, 0)]))
        # The actual ordinary SDK link probe may be signed; fixture may not.
        self.verify(fixture(), probe=fixture(kind=2, extras=[words(0x1D, 16, 0, 0)]))

    def test_raw_undefined_cannot_hide_behind_a_zero_dysymtab_count(self):
        for value, descriptor in [(0, 0), (0, 0x40), (8, 0)]:
            changed = fixture()
            struct.pack_into("<IBBHQ", changed, 0x800, 1, 1, 0, descriptor, value)
            with self.assertRaisesRegex(gate.Rejected, "undefined"):
                self.verify(changed)
        changed = fixture()
        changed[0x804] = 0xB  # N_INDR alias is not silently a definition.
        with self.assertRaisesRegex(gate.Rejected, "indirect"):
            self.verify(changed)

    def test_bind_and_chained_imports_are_independently_rejected(self):
        for index in [3, 5, 7]:
            values = [0] * 10
            values[index - 1:index + 1] = [0x830, 1]
            with self.assertRaisesRegex(gate.Rejected, "dynamic_imports"):
                self.verify(fixture(extras=[words(0x80000022, 48, *values)]))
        chain = words(0x80000034, 16, 0x840, 48)
        valid = fixture(extras=[chain])
        valid[0x840:0x85C] = words(0, 28, 32, 48, 0, 1, 0)
        self.verify(valid)
        imported = valid.copy()
        struct.pack_into("<I", imported, 0x840 + 16, 1)
        with self.assertRaisesRegex(gate.Rejected, "dynamic_imports"):
            self.verify(imported)
        changed = valid.copy()
        struct.pack_into("<I", changed, 0x840 + 20, 99)
        with self.assertRaisesRegex(gate.Rejected, "chained_header"):
            self.verify(changed)

    def test_metadata_header_aliases_strings_and_symbol_indices_refuse(self):
        for field, value in [(8, 32), (12, 0xFFFFFFFF), (16, 0x8FF), (20, 256)]:
            changed = fixture()
            struct.pack_into("<I", changed, command_at(changed, 0x2) + field, value)
            with self.assertRaises(gate.Rejected):
                self.verify(changed)
        changed = fixture()
        struct.pack_into("<I", changed, 0x800, 8)
        with self.assertRaisesRegex(gate.Rejected, "symbol_string"):
            self.verify(changed)
        changed = fixture()
        struct.pack_into("<I", changed, command_at(changed, 0xB) + 28, 2)
        with self.assertRaises(gate.Rejected):
            self.verify(changed)

    def test_cli_reads_only_bounded_regular_inputs_and_prints_fixed_categories(self):
        with tempfile.TemporaryDirectory(prefix="public-sdk-fixture-gate-") as temporary:
            root = Path(temporary)
            candidate, probe = root / "fixture", root / "sdk-probe"
            candidate.write_bytes(fixture())
            probe.write_bytes(fixture(kind=2))
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = gate.main(["--fixture", str(candidate), "--sdk-probe", str(probe), "--platform", "2"])
            self.assertEqual(status, 0)
            self.assertTrue(json.loads(stdout.getvalue())["public_fixture_metadata_verified"])
            candidate.write_bytes(b"bad")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = gate.main(["--fixture", str(candidate), "--sdk-probe", str(probe), "--platform", "2"])
            self.assertEqual(status, 2)
            self.assertNotIn(str(root), stdout.getvalue())
            self.assertFalse(json.loads(stdout.getvalue())["public_fixture_metadata_verified"])


if __name__ == "__main__":
    unittest.main()
