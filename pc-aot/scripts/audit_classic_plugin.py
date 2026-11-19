"""Read one Windows DLL offline to identify Classic SKSE test candidates.

Usage: python scripts/audit_classic_plugin.py path/to/plugin.dll
The report covers PE headers and two public export names. Function signatures,
plugin behavior, dependencies and iPhone compatibility require separate tests.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import struct


ENTRY_NAMES = ("SKSEPlugin_Query", "SKSEPlugin_Load")
MAX_EXPORTS = 65_536
MAX_NAME_BYTES = 4096


def inspect(stream, file_size):
    def read(offset, size):
        if offset < 0 or size < 0 or offset + size > file_size:
            raise ValueError("file range outside DLL")
        stream.seek(offset)
        data = stream.read(size)
        if len(data) != size:
            raise ValueError("DLL was truncated during inspection")
        return data

    dos = read(0, 64)
    if dos[:2] != b"MZ":
        raise ValueError("missing DOS header")
    pe = struct.unpack_from("<I", dos, 60)[0]
    header = read(pe, 24)
    if pe < 64 or header[:4] != b"PE\0\0":
        raise ValueError("invalid PE header")
    machine, section_count = struct.unpack_from("<HH", header, 4)
    optional_size, flags = struct.unpack_from("<HH", header, 20)
    optional = read(pe + 24, optional_size)
    magic = int.from_bytes(optional[:2], "little")
    directories = {0x10B: 96, 0x20B: 112}.get(magic)
    if directories is None or len(optional) < directories or not 1 <= section_count <= 96:
        raise ValueError("invalid optional header or section count")
    image_size, header_size = struct.unpack_from("<II", optional, 56)
    directory_count = struct.unpack_from("<I", optional, directories - 4)[0]
    if directory_count > (optional_size - directories) // 8:
        raise ValueError("data directory count exceeds optional header")
    table = pe + 24 + optional_size
    if not table + section_count * 40 <= header_size <= min(file_size, image_size):
        raise ValueError("section table or image headers outside DLL")

    # Each region maps an RVA interval to the bytes actually present on disk.
    regions = [(0, header_size, 0, header_size, False)]
    raw_ranges = []
    for index in range(section_count):
        row = read(table + index * 40, 40)
        virtual_size, rva, raw_size, offset = struct.unpack_from("<IIII", row, 8)
        virtual_size = virtual_size or raw_size
        if rva + virtual_size > image_size:
            raise ValueError("section outside virtual image")
        if raw_size:
            if not header_size <= offset <= file_size - raw_size:
                raise ValueError("section outside DLL")
            raw_ranges.append((offset, offset + raw_size))
        if virtual_size:
            executable = bool(struct.unpack_from("<I", row, 36)[0] & 0x20000000)
            regions.append((rva, rva + virtual_size, offset, min(raw_size, virtual_size), executable))
    regions.sort()
    raw_ranges.sort()
    for ranges in (regions, raw_ranges):
        if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
            raise ValueError("overlapping sections")

    def region(rva):
        for start, end, offset, backed, executable in regions:
            if start <= rva < end:
                delta = rva - start
                return offset + delta, max(0, backed - delta), executable
        raise ValueError("RVA outside image sections")

    def file_ranges(rva, size):
        parts = []
        while size:
            offset, available, _ = region(rva)
            if not rva or not available:
                raise ValueError("table exceeds file-backed sections")
            chunk = min(available, size)
            parts.append((offset, chunk))
            rva += chunk
            size -= chunk
        return parts

    def rva_bytes(rva, size):
        return b"".join(read(offset, length) for offset, length in file_ranges(rva, size))

    def name_at(rva, limit):
        offset, available, _ = region(rva)
        data = read(offset, min(available, limit, MAX_NAME_BYTES))
        end = data.find(b"\0")
        if not rva or end < 1 or not data[:end].isascii():
            raise ValueError("invalid or over-limit export string")
        return data[:end].decode("ascii")

    exports = {name: "absent" for name in ENTRY_NAMES}
    export_rva, export_size = struct.unpack_from("<II", optional, directories) if directory_count else (0, 0)
    if export_rva or export_size:
        export_end = export_rva + export_size
        if not export_rva or export_size < 40 or export_end > image_size:
            raise ValueError("invalid export directory range")
        file_ranges(export_rva, export_size)
        directory = rva_bytes(export_rva, 40)
        functions, names, addresses, pointers, ordinals = struct.unpack_from("<IIIII", directory, 20)
        if max(functions, names) > MAX_EXPORTS:
            raise ValueError("export table exceeds inspection limit")
        address_table = rva_bytes(addresses, functions * 4)
        name_table = rva_bytes(pointers, names * 4)
        ordinal_table = rva_bytes(ordinals, names * 2)
        previous = ""
        for index in range(names):
            name_rva = struct.unpack_from("<I", name_table, index * 4)[0]
            name = name_at(name_rva, MAX_NAME_BYTES)
            ordinal = struct.unpack_from("<H", ordinal_table, index * 2)[0]
            if ordinal >= functions or name <= previous:
                raise ValueError("invalid export ordinal or name order")
            previous = name
            if name not in exports:
                continue
            target = struct.unpack_from("<I", address_table, ordinal * 4)[0]
            if not target:
                exports[name] = "null"
            elif export_rva <= target < export_end:
                # PE forwarders are strings inside the declared export directory.
                forward = name_at(target, export_end - target)
                library, _, symbol = forward.rpartition(".")
                if not library or not symbol:
                    raise ValueError("invalid export forwarder")
                exports[name] = "forwarded"
            else:
                _, backed, executable = region(target)
                exports[name] = "direct_code" if executable and backed else "non_code"
    is_dll = bool(flags & 0x2000)
    candidate = (is_dll and bool(flags & 0x0002) and magic == 0x10B and machine == 0x014C
                 and all(value == "direct_code" for value in exports.values()))
    return {
        "format": "PE32" if magic == 0x10B else "PE32+",
        "machine": {0x014C: "i386", 0x8664: "AMD64", 0xAA64: "ARM64"}.get(machine, f"0x{machine:04x}"),
        "dll": is_dll, "exports": exports, "classic_test_candidate": candidate,
        "iphone_compatibility": "untested",
    }


def audit(path):
    if not path.is_file():
        raise ValueError("expected a regular DLL file")
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("expected a regular DLL file")
        report = inspect(stream, before.st_size)
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("DLL changed during inspection")
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(audit(args.dll), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(2, f"Audit failed: {error}\n")


if __name__ == "__main__":
    main()
