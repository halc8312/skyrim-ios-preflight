#!/usr/bin/env python3
"""Inspect only a self-authored unsigned SDK fixture and tiny SDK link probe.

No image is loaded/executed, no SDK implementation is copied, and no signature
authentication or private-framework compatibility follows from this check.
The builder separately checks the exact seven exports using Apple's SDK nm.
"""
import argparse
import json
import stat
import struct
from pathlib import Path

MAX_BYTES = 16 * 1024 * 1024
SYSTEM = "/usr/lib/libSystem.B.dylib"
MIN_OS = 0x001A0000
SDK = 0x001A0500
LOADS = {0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20}
LINK_DATA = {0x1E, 0x26, 0x29, 0x2B, 0x2E, 0x80000033, 0x80000034}


class Rejected(ValueError):
    pass


def need(condition, category):
    if not condition:
        raise Rejected(category)


def region(data, offset, size):
    need(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, "file_bounds")
    return data[offset:offset + size]


def unpack(data, offset, format):
    return struct.unpack(format, region(data, offset, struct.calcsize(format)))


def name(data):
    head, separator, tail = data.partition(b"\0")
    need(head and (not separator or not any(tail)) and all(0x21 <= x <= 0x7e for x in head),
         "segment_name")
    return head.decode("ascii")


def command_string(command, minimum=24):
    offset, = unpack(command, 8, "<I")
    need(minimum <= offset < len(command), "command_string_bounds")
    end = command.find(b"\0", offset)
    need(end >= offset and not any(command[end:]), "command_string_terminator")
    try:
        return command[offset:end].decode("utf-8")
    except UnicodeError:
        raise Rejected("command_string_encoding") from None


def inspect(data, kind, platform):
    need(32 <= len(data) <= MAX_BYTES, "image_size")
    magic, cpu, subtype, filetype, count, command_bytes, flags, reserved = unpack(data, 0, "<8I")
    need((magic, cpu, subtype, filetype, reserved) == (0xFEEDFACF, 0x100000C, 0, kind, 0),
         "plain_arm64_image_kind")
    need(1 <= count <= 512 and 8 <= command_bytes <= 65536, "command_limits")
    region(data, 32, command_bytes)
    end, offset = 32 + command_bytes, 32
    seen, builds, segments, dylibs, metadata = set(), [], [], [], []
    binds, chained, signatures, rebases = [], [], 0, []
    symtab = dysymtab = None
    identifiers, dynamic_linkers, entrypoints, uuids = [], [], 0, 0
    for _ in range(count):
        need(offset + 8 <= end, "command_bounds")
        command, size = unpack(data, offset, "<2I")
        need(size >= 8 and size % 8 == 0 and size <= end - offset, "command_shape")
        c = region(data, offset, size)
        if command not in LOADS and command not in {0x19, 0x8000001C}:
            need(command not in seen, "duplicate_command")
            seen.add(command)
        if command == 0x19:
            need(size >= 72, "segment_shape")
            segment_name = name(c[8:24])
            vm, vm_size, fileoff, filesize, maxprot, initprot, nsects, segment_flags = unpack(c, 24, "<4Q4I")
            need(size == 72 + 80 * nsects and vm + vm_size <= 1 << 64 and vm_size >= filesize,
                 "segment_shape")
            region(data, fileoff, filesize)
            need(maxprot & ~7 == 0 and initprot & ~maxprot == 0 and initprot & 6 != 6,
                 "segment_protections")
            need(segment_flags & ~0x10 == 0 and all(s[0] != segment_name for s in segments),
                 "segment_flags_or_duplicate")
            for i in range(nsects):
                s = c[72 + i * 80:152 + i * 80]
                need(name(s[16:32]) == segment_name, "section_segment")
                address, length, location, exponent, reloff, nreloc, section_flags = unpack(s, 32, "<2Q5I")
                need(exponent <= 31 and vm <= address and address + length <= vm + vm_size,
                     "section_vm_bounds")
                if length and section_flags & 0xFF not in {1, 0xC, 0x12}:
                    need(fileoff <= location and location + length <= fileoff + filesize
                         and address - vm == location - fileoff, "section_file_bounds")
                if nreloc:
                    metadata.append((reloff, nreloc * 8))
            segments.append((segment_name, vm, vm_size, fileoff, filesize, nsects, initprot))
        elif command == 0x32:
            need(size >= 24, "build_version_shape")
            target, minimum, sdk, tools = unpack(c, 8, "<4I")
            need(size == 24 + tools * 8 and tools <= 32, "build_version_shape")
            builds.append((target, minimum, sdk))
        elif command in LOADS or command == 0xD:
            need(size >= 24, "dylib_command_shape")
            path = command_string(c)
            current, compatible = unpack(c, 16, "<2I")
            if command == 0xD:
                identifiers.append(path)
            else:
                dylibs.append((command, path, current, compatible))
        elif command == 0xE:
            need(size >= 16, "dynamic_linker_shape")
            dynamic_linkers.append(command_string(c, 12))
        elif command == 0x8000001C:
            # Neither our plain fixture nor int-main SDK reference needs rpaths.
            raise Rejected("rpath_not_supported")
        elif command in {0x22, 0x80000022}:
            need(size == 48 and not ({0x22, 0x80000022} - {command}) & seen,
                 "dyld_info_shape")
            values = unpack(c, 8, "<10I")
            metadata.extend((a, b) for a, b in zip(values[::2], values[1::2]) if b)
            rebases.append(values[1])
            binds.extend([values[3], values[5], values[7]])
        elif command in LINK_DATA or command == 0x1D:
            need(size == 16, "linkedit_command_shape")
            location, length = unpack(c, 8, "<2I")
            if length:
                metadata.append((location, length))
            if command == 0x1D:
                signatures += 1
            elif command == 0x80000034:
                need(length >= 28, "chained_header_shape")
                payload = region(data, location, length)
                version, starts, imports, symbols, nimports, format, symformat = unpack(payload, 0, "<7I")
                need(version == 0 and max(starts, imports, symbols) <= length
                     and format in {1, 2, 3} and symformat == 0, "chained_header_shape")
                need(not nimports or (imports >= 28 and symbols >= 28
                     and imports + nimports * {1: 4, 2: 8, 3: 16}[format] <= length),
                     "chained_import_bounds")
                chained.append(nimports)
        elif command == 0x2:
            need(size == 24, "symtab_shape")
            symtab = unpack(c, 8, "<4I")
            metadata.extend([(symtab[0], symtab[1] * 16), (symtab[2], symtab[3])])
        elif command == 0xB:
            need(size == 80, "dysymtab_shape")
            dysymtab = unpack(c, 8, "<18I")
            for pair, stride in [(6, 8), (8, 56), (10, 4), (12, 4), (14, 8), (16, 8)]:
                if dysymtab[pair + 1]:
                    metadata.append((dysymtab[pair], dysymtab[pair + 1] * stride))
        elif command == 0x1B:
            need(size == 24, "uuid_shape")
            uuids += 1
        elif command == 0x2A:
            need(size == 16, "source_version_shape")
        elif command == 0x2C:
            need(size == 24, "encryption_info_shape")
            location, length, cryptid, pad = unpack(c, 8, "<4I")
            need(cryptid == 0 and pad == 0, "encrypted_image")
            if length:
                region(data, location, length)
        elif command == 0x80000028:
            need(size == 24, "entrypoint_shape")
            entrypoints += 1
        else:
            raise Rejected("unrecognized_command")
        offset += size
    need(offset == end and builds == [(platform, MIN_OS, SDK)], "platform_or_sdk")
    need(uuids == 1 and segments and segments[0][0] in {"__TEXT", "__PAGEZERO"}, "segment_or_uuid")
    headers = [s for s in segments if s[0] == "__TEXT" and s[3] == 0 and s[4] >= end and s[6] == 5]
    need(len(headers) == 1, "readable_executable_header_required")
    need(segments[-1][0] == "__LINKEDIT" and segments[-1][5] == 0
         and segments[-1][6] == 1 and segments[-1][3] + segments[-1][4] == len(data), "terminal_linkedit")
    for left, right in zip(segments, segments[1:]):
        need(left[1] + left[2] <= right[1], "segment_vm_overlap")
    backed = [s for s in segments if s[4]]
    for left, right in zip(backed, backed[1:]):
        need(left[3] + left[4] <= right[3], "segment_file_overlap")
    linkoff, linksize = segments[-1][3:5]
    for location, length in metadata:
        if length:
            need(linkoff <= location and location + length <= linkoff + linksize,
                 "metadata_not_in_linkedit")
    need(symtab is not None and dysymtab is not None, "symbol_tables_required")
    symoff, nsyms, stroff, strsize = symtab
    strings = region(data, stroff, strsize)
    need(strsize > 0, "string_table_empty")
    # One scan bounds all name-terminator queries, even if many symbols name
    # the same long suffix. No quadratic search through a hostile string table.
    last_terminator = strings.rfind(b"\0")
    for pair in [0, 2, 4]:
        need(dysymtab[pair] + dysymtab[pair + 1] <= nsyms, "dysymtab_symbol_bounds")
    undefined = 0
    sections = sum(s[5] for s in segments)
    for i in range(nsyms):
        strx, typ, sect, _, value = unpack(data, symoff + i * 16, "<IBBHQ")
        need(strx < strsize and strx <= last_terminator, "symbol_string_bounds")
        if typ & 0xE0:  # Debugging entries do not define or import link symbols.
            continue
        base = typ & 0xE
        need(base in {0, 2, 0xE}, "unsupported_indirect_or_prebound_symbol")
        if base == 0:
            undefined += 1  # Includes weak undefined and unallocated common.
        elif base == 0xE:
            need(1 <= sect <= sections, "symbol_section_bounds")
    if kind == 6:
        need(flags & 0x81 == 0x81 and not dynamic_linkers and entrypoints == 0,
             "fixture_link_flags_or_entry")
        need(identifiers == ["@rpath/PrivateGuest.framework/PrivateGuest"], "fixture_identifier")
    else:
        need(not identifiers and dynamic_linkers == ["/usr/lib/dyld"] and entrypoints == 1,
             "sdk_probe_entry")
    return dict(dylibs=dylibs, binds=binds, chained=chained, rebases=rebases,
                raw_undefined=undefined, indexed_undefined=dysymtab[5], signatures=signatures)


def verify(fixture, sdk_probe, platform):
    need(platform in {2, 7}, "expected_platform")
    reference = inspect(sdk_probe, 2, platform)
    need(len(reference["dylibs"]) == 1, "sdk_system_dependency")
    expected = reference["dylibs"][0]
    need(expected[:2] == (0xC, SYSTEM) and min(expected[2:]) >= 0x10000, "sdk_system_dependency")
    actual = inspect(fixture, 6, platform)
    need(actual["dylibs"] == [expected], "fixture_system_dependency_or_versions")
    need(actual["raw_undefined"] == actual["indexed_undefined"] == 0, "fixture_undefined_symbols")
    need(not any(actual["binds"]) and not any(actual["chained"]), "fixture_dynamic_imports")
    need(actual["signatures"] == 0, "fixture_has_code_signature")
    return dict(public_fixture_metadata_verified=True, platform=platform,
                sdk_system_versions_match=True, ordinary_system_loads=1,
                raw_undefined_symbols=0, indexed_undefined_symbols=0,
                legacy_bind_bytes=sum(actual["binds"]), chained_imports=sum(actual["chained"]),
                local_rebase_bytes=sum(actual["rebases"]), code_signature_commands=0,
                signature_authenticated=False, game_executed=False,
                private_framework_verified=False)


def read(path):
    metadata = path.lstat()
    need(stat.S_ISREG(metadata.st_mode) and 32 <= metadata.st_size <= MAX_BYTES, "input_regular_file_size")
    with path.open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    need(len(data) == metadata.st_size and len(data) <= MAX_BYTES, "input_changed_or_too_large")
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--sdk-probe", required=True, type=Path)
    parser.add_argument("--platform", required=True, type=int, choices=[2, 7])
    args = parser.parse_args(argv)
    try:
        print(json.dumps(verify(read(args.fixture), read(args.sdk_probe), args.platform), sort_keys=True))
        return 0
    except Rejected as error:
        print(json.dumps(dict(public_fixture_metadata_verified=False, category=str(error))))
        return 2
    except (OSError, OverflowError, struct.error):
        print(json.dumps(dict(public_fixture_metadata_verified=False, category="input_io_or_shape")))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
