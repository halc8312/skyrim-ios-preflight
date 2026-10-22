"""Hand-authored format fixtures only; no executable game code or real signer."""
import copy
import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import signing_binding as binding


def segment(name, vmaddr, vmsize, fileoff, filesize, section=None):
    command = struct.pack("<II16sQQQQiiII", 0x19, 72 + (80 if section else 0), name,
                          vmaddr, vmsize, fileoff, filesize, 7, 5, int(bool(section)), 0)
    if section:
        section_name, address, offset, size = section
        command += struct.pack("<16s16sQQ8I", section_name, name, address, size,
                               offset, 4, 0, 0, 0, 0, 0, 0)
    return command


def unsigned_fixture(platform=2, linkedit_vm=0x4000):
    base = 0x100000000
    identity = b"@rpath/Synthetic.framework/Synthetic\0"
    id_size = binding.align(24 + len(identity), 8)
    dylib = struct.pack("<6I", 0xD, id_size, 24, 0, 0x10000, 0x10000) + identity
    dylib += bytes(id_size - len(dylib))
    commands = [
        segment(b"__TEXT", base, 0x4000, 0, 0x400, (b"__text", base + 0x300, 0x300, 32)),
        segment(b"__DATA", base + 0x4000, 0x4000, 0x400, 0x100,
                (b"__data", base + 0x4000, 0x400, 16)),
        segment(b"__LINKEDIT", base + 0x8000, linkedit_vm, 0x500, 0x31),
        dylib,
        struct.pack("<6I", 0x32, 24, platform, 0x1A0000, 0x1A0500, 0),
        struct.pack("<II16s", 0x1B, 24, b"PUBLIC-FIXTURE!!"),
        struct.pack("<4I", 0x80000033, 16, 0x500, 8),
        struct.pack("<4I", 0x80000034, 16, 0x510, 8),
    ]
    header = struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, 6, len(commands),
                         sum(map(len, commands)), 0x85, 0)
    data = bytearray(0x531)
    data[:32] = header
    data[32:32 + sum(map(len, commands))] = b"".join(commands)
    data[0x300:0x320] = bytes(range(32))  # Deliberately not executable code.
    data[0x400:0x410] = b"PUBLIC_DATA_ONLY"
    data[0x500:0x508] = b"EXPORTS!"
    data[0x510:0x518] = b"FIXUPS!!"
    return bytes(data)


def synthetic_signature(extra=0):
    # Only a structural SuperBlob + CodeDirectory-shaped child. No authentic
    # CMS/CodeDirectory hashes exist and no OS acceptance is claimed.
    child = struct.pack(">II", 0xFADE0C02, 44 + extra) + bytes(36 + extra)
    length = 20 + len(child)
    blob = struct.pack(">5I", 0xFADE0CC0, length, 1, 0, 20) + child
    return blob + bytes(binding.align(len(blob), 16) - len(blob))


def sign_shape(unsigned, extra=0, signature=None):
    manifest = binding.make_manifest(unsigned)
    blob = synthetic_signature(extra) if signature is None else signature
    offset = binding.align(len(unsigned), 16)
    data = bytearray(unsigned + bytes(offset - len(unsigned)) + blob)
    struct.pack_into("<II", data, 16, manifest["ncmds"] + 1, manifest["sizeofcmds"] + 16)
    struct.pack_into("<4I", data, 32 + manifest["sizeofcmds"], 0x1D, 16, offset, len(blob))
    size = len(data) - manifest["linkedit_fileoff"]
    vm = manifest["linkedit_vmsize"] if size <= manifest["linkedit_vmsize"] else binding.align(size, binding.PAGE)
    struct.pack_into("<Q", data, manifest["linkedit_command_offset"] + 32, vm)
    struct.pack_into("<Q", data, manifest["linkedit_command_offset"] + 48, size)
    return bytes(data), manifest


def with_commands(original, additions):
    data = bytearray(original)
    ncmds, size = struct.unpack_from("<II", data, 16)
    extra = b"".join(additions)
    end = 32 + size
    data[end:end + len(extra)] = extra
    struct.pack_into("<II", data, 16, ncmds + len(additions), size + len(extra))
    return bytes(data)


def metadata_fixture():
    original = bytearray(unsigned_fixture())
    original.extend(bytes(0x600 - len(original)))
    link = binding.parse(unsigned_fixture(), signed=False)["linkedit"]["offset"]
    struct.pack_into("<Q", original, link + 48, 0x100)
    symtab = struct.pack("<6I", 0x2, 24, 0x520, 2, 0x540, 16)
    dysymtab = struct.pack("<20I", 0xB, 80, 0, 0, 0, 2, 2, 0,
                           0x550, 1, 0x558, 1, 0x590, 1, 0x594, 1, 0x598, 1, 0x5A0, 1)
    dyld = struct.pack("<12I", 0x80000022, 48, 0x5C0, 8, 0x5C8, 8,
                       0x5D0, 8, 0x5D8, 8, 0x5E0, 8)
    # The first text section's relocation table is also LINKEDIT-backed.
    struct.pack_into("<II", original, 32 + 72 + 56, 0x5B0, 1)
    return with_commands(bytes(original), [symtab, dysymtab, dyld])


class SigningBindingTests(unittest.TestCase):
    def verify(self, data, manifest, expected=None):
        return binding.verify_binding(data, manifest,
                                      expected_unsigned_sha256=expected or manifest["unsigned_sha256"])

    def rejected(self, data, manifest, expected=None):
        with self.assertRaises(binding.Rejected):
            self.verify(bytes(data), manifest, expected)

    def test_roundtrip_device_and_simulator_in_memory_only(self):
        for platform in (2, 7):
            original = unsigned_fixture(platform)
            signed, manifest = sign_shape(original)
            before = (original, signed, copy.deepcopy(manifest))
            result = self.verify(signed, manifest)
            self.assertTrue(result["unsigned_full_sha256_matches"])
            for key in ("os_signature_authenticated", "attestation", "apple_signer_tested", "loader_integration_allowed"):
                self.assertFalse(result[key])
            self.assertEqual((original, signed, manifest), before)

    def test_apple_default_empty_cms_reservation_is_exact_and_zero_only(self):
        # Hand-authored equivalent of the SDK signer's allocation shape, without
        # copying its actual CodeDirectory or any executable bytes.
        cd = struct.pack(">II", 0xFADE0C02, 44) + bytes(36)
        cms = struct.pack(">II", 0xFADE0B01, 8)
        length = 28 + len(cd) + len(cms)
        raw = struct.pack(">7I", 0xFADE0CC0, length, 2, 0, 28, 0x10000, 28 + len(cd)) + cd + cms
        reserved = raw + bytes(binding.align(length, 16) + 18000 - length)
        original = unsigned_fixture()
        signed, manifest = sign_shape(original, signature=reserved)
        self.assertTrue(self.verify(signed, manifest)["unsigned_full_sha256_matches"])
        for delta in (-16, 16):
            changed = reserved[:delta] if delta < 0 else reserved + bytes(delta)
            self.rejected(*sign_shape(original, signature=changed))
        for index in (length, len(reserved) - 1):
            changed = bytearray(reserved)
            changed[index] = 1
            self.rejected(*sign_shape(original, signature=bytes(changed)))
        # Additional reservation must not be granted to an absent/nonempty CMS.
        self.rejected(*sign_shape(original, signature=synthetic_signature() + bytes(18000)))
        changed = bytearray(raw)
        struct.pack_into(">I", changed, 4, length + 1)
        struct.pack_into(">I", changed, 28 + len(cd) + 4, 9)
        changed += b"X"
        changed += bytes(binding.align(len(changed), 16) + 18000 - len(changed))
        self.rejected(*sign_shape(original, signature=bytes(changed)))

    def test_adhoc_reservation_keeps_original_text_linkedit_and_trusted_hash_bound(self):
        cd = struct.pack(">II", 0xFADE0C02, 44) + bytes(36)
        raw = struct.pack(">7I", 0xFADE0CC0, 80, 2, 0, 28, 0x10000, 72) + cd + struct.pack(">II", 0xFADE0B01, 8)
        signed, manifest = sign_shape(unsigned_fixture(), signature=raw + bytes(18000))
        self.verify(signed, manifest)
        for position in (manifest["first_data"], manifest["linkedit_fileoff"]):
            changed = bytearray(signed)
            changed[position] ^= 1
            self.rejected(changed, manifest)
        self.rejected(signed, manifest, "0" * 64)

    def test_linkedit_vm_growth_and_non_page_aligned_original_retention(self):
        signed, manifest = sign_shape(unsigned_fixture(), 0x8000)
        self.assertTrue(self.verify(signed, manifest)["unsigned_full_sha256_matches"])
        signed, manifest = sign_shape(unsigned_fixture(linkedit_vm=512))
        self.assertEqual(struct.unpack_from("<Q", signed, manifest["linkedit_command_offset"] + 32)[0], 512)
        self.verify(signed, manifest)

    def test_every_original_byte_outside_explicit_restoration_fields_is_bound(self):
        original = unsigned_fixture()
        signed, manifest = sign_shape(original)
        mutable = set(range(16, 24))
        mutable.update(range(32 + manifest["sizeofcmds"], 48 + manifest["sizeofcmds"]))
        link = manifest["linkedit_command_offset"]
        mutable.update(range(link + 32, link + 40))
        mutable.update(range(link + 48, link + 56))
        checked = 0
        for position in range(len(original)):
            if position not in mutable:
                damaged = bytearray(signed)
                damaged[position] ^= 1
                with self.subTest(position=position):
                    self.rejected(damaged, manifest)
                checked += 1
        self.assertEqual(checked, len(original) - len(mutable))

    def test_preexisting_signature_unknown_fat_endian_cpu_and_filetypes_refuse(self):
        signed, manifest = sign_shape(unsigned_fixture())
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(signed)
        for field, value in ((0, 0xCAFEBABE), (0, 0xCFFAEDFE), (4, 0x01000007),
                             (8, 2), (12, 2), (28, 1)):
            data = bytearray(unsigned_fixture())
            struct.pack_into("<I", data, field, value)
            with self.subTest(field=field, value=value), self.assertRaises(binding.Rejected):
                binding.make_manifest(bytes(data))
        data = bytearray(unsigned_fixture())
        image = binding.parse(bytes(data), signed=False)
        struct.pack_into("<I", data, image["commands"][-1][1], 0x7FFFFFFF)
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(data))

    def test_nonzero_original_header_padding_and_bad_command_sizes_refuse(self):
        original = bytearray(unsigned_fixture())
        end = 32 + struct.unpack_from("<I", original, 20)[0]
        original[end] = 1
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(original))
        for size in (0, 7, 73, 0xFFFFFFF8):
            damaged = bytearray(unsigned_fixture())
            struct.pack_into("<I", damaged, 36, size)
            with self.assertRaises(binding.Rejected):
                binding.make_manifest(bytes(damaged))

    def test_signature_command_must_be_new_terminal_and_exactly_sized(self):
        signed, manifest = sign_shape(unsigned_fixture())
        command = 32 + manifest["sizeofcmds"]
        for offset, value in ((16, manifest["ncmds"]), (20, manifest["sizeofcmds"]),
                              (command + 4, 24), (command + 8, len(unsigned_fixture()) - 16),
                              (command + 12, 0)):
            damaged = bytearray(signed)
            struct.pack_into("<I", damaged, offset, value)
            self.rejected(damaged, manifest)
        image = binding.parse(signed, signed=True)
        first, second = image["commands"][4:6]
        self.assertEqual(first[2], second[2])
        damaged = bytearray(signed)
        damaged[first[1]:first[1] + first[2]], damaged[second[1]:second[1] + second[2]] = (
            signed[second[1]:second[1] + second[2]], signed[first[1]:first[1] + first[2]])
        self.rejected(damaged, manifest)

    def test_alignment_signature_tail_and_linkedit_changes_are_constrained(self):
        original = unsigned_fixture()
        signed, manifest = sign_shape(original)
        damaged = bytearray(signed)
        damaged[len(original)] = 1
        self.rejected(damaged, manifest)
        self.rejected(signed + b"\0", manifest)
        self.rejected(signed[:-1], manifest)
        link = manifest["linkedit_command_offset"]
        for field, value in ((24, 1), (32, 0x8000), (40, 0), (48, 1)):
            damaged = bytearray(signed)
            struct.pack_into("<Q", damaged, link + field, value)
            self.rejected(damaged, manifest)

    def test_signature_envelope_unknown_slots_overlap_and_trailing_bytes_refuse(self):
        signed, manifest = sign_shape(unsigned_fixture())
        start = binding.align(manifest["unsigned_length"], 16)
        for field, value in ((0, 0), (4, 0xFFFFFFFF), (8, 0), (8, 33),
                             (12, 0xDEAD), (16, 4), (20, 0xDEADBEEF), (24, 0xFFFFFFFF)):
            damaged = bytearray(signed)
            struct.pack_into(">I", damaged, start + field, value)
            self.rejected(damaged, manifest)
        blob = synthetic_signature(extra=1)
        with self.assertRaises(binding.Rejected):
            binding.signature_shape(blob[:-1] + b"\x01")

    def test_signature_body_is_excluded_and_not_authenticated(self):
        signed, manifest = sign_shape(unsigned_fixture())
        changed = bytearray(signed)
        changed[-1] ^= 1  # Inside the opaque CodeDirectory-shaped child body.
        result = self.verify(bytes(changed), manifest)
        self.assertTrue(result["unsigned_full_sha256_matches"])
        self.assertFalse(result["os_signature_authenticated"])

    def test_all_metadata_ranges_stay_in_original_linkedit(self):
        original = metadata_fixture()
        signed, manifest = sign_shape(original)
        self.verify(signed, manifest)
        image = binding.parse(original, signed=False)
        offsets = {command: offset for command, offset, _ in image["commands"]}
        symtab = offsets[0x2]
        dysymtab = offsets[0xB]
        dyld = offsets[0x80000022]
        header_padding = 32 + image["sizeofcmds"]
        ranges = [(symtab + 8, symtab + 12), (symtab + 16, symtab + 20)]
        ranges += [(dysymtab + 8 + pair * 4, dysymtab + 12 + pair * 4)
                   for pair in (6, 8, 10, 12, 14, 16)]
        ranges += [(dyld + 8 + pair * 4, dyld + 12 + pair * 4) for pair in (0, 2, 4, 6, 8)]
        ranges += [(32 + 72 + 56, 32 + 72 + 60)]
        for position, count_position in ranges:
            for bad_offset in (header_padding, 0xFFFFFFFF):
                bad = bytearray(original)
                struct.pack_into("<I", bad, position, bad_offset)
                struct.pack_into("<I", bad, count_position, 1)
                with self.subTest(position=position, bad_offset=bad_offset), self.assertRaises(binding.Rejected):
                    binding.make_manifest(bytes(bad))
        bad = bytearray(original)
        struct.pack_into("<II", bad, dysymtab + 8, 2, 1)
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(bad))

    def test_symbol_table_cannot_alias_zero_signature_command_spare_space(self):
        original = bytearray(unsigned_fixture())
        image = binding.parse(bytes(original), signed=False)
        uuid = next(offset for command, offset, _ in image["commands"] if command == 0x1B)
        padding = 32 + image["sizeofcmds"]
        struct.pack_into("<6I", original, uuid, 0x2, 24, padding, 1, image["first_data"], 1)
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(original))

    def test_file_backed_section_vm_mapping_cannot_alias_header_padding(self):
        original = bytearray(unsigned_fixture())
        image = binding.parse(bytes(original), signed=False)
        section = 32 + 72
        text_vm = image["commands"][0][1] + 24
        vmaddr = struct.unpack_from("<Q", original, text_vm)[0]
        struct.pack_into("<Q", original, section + 32, vmaddr + 32 + image["sizeofcmds"])
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(original))
        original = bytearray(unsigned_fixture())
        original[section + 16:section + 32] = b"__DATA" + bytes(10)
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(original))

    def test_zero_fill_section_cannot_alias_header_padding_or_file_bytes(self):
        original = unsigned_fixture()
        image = binding.parse(original, signed=False)
        first = image["commands"][0]
        vmaddr = image["linkedit"]["vmaddr"] - 0x8000
        new_end = 32 + image["sizeofcmds"] + 80
        for kind in (1, 0xC, 0x12):
            for address, section_offset, accepted in ((vmaddr + new_end, 0, False),
                                                      (vmaddr + 0x400, 1, False),
                                                      (vmaddr + 0x400, 0, True)):
                second = struct.pack("<16s16sQQ8I", b"__bss", b"__TEXT", address, 16,
                                     section_offset, 4, 0, 0, kind, 0, 0, 0)
                text = bytearray(original[first[1]:first[1] + first[2]] + second)
                struct.pack_into("<I", text, 4, len(text))
                struct.pack_into("<I", text, 64, 2)
                commands = bytes(text) + original[first[1] + first[2]:32 + image["sizeofcmds"]]
                data = bytearray(original)
                data[32:32 + len(commands)] = commands
                struct.pack_into("<I", data, 20, len(commands))
                if accepted:
                    signed, manifest = sign_shape(bytes(data))
                    self.verify(signed, manifest)
                else:
                    with self.assertRaises(binding.Rejected):
                        binding.make_manifest(bytes(data))
    def test_dysymtab_requires_symtab(self):
        original = with_commands(unsigned_fixture(), [struct.pack("<20I", 0xB, 80, *([0] * 18))])
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(original)

    def test_sdk_source_version_and_inactive_encryption_range_are_immutable(self):
        source_version = struct.pack("<IIQ", 0x2A, 16, 0)
        encryption = struct.pack("<6I", 0x2C, 24, 0x300, 32, 0, 0)
        original = with_commands(unsigned_fixture(), [source_version, encryption])
        signed, manifest = sign_shape(original)
        self.verify(signed, manifest)
        image = binding.parse(original, signed=False)
        offset = next(pos for command, pos, _ in image["commands"] if command == 0x2C)
        for field, value in ((8, 32 + image["sizeofcmds"]), (12, 0x1000), (16, 1), (20, 1)):
            bad = bytearray(original)
            struct.pack_into("<I", bad, offset + field, value)
            with self.assertRaises(binding.Rejected):
                binding.make_manifest(bytes(bad))
        source_offset = next(pos for command, pos, _ in image["commands"] if command == 0x2A)
        for position in (source_offset + 8, offset + 8):
            bad = bytearray(signed)
            bad[position] ^= 1
            self.rejected(bad, manifest)

    def test_sdk_zero_file_size_segment_retains_vm_bounds(self):
        original = bytearray(unsigned_fixture())
        image = binding.parse(bytes(original), signed=False)
        data = image["commands"][1][1]
        struct.pack_into("<QQ", original, data + 40, 0, 0)
        struct.pack_into("<I", original, data + 72 + 48, 0)
        struct.pack_into("<I", original, data + 72 + 64, 1)
        signed, manifest = sign_shape(bytes(original))
        self.verify(signed, manifest)
        struct.pack_into("<Q", original, data + 24, 0x100001000)
        with self.assertRaises(binding.Rejected):
            binding.make_manifest(bytes(original))

    def test_manifest_duplicate_keys_nonfinite_and_bounded_same_read_refuse(self):
        signed, manifest = sign_shape(unsigned_fixture())
        raw = json.dumps(manifest)
        duplicate = raw.replace('"unsigned_length":', '"unsigned_length": 0, "unsigned_length":', 1)
        with self.assertRaises(binding.Rejected):
            binding.load_manifest(duplicate.encode())
        for raw in (b'{"x":NaN}', b'{"x":Infinity}', b' ' * 16385):
            with self.assertRaises(binding.Rejected):
                binding.load_manifest(raw)
        self.assertEqual(binding.load_manifest(json.dumps(manifest).encode()), manifest)
        with tempfile.TemporaryDirectory(prefix="public-manifest-bound-") as temporary:
            path = Path(temporary) / "oversize.json"
            path.write_bytes(b" " * 16385)
            with self.assertRaises(binding.Rejected):
                binding.read_bounded(path, 16384)

    def test_alternate_code_directory_limit_is_one_past_last(self):
        child = struct.pack(">II", 0xFADE0C02, 44) + bytes(36)
        for alternate, accepted in ((0x1000, True), (0x1004, True), (0x1005, False)):
            length = 28 + len(child) * 2
            blob = (struct.pack(">7I", 0xFADE0CC0, length, 2, 0, 28, alternate, 28 + len(child))
                    + child + child)
            blob += bytes(binding.align(length, 16) - length)
            if accepted:
                binding.signature_shape(blob)
            else:
                with self.assertRaises(binding.Rejected):
                    binding.signature_shape(blob)

    def test_deep_manifest_nesting_emits_only_fixed_cli_rejection(self):
        nested = b"[" * 1100 + b"]" * 1100
        with self.assertRaises(binding.Rejected):
            binding.load_manifest(nested)
        signed, manifest = sign_shape(unsigned_fixture())
        with tempfile.TemporaryDirectory(prefix="public-nested-sidecar-") as temporary:
            root = Path(temporary)
            image, sidecar = root / "synthetic.bin", root / "manifest.json"
            image.write_bytes(signed)
            sidecar.write_bytes(nested)
            output = io.StringIO()
            arguments = ["signing_binding.py", "verify", str(image), str(sidecar),
                         "--expected-unsigned-sha256", manifest["unsigned_sha256"]]
            with patch.object(sys, "argv", arguments), contextlib.redirect_stdout(output):
                self.assertEqual(binding.main(), 2)
            self.assertEqual(json.loads(output.getvalue()),
                             dict(success=False, category="binding_rejected", os_signature_authenticated=False))

    def test_manifest_must_match_separately_trusted_full_sha_and_exact_schema(self):
        signed, manifest = sign_shape(unsigned_fixture())
        self.rejected(signed, manifest, "0" * 64)
        for key, value in (("unsigned_length", True), ("vm_alignment", 4096),
                           ("unsigned_length", 32), ("linkedit_filesize", 1),
                           ("first_data", 0), ("ncmds", 513), ("schema", "future")):
            bad = dict(manifest, **{key: value})
            self.rejected(signed, bad)
        bad = dict(manifest, unsigned_sha256="0" * 64)
        self.rejected(signed, bad, manifest["unsigned_sha256"])
        self.rejected(signed, dict(manifest, unknown=1))

    def test_read_only_file_inputs_and_exclusive_manifest_destination(self):
        original = unsigned_fixture()
        signed, manifest = sign_shape(original)
        with tempfile.TemporaryDirectory(prefix="public-signing-binding-") as temporary:
            root = Path(temporary)
            path = root / "synthetic.bin"
            path.write_bytes(signed)
            before = path.read_bytes()
            self.verify(binding.read_bounded(path), manifest)
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(len(list(root.iterdir())), 1)
            unsigned_path, output = root / "unsigned.bin", root / "manifest.json"
            unsigned_path.write_bytes(original)
            arguments = ["signing_binding.py", "manifest", str(unsigned_path), str(output)]
            with patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(binding.main(), 0)
            saved = output.read_bytes()
            self.assertEqual(json.loads(saved), manifest)
            with patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(binding.main(), 2)
            self.assertEqual(output.read_bytes(), saved)
            self.assertEqual(unsigned_path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
