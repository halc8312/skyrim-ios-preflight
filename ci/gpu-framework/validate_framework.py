"""Inspect a PUBLIC thin ARM64 framework. Metadata/linking is not GPU execution."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

PIN = "6aed50955d934ac36049ba8d002034841633ae02"
HEADER_PIN = "673658bc2bd70ec39fc55ebe6bb0173cf6d0a603"
INSTALL_NAME = "@rpath/WgpuNative.framework/WgpuNative"
REQUIRED = {
    "_wgpuGetVersion", "_wgpuCreateInstance", "_wgpuInstanceCreateSurface",
    "_wgpuInstanceRequestAdapter", "_wgpuAdapterRequestDevice", "_wgpuAdapterGetInfo",
    "_wgpuDeviceGetQueue", "_wgpuDeviceCreateTexture", "_wgpuTextureCreateView",
    "_wgpuDeviceCreateBuffer", "_wgpuDeviceCreateCommandEncoder",
    "_wgpuCommandEncoderBeginRenderPass", "_wgpuRenderPassEncoderEnd",
    "_wgpuCommandEncoderCopyTextureToBuffer", "_wgpuCommandEncoderFinish",
    "_wgpuQueueSubmit", "_wgpuBufferMapAsync", "_wgpuBufferGetConstMappedRange",
    "_wgpuBufferUnmap", "_wgpuDevicePoll", "_wgpuSurfaceGetCapabilities",
    "_wgpuSurfaceConfigure", "_wgpuSurfaceGetCurrentTexture", "_wgpuSurfacePresent",
}


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("truncated u32")
    return struct.unpack_from("<I", data, offset)[0]


def cstring(data: bytes, start: int, end: int) -> str:
    if not 0 <= start < end <= len(data):
        raise ValueError("invalid string bounds")
    stop = data.find(b"\0", start, end)
    if stop < 0:
        raise ValueError("unterminated string")
    return data[start:stop].decode("ascii")


def uleb(data: bytes, offset: int, end: int) -> tuple[int, int]:
    result = 0
    for shift in range(0, 70, 7):
        if offset >= end:
            raise ValueError("truncated ULEB128")
        value = data[offset]
        offset += 1
        if shift == 63 and value > 1:
            raise ValueError("overflowing ULEB128")
        result |= (value & 0x7f) << shift
        if not value & 0x80:
            return result, offset
    raise ValueError("overflowing ULEB128")


def exported_names(trie: bytes) -> set[str]:
    names: set[str] = set()
    visits = 0

    def visit(offset: int, prefix: str, ancestors: frozenset[int]) -> None:
        nonlocal visits
        visits += 1
        if visits > 100000 or len(ancestors) > 128 or len(prefix) > 1024:
            raise ValueError("unreasonable export trie")
        if offset in ancestors or offset >= len(trie):
            raise ValueError("cyclic or out-of-bounds export trie")
        terminal_size, position = uleb(trie, offset, len(trie))
        terminal_end = position + terminal_size
        if terminal_end >= len(trie):
            raise ValueError("truncated export terminal")
        if terminal_size:
            flags, address_position = uleb(trie, position, terminal_end)
            # Only direct regular C functions, not re-exports/TLS/resolvers.
            if flags & ~4:
                raise ValueError("unsupported export flags")
            _, address_end = uleb(trie, address_position, terminal_end)
            if address_end != terminal_end or not prefix or prefix in names:
                raise ValueError("invalid or duplicate export")
            names.add(prefix)
        children = trie[terminal_end]
        position = terminal_end + 1
        for _ in range(children):
            edge = cstring(trie, position, len(trie))
            if not edge:
                raise ValueError("empty export edge")
            position += len(edge) + 1
            child, position = uleb(trie, position, len(trie))
            visit(child, prefix + edge, ancestors | {offset})

    if not trie:
        raise ValueError("missing export trie")
    visit(0, "", frozenset())
    return names


def inspect(data: bytes, platform: int, minimum_os: int,
            required: set[str] | None = None) -> dict:
    if len(data) < 32:
        raise ValueError("truncated Mach-O header")
    magic, cpu, subtype, kind, count, command_size, flags, _ = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C or subtype & 0xffffff == 2 or kind != 6:
        raise ValueError("expected a thin plain ARM64 MH_DYLIB")
    if not flags & 0x80:
        raise ValueError("MH_TWOLEVEL namespace is required")
    if count > 4096 or command_size > 4 * 1024 * 1024 or 32 + command_size > len(data):
        raise ValueError("invalid load command bounds")
    position, end = 32, 32 + command_size
    version = identity = export_range = None
    signature_present = False
    loads: list[str] = []
    for _ in range(count):
        if position + 8 > end:
            raise ValueError("truncated load command")
        command, size = struct.unpack_from("<2I", data, position)
        if size < 8 or size % 8 or position + size > end:
            raise ValueError("invalid load command size")
        if command == 0x32:
            if size < 24 or version is not None or u32(data, position + 20) > (size - 24) // 8:
                raise ValueError("invalid LC_BUILD_VERSION")
            version = (u32(data, position + 8), u32(data, position + 12), u32(data, position + 16))
        elif command in (0xd, 0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023):
            if size < 24:
                raise ValueError("truncated dylib command")
            name_offset = u32(data, position + 8)
            if name_offset < 24:
                raise ValueError("invalid dylib name offset")
            name = cstring(data, position + name_offset, position + size)
            if command == 0xd:
                if identity is not None:
                    raise ValueError("duplicate install name")
                identity = name
            else:
                loads.append(name)
        elif command == 0x80000033:
            if size != 16 or export_range is not None:
                raise ValueError("invalid export trie command")
            export_range = (u32(data, position + 8), u32(data, position + 12))
        elif command in (0x22, 0x80000022):
            if size != 48 or export_range is not None:
                raise ValueError("invalid dyld info command")
            export_range = (u32(data, position + 40), u32(data, position + 44))
        elif command == 0x1d:
            signature_present = True
        position += size
    if position != end or version is None or version[0] != platform or version[1] > minimum_os:
        raise ValueError("wrong platform/minimum OS or load command extent")
    if identity != INSTALL_NAME:
        raise ValueError("unexpected framework install_name")
    if any(not (name.startswith("/usr/lib/") or name.startswith("/System/Library/Frameworks/")) for name in loads):
        raise ValueError("non-system dynamic dependency")
    if export_range is None:
        raise ValueError("missing exports")
    export_start, export_size = export_range
    if not export_size or export_start < end or export_start + export_size > len(data):
        raise ValueError("invalid export data bounds")
    names = exported_names(data[export_start:export_start + export_size])
    unexpected = sorted(name for name in names if not name.startswith("_wgpu"))
    missing = sorted((REQUIRED if required is None else required) - names)
    if unexpected:
        raise ValueError(f"non-C-API dynamic exports: {unexpected}")
    if missing:
        raise ValueError(f"missing required C-API exports: {missing}")
    return {
        "source_commit": PIN, "headers_commit": HEADER_PIN,
        "sha256": hashlib.sha256(data).hexdigest(), "platform": platform,
        "minimum_os": version[1], "sdk": version[2], "install_name": identity,
        "two_level_namespace": True, "system_loads": loads,
        "c_api_export_count": len(names), "c_api_exports": sorted(names),
        "metadata_verified": True, "gpu_runtime_verified": False,
        "code_signature_present": signature_present, "signing_verified": False,
        "version_runtime_verified": False,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--platform", required=True, type=int, choices=(2, 7))
    parser.add_argument("--minimum-os", default="26.0")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    parts = [int(part) for part in args.minimum_os.split(".")]
    if not 1 <= len(parts) <= 3 or any(not 0 <= part <= (65535 if i == 0 else 255) for i, part in enumerate(parts)):
        parser.error("invalid minimum OS")
    minimum = (parts + [0] * (3 - len(parts)))
    minimum = minimum[0] << 16 | minimum[1] << 8 | minimum[2]
    if args.binary.stat().st_size > 512 * 1024 * 1024:
        parser.error("unexpected framework size")
    result = inspect(args.binary.read_bytes(), args.platform, minimum)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
