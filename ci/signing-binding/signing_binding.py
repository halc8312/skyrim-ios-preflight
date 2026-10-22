"""Experimental byte binding only; not signature authentication or a loader.

Supports one deliberately narrow post-signing shape. All input files are read
only. A separately trusted unsigned SHA-256 is mandatory for verification.
No actual Apple/AltServer signer or private framework has been tested here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

MAX_IMAGE = 1 << 30
MAX_SIGNATURE = 16 << 20
PAGE = 16384
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1D
LC_BUILD_VERSION = 0x32
SCHEMA = "strict_unsigned_macho_reconstruction_v1"


class Rejected(ValueError):
    """Sanitized, fixed-category rejection: never includes input bytes/paths."""


def require(condition: bool, category: str) -> None:
    if not condition:
        raise Rejected(category)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align(value: int, unit: int) -> int:
    return (value + unit - 1) & -unit


def bounded(data: bytes, offset: int, size: int, category: str) -> None:
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, category)


def c_name(raw: bytes) -> bytes:
    name, _, tail = raw.partition(b"\0")
    require(bool(name) and not any(tail), "unsupported_segment_name")
    return name


def parse(data: bytes, *, signed: bool) -> dict:
    require(type(data) is bytes and 32 <= len(data) <= MAX_IMAGE, "image_size")
    magic, cpu, subtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from("<8I", data)
    require((magic, cpu, subtype, filetype, reserved) ==
            (MH_MAGIC_64, 0x0100000C, 0, 6, 0), "unsupported_macho_header")
    require(1 <= ncmds <= 512 and 8 <= sizeofcmds <= 65536, "load_command_limits")
    bounded(data, 32, sizeofcmds, "load_command_bounds")
    end = 32 + sizeofcmds
    position = 32
    commands, segments, first_sections = [], [], []
    singleton = set()
    # Narrow contemporary dylib command subset. Unknown/legacy forms refuse.
    fixed = {0x2: 24, 0xB: 80, 0x1B: 24, 0x2A: 16, 0x2C: 24, 0x80000022: 48,
             0x26: 16, 0x29: 16, 0x2E: 16, 0x80000033: 16, 0x80000034: 16}
    link_data = []
    symbol_count = None
    dynamic_symbols = None
    encryption = None
    signature = None
    for index in range(ncmds):
        require(position + 8 <= end, "load_command_bounds")
        command, size = struct.unpack_from("<II", data, position)
        require(size >= 8 and size % 8 == 0 and size <= end - position, "load_command_size")
        commands.append((command, position, size))
        if command == LC_SEGMENT_64:
            require(size >= 72, "segment_size")
            values = struct.unpack_from("<II16sQQQQiiII", data, position)
            _, _, raw_name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, segflags = values
            name = c_name(raw_name)
            require(size == 72 + nsects * 80 and vmsize >= filesize, "segment_size")
            bounded(data, fileoff, filesize, "segment_file_bounds")
            require(vmaddr + vmsize <= (1 << 64), "segment_vm_bounds")
            require(name not in {segment["name"] for segment in segments}, "duplicate_segment")
            segment = dict(name=name, offset=position, vmaddr=vmaddr, vmsize=vmsize,
                           fileoff=fileoff, filesize=filesize, nsects=nsects)
            segments.append(segment)
            for number in range(nsects):
                section = position + 72 + number * 80
                require(c_name(data[section + 16:section + 32]) == name, "section_segment_name")
                address, section_size, section_offset, exponent = struct.unpack_from("<QQII", data, section + 32)
                section_flags = struct.unpack_from("<I", data, section + 64)[0]
                relocation_offset, relocation_count = struct.unpack_from("<II", data, section + 56)
                link_data.append((relocation_offset, relocation_count * 8))
                require(exponent <= 31 and vmaddr <= address and
                        address + section_size <= vmaddr + vmsize, "section_vm_bounds")
                if section_size and section_flags & 0xFF in (1, 0xC, 0x12):
                    require(section_offset == 0 and address >= vmaddr + filesize,
                            "zerofill_must_be_unmapped_segment_tail")
                elif section_size:
                    require(fileoff <= section_offset and section_offset + section_size <= fileoff + filesize,
                            "section_file_bounds")
                    require(address - vmaddr == section_offset - fileoff, "section_file_vm_mapping")
                    if name == b"__TEXT":
                        first_sections.append(section_offset)
        elif command == LC_CODE_SIGNATURE:
            require(signed and signature is None and index == ncmds - 1 and size == 16,
                    "signature_command_shape")
            signature = dict(command_offset=position, dataoff=struct.unpack_from("<I", data, position + 8)[0],
                             datasize=struct.unpack_from("<I", data, position + 12)[0])
        elif command == LC_BUILD_VERSION:
            require(command not in singleton and size >= 24, "build_version_shape")
            singleton.add(command)
            platform, minos, sdk, ntools = struct.unpack_from("<4I", data, position + 8)
            require(platform in (2, 7) and size == 24 + ntools * 8, "unsupported_platform")
        elif command in (0xC, 0xD):  # LC_LOAD_DYLIB / LC_ID_DYLIB
            require(size >= 32, "dylib_command_shape")
            name_offset = struct.unpack_from("<I", data, position + 8)[0]
            require(24 <= name_offset < size and b"\0" in data[position + name_offset:position + size],
                    "dylib_name_bounds")
            if command == 0xD:
                require(command not in singleton, "duplicate_load_command")
                singleton.add(command)
        elif command in fixed:
            require(size == fixed[command] and command not in singleton, "fixed_load_command_shape")
            singleton.add(command)
            if command in (0x26, 0x29, 0x2E, 0x80000033, 0x80000034):
                link_data.append(struct.unpack_from("<II", data, position + 8))
            elif command == 0x2:
                symoff, symbol_count, stroff, strsize = struct.unpack_from("<4I", data, position + 8)
                link_data.extend(((symoff, symbol_count * 16), (stroff, strsize)))
            elif command == 0xB:
                dynamic_symbols = struct.unpack_from("<18I", data, position + 8)
                # dylib_table_of_contents / dylib_module_64 / dylib_reference /
                # indirect symbols / external and local relocation_info.
                for pair, entry_size in ((6, 8), (8, 56), (10, 4), (12, 4), (14, 8), (16, 8)):
                    link_data.append((dynamic_symbols[pair], dynamic_symbols[pair + 1] * entry_size))
            elif command == 0x80000022:
                dyld_info = struct.unpack_from("<10I", data, position + 8)
                link_data.extend((dyld_info[pair], dyld_info[pair + 1]) for pair in range(0, 10, 2))
            elif command == 0x2C:
                cryptoff, cryptsize, cryptid, pad = struct.unpack_from("<4I", data, position + 8)
                require(cryptid == 0 and pad == 0, "encrypted_image_unsupported")
                encryption = (cryptoff, cryptsize)
        else:
            raise Rejected("unrecognized_load_command")
        position += size
    require(position == end, "load_command_count")
    require(LC_BUILD_VERSION in singleton and 0xD in singleton, "required_load_command")
    require(bool(segments) and segments[0]["name"] == b"__TEXT" and
            segments[-1]["name"] == b"__LINKEDIT", "segment_order")
    text, linkedit = segments[0], segments[-1]
    require(text["fileoff"] == 0 and bool(first_sections), "text_header_layout")
    first_data = min(first_sections)
    require(end <= first_data <= text["filesize"], "header_padding_bounds")
    require(not any(data[end:first_data]), "nonzero_header_padding")
    if encryption is not None:
        cryptoff, cryptsize = encryption
        require((cryptsize == 0 and cryptoff == 0) or
                (cryptsize > 0 and first_data <= cryptoff and cryptoff + cryptsize <= text["filesize"]),
                "unencrypted_range_must_be_text_payload")
    require(linkedit["nsects"] == 0 and linkedit["fileoff"] > 0 and
            linkedit["fileoff"] + linkedit["filesize"] == len(data), "linkedit_not_terminal")
    for left, right in zip(segments, segments[1:]):
        require(left["vmaddr"] + left["vmsize"] <= right["vmaddr"], "segment_overlap_or_order")
    # SDK zero-fill-only __DATA can have fileoff=filesize=0. Its empty file
    # interval does not overlap __TEXT; its VM extent is still checked above.
    file_segments = [segment for segment in segments if segment["filesize"]]
    for left, right in zip(file_segments, file_segments[1:]):
        require(left["fileoff"] + left["filesize"] <= right["fileoff"], "segment_overlap_or_order")
    payload_end = signature["dataoff"] if signature is not None else len(data)
    for offset, size in link_data:
        if size:
            require(linkedit["fileoff"] <= offset and offset + size <= payload_end, "linkedit_data_bounds")
    if dynamic_symbols is not None:
        require(symbol_count is not None, "dysymtab_without_symtab")
        for pair in (0, 2, 4):
            require(dynamic_symbols[pair] + dynamic_symbols[pair + 1] <= symbol_count,
                    "dysymtab_symbol_bounds")
    require((signature is not None) == signed, "signature_presence")
    return dict(ncmds=ncmds, sizeofcmds=sizeofcmds, first_data=first_data,
                linkedit=linkedit, signature=signature, commands=commands)


def make_manifest(unsigned: bytes) -> dict:
    image = parse(unsigned, signed=False)
    require(image["first_data"] >= 32 + image["sizeofcmds"] + 16, "insufficient_zero_header_padding")
    linkedit = image["linkedit"]
    return dict(schema=SCHEMA, unsigned_sha256=sha(unsigned), unsigned_length=len(unsigned),
                ncmds=image["ncmds"], sizeofcmds=image["sizeofcmds"],
                first_data=image["first_data"], linkedit_command_offset=linkedit["offset"],
                linkedit_fileoff=linkedit["fileoff"], linkedit_filesize=linkedit["filesize"],
                linkedit_vmsize=linkedit["vmsize"], vm_alignment=PAGE)


def validate_manifest(manifest: dict, expected_unsigned_sha256: str) -> None:
    fields = {"schema", "unsigned_sha256", "unsigned_length", "ncmds", "sizeofcmds", "first_data",
              "linkedit_command_offset", "linkedit_fileoff", "linkedit_filesize", "linkedit_vmsize", "vm_alignment"}
    require(type(manifest) is dict and set(manifest) == fields, "manifest_schema")
    require(type(expected_unsigned_sha256) is str and len(expected_unsigned_sha256) == 64 and
            all(c in "0123456789abcdef" for c in expected_unsigned_sha256), "trusted_unsigned_sha256")
    require(manifest["schema"] == SCHEMA and manifest["unsigned_sha256"] == expected_unsigned_sha256,
            "manifest_expected_sha256")
    for key in fields - {"schema", "unsigned_sha256"}:
        require(type(manifest[key]) is int and 0 <= manifest[key] <= MAX_IMAGE, "manifest_integer")
    require(32 <= manifest["unsigned_length"] <= MAX_IMAGE and manifest["vm_alignment"] == PAGE,
            "manifest_limits")
    end = 32 + manifest["sizeofcmds"]
    require(1 <= manifest["ncmds"] <= 512 and 8 <= manifest["sizeofcmds"] <= 65536 and
            end + 16 <= manifest["first_data"] <= manifest["linkedit_fileoff"] and
            32 <= manifest["linkedit_command_offset"] <= end - 72 and
            manifest["linkedit_fileoff"] + manifest["linkedit_filesize"] == manifest["unsigned_length"] and
            manifest["linkedit_vmsize"] >= manifest["linkedit_filesize"], "manifest_layout")


def signature_shape(blob: bytes) -> None:
    # Structural envelope only; no CodeDirectory hashes, CMS, signer, entitlement
    # policy, certificate trust or OS acceptance are authenticated by this tool.
    require(16 <= len(blob) <= MAX_SIGNATURE and len(blob) % 16 == 0, "signature_blob_size")
    magic, length, count = struct.unpack_from(">III", blob)
    require(magic == 0xFADE0CC0 and 1 <= count <= 32 and 12 + 8 * count <= length <= len(blob)
            and align(length, 16) == len(blob) and not any(blob[length:]), "signature_superblob")
    intervals, slots = [], set()
    known = {0: 0xFADE0C02, 2: 0xFADE0C01, 5: 0xFADE7171,
             7: 0xFADE7172, 0x10000: 0xFADE0B01}
    for number in range(count):
        slot, offset = struct.unpack_from(">II", blob, 12 + 8 * number)
        # CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT is one past the last slot.
        expected_magic = 0xFADE0C02 if 0x1000 <= slot < 0x1005 else known.get(slot)
        require(expected_magic is not None and slot not in slots and 12 + 8 * count <= offset <= length - 8,
                "signature_index")
        child_magic, child_length = struct.unpack_from(">II", blob, offset)
        require(child_magic == expected_magic and 8 <= child_length <= length - offset, "signature_child")
        slots.add(slot)
        intervals.append((offset, offset + child_length))
    require(0 in slots, "signature_primary_code_directory")
    intervals.sort()
    cursor = 12 + 8 * count
    for start, end in intervals:
        require(start >= cursor and not any(blob[cursor:start]), "signature_overlap_or_gap")
        cursor = end
    require(not any(blob[cursor:length]), "signature_trailing_data")


def verify_binding(signed: bytes, manifest: dict, *, expected_unsigned_sha256: str) -> dict:
    validate_manifest(manifest, expected_unsigned_sha256)
    image = parse(signed, signed=True)
    sig, linkedit = image["signature"], image["linkedit"]
    original_length = manifest["unsigned_length"]
    require(image["ncmds"] == manifest["ncmds"] + 1 and
            image["sizeofcmds"] == manifest["sizeofcmds"] + 16 and
            sig["command_offset"] == 32 + manifest["sizeofcmds"] and
            image["first_data"] == manifest["first_data"], "appended_signature_command_only")
    require(linkedit["offset"] == manifest["linkedit_command_offset"] and
            linkedit["fileoff"] == manifest["linkedit_fileoff"], "linkedit_identity")
    require(sig["dataoff"] == align(original_length, 16) and
            0 < sig["datasize"] <= MAX_SIGNATURE and
            sig["dataoff"] + sig["datasize"] == len(signed) and
            original_length <= sig["dataoff"] and not any(signed[original_length:sig["dataoff"]]),
            "signature_append_only")
    expected_vm = (manifest["linkedit_vmsize"] if linkedit["filesize"] <= manifest["linkedit_vmsize"]
                   else align(linkedit["filesize"], PAGE))
    require(linkedit["filesize"] == len(signed) - linkedit["fileoff"] and
            linkedit["vmsize"] == expected_vm,
            "linkedit_size_policy")
    signature_shape(signed[sig["dataoff"]:])
    restored = bytearray(signed[:original_length])
    struct.pack_into("<II", restored, 16, manifest["ncmds"], manifest["sizeofcmds"])
    restored[sig["command_offset"]:sig["command_offset"] + 16] = b"\0" * 16
    struct.pack_into("<Q", restored, linkedit["offset"] + 32, manifest["linkedit_vmsize"])
    struct.pack_into("<Q", restored, linkedit["offset"] + 48, manifest["linkedit_filesize"])
    original = bytes(restored)
    require(sha(original) == expected_unsigned_sha256, "unsigned_full_sha256_mismatch")
    require(make_manifest(original) == manifest, "restored_manifest_mismatch")
    return dict(unsigned_full_sha256_matches=True, original_byte_count=original_length,
                reconstructed_sha256=expected_unsigned_sha256, signature_bytes=sig["datasize"],
                os_signature_authenticated=False, attestation=False, apple_signer_tested=False,
                loader_integration_allowed=False)


def read_bounded(path: Path, limit: int = MAX_IMAGE) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "input_limit")
    return data


def load_manifest(data: bytes) -> dict:
    require(len(data) <= 16384, "manifest_size")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate_manifest_key")
            result[key] = value
        return result
    def nonfinite(_):
        raise Rejected("nonfinite_manifest_number")
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique, parse_constant=nonfinite)
    except RecursionError:
        raise Rejected("manifest_nesting_limit") from None
    require(type(value) is dict, "manifest_object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    make = modes.add_parser("manifest")
    make.add_argument("unsigned", type=Path)
    make.add_argument("output", type=Path)
    verify = modes.add_parser("verify")
    verify.add_argument("signed", type=Path)
    verify.add_argument("manifest", type=Path)
    verify.add_argument("--expected-unsigned-sha256", required=True)
    args = parser.parse_args()
    try:
        if args.mode == "manifest":
            value = make_manifest(read_bounded(args.unsigned))
            with args.output.open("x", encoding="utf-8", newline="\n") as output:
                json.dump(value, output, indent=2)
                output.write("\n")
            result = dict(manifest_created=True, unsigned_sha256=value["unsigned_sha256"])
        else:
            value = load_manifest(read_bounded(args.manifest, 16384))
            result = verify_binding(read_bounded(args.signed), value,
                                    expected_unsigned_sha256=args.expected_unsigned_sha256)
        print(json.dumps(dict(success=True, **result), sort_keys=True))
        return 0
    except (Rejected, OSError, ValueError, struct.error):
        # No arbitrary exception text or input paths can quote private content.
        print(json.dumps(dict(success=False, category="binding_rejected", os_signature_authenticated=False)))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
