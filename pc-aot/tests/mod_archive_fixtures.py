"""Small authored read fixtures. No game files, archiver tools or network."""
import lzma
import struct
import zlib


def seven_uint(value):
    for extra in range(8):
        if value < (1 << (7 * (extra + 1))):
            first = (0xff << (8 - extra)) & 0xff
            tail = value.to_bytes(8, "little")[:extra]
            return bytes([first | (value >> (8 * extra))]) + tail
    return b"\xff" + value.to_bytes(8, "little")


def seven_zip(files):
    # One independent LZMA2 folder per regular file and an uncompressed header.
    streams = [lzma.compress(data, format=lzma.FORMAT_RAW,
                             filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 1 << 20}])
               for data in files.values()]
    count = seven_uint(len(files))
    pack = b"\x06\x00" + count + b"\x09" + b"".join(seven_uint(len(s)) for s in streams) + b"\x00"
    coder = b"\x01\x21\x21\x01\x10"  # one coder, LZMA2, 1 MiB dictionary
    folders = b"\x07\x0b" + count + b"\x00" + coder * len(files)
    sizes = b"\x0c" + b"".join(seven_uint(len(data)) for data in files.values())
    checks = b"\x0a\x01" + b"".join(struct.pack("<I", zlib.crc32(data)) for data in files.values())
    names = b"\x00" + "".join(n + "\0" for n in files).encode("utf-16le")
    info = b"\x05" + count + b"\x11" + seven_uint(len(names)) + names + b"\x00"
    # One unpack stream per folder (an empty SubStreamsInfo selects the defaults).
    header = b"\x01\x04" + pack + folders + sizes + checks + b"\x00\x08\x00\x00" + info + b"\x00"
    payload = b"".join(streams)
    start = struct.pack("<QQI", len(payload), len(header), zlib.crc32(header))
    return b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start + payload + header


def rar4_stored(name, data):
    def block(kind, flags, fields):
        header = struct.pack("<BHH", kind, flags, len(fields) + 7) + fields
        return struct.pack("<H", zlib.crc32(header) & 0xffff) + header

    name = name.encode("ascii")
    fields = struct.pack("<IIBIIBBHI", len(data), len(data), 3, zlib.crc32(data), 0,
                         20, 0x30, len(name), 0o100644) + name
    return (b"Rar!\x1a\x07\x00" + block(0x73, 0, bytes(6)) +
            block(0x74, 0x8000, fields) + data + block(0x7b, 0, b""))


def rar5_stored(name, data):
    def vint(n):
        out = bytearray()
        while n >= 128:
            out.append((n & 127) | 128)
            n >>= 7
        out.append(n)
        return bytes(out)

    def block(fields):
        header = vint(len(fields)) + fields
        return struct.pack("<I", zlib.crc32(header)) + header

    name = name.encode("utf-8")
    # File header with data area, checksum, UNIX file mode and stored method.
    fields = (b"\x02\x02" + vint(len(data)) + b"\x04" + vint(len(data)) + vint(0o100644) +
              struct.pack("<I", zlib.crc32(data)) + b"\x00\x01" + vint(len(name)) + name)
    return b"Rar!\x1a\x07\x01\x00" + block(b"\x01\x00\x00") + block(fields) + data + block(b"\x05\x00\x00")
