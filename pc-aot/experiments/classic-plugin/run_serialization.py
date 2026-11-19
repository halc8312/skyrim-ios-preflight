"""Exchange an authored Classic co-save between its Windows DLL and its AOT translation."""
import argparse
from importlib import metadata
import json
from pathlib import Path
import struct
import subprocess
import sys

from run import HERE, ROOT, authored_bytes, build_windows_dll, device_objects, lift_authored, run

DLL = "serial_fixture.dll"
HASH = "107eb9644161fe19519d9fcf53ea02362cc76935f75529863fe53168c6d8e847"
PORT = ROOT / "ios/runtime-port"


def expected_bytes(value):
    payload = b"opaque\x00\xff\x80\x01!"
    other = struct.pack("<6I", 0xf3e2d1c0, 1, 12 + len(payload), 0xdeca1234, 9, len(payload)) + payload
    owned = b"" if value is None else struct.pack("<7I", 0x46585452, 1, 16, 0x56414c55, 1, 4, value)
    return struct.pack("<4s4I", b"SKSE", 1, 0x01070030, 0x09200000, 1 + bool(owned)) + other + owned


def checked_file(path, value):
    expected = expected_bytes(value)
    if path.is_symlink() or not path.is_file():
        raise ValueError("expected an authored regular co-save")
    with path.open("rb") as stream:
        actual = stream.read(len(expected) + 1)
    if actual != expected:
        raise ValueError("co-save differs from the fixture in its header, owner or opaque section")
    return actual


def native_host(work, llvm):
    clang = build_windows_dll(work, llvm, HERE / "serialization_fixture.c", "serial_fixture")
    run(clang, "--target=i686-pc-windows-msvc", "-std=c11", "-O1", "-fuse-ld=lld",
        HERE / "serialization_windows.c", HERE / "serialization_session.c", HERE / "serialization_files.c",
        "serial_fixture.lib", "-o", "serial_windows.exe", cwd=work)
    return authored_bytes(work / DLL, HASH)


def measured(executable, mode, work, expected):
    row = run(executable, mode, cwd=work).decode("ascii").splitlines()
    if row != [expected]:
        raise ValueError(f"unexpected serialization callback results: {row!r}")
    return expected


def windows(work, llvm):
    native_host(work, llvm)
    (work / "seed.skse").write_bytes(expected_bytes(None))
    row = measured(work / "serial_windows.exe", "start", work, "11 0 16 0 0")
    checked_file(work / "seed.skse", None)
    checked_file(work / "windows.skse", 16)
    return {"dll_sha256": HASH, "windows_save": row}


def serial_layout(dll, graph, generated):
    import pefile

    exports = {n: int(addr, 16) for n, addr in graph["exports"].items()}
    entries = {int(addr, 16) for addr in graph["functions"]}
    if (set(exports) != {"SKSEPlugin_Query", "SKSEPlugin_Load"} or
            not set(exports.values()) < entries or len(entries) != 5 or graph["iat"] or graph["indirect_jumps"]):
        raise ValueError("expected two SKSE exports and three serialization callbacks")
    with pefile.PE(str(dll)) as pe:
        sections = {s.Name.rstrip(b"\0"): s for s in pe.sections}
        data = sections[b".data"]
        if data.SizeOfRawData or data.Misc_VirtualSize != 4:
            raise ValueError("unexpected mutable data in authored serialization DLL")
        ro = sections[b".rdata"]
        rodata = ro.get_data(length=ro.Misc_VirtualSize)
        base = pe.OPTIONAL_HEADER.ImageBase
        ro_address, end = base + ro.VirtualAddress, base + pe.OPTIONAL_HEADER.SizeOfImage
        if not 0x800c < ro_address < ro_address + len(rodata) <= end <= 2 * 1024 * 1024:
            raise ValueError("unexpected serialization DLL memory layout")
    (generated / "serial_layout.h").write_text(
        f"#define SERIAL_QUERY f_{exports['SKSEPlugin_Query']:X}\n"
        f"#define SERIAL_LOAD f_{exports['SKSEPlugin_Load']:X}\n"
        f"#define SERIAL_RODATA {ro_address}u\n#define SERIAL_IMAGE_END {end}u\n"
        "static const unsigned char serial_rodata[] = {" + ",".join(str(b) for b in rodata) + "};\n",
        encoding="ascii")


def exchange(work, skydream, out, cc="clang", darwin_headers=None):
    data = authored_bytes(work / DLL, HASH)
    file_data = checked_file(work / "windows.skse", 16)
    dll, graph, generated, flags = lift_authored(data, DLL, "serial", skydream, out)
    serial_layout(dll, graph, generated)
    flags.extend(["-I", PORT])
    units = [generated / "serial_00.c", generated / "serial_table.c", HERE / "serialization_translated.c",
             HERE / "serialization_session.c", HERE / "serialization_files.c", PORT / "classic_cosave.c"]
    run(cc, *flags, *units, "-lm", "-o", out / "serial_translated", cwd=out)
    (out / "windows.skse").write_bytes(file_data)
    row = measured(out / "serial_translated", "exchange", out, "77 16 21 2 1")
    checked_file(out / "windows.skse", 16)
    checked_file(out / "translated.skse", 21)
    return {"dll_sha256": HASH, "translated_save": row, "ios_device_objects":
            device_objects(cc, darwin_headers, units, flags, out), "iphone_execution_verified": False}


def finish(work, exchange_dir, out, llvm):
    original = authored_bytes(work / DLL, HASH)
    exchanged_file = exchange_dir / "translated.skse" if exchange_dir.is_dir() else exchange_dir
    value = checked_file(exchanged_file, 21)
    if native_host(out, llvm) != original:
        raise ValueError("rebuilt fixture differs from the original Windows DLL")
    # Use the bytes supplied to the translator with a freshly built host.
    (out / DLL).write_bytes(original)
    (out / "translated.skse").write_bytes(value)
    row = measured(out / "serial_windows.exe", "finish", out, "99 21 26 2 1")
    checked_file(out / "translated.skse", 21)
    checked_file(out / "returned.skse", 26)
    return {"dll_sha256": HASH, "windows_restore_and_save": row, "foreign_section_preserved": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    start = sub.add_parser("windows")
    compare = sub.add_parser("exchange")
    end = sub.add_parser("finish")
    for item in (start, compare, end):
        item.add_argument("work", type=Path)
    for item in (start, end):
        item.add_argument("--llvm-bin", type=Path, default=Path("C:/Program Files/LLVM/bin"))
    compare.add_argument("out", type=Path)
    compare.add_argument("--skydream", type=Path, required=True)
    compare.add_argument("--cc", default="clang")
    compare.add_argument("--darwin-headers", type=Path)
    end.add_argument("exchange_dir", type=Path)
    end.add_argument("out", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "windows":
            result = windows(args.work.resolve(), args.llvm_bin.resolve())
        elif args.mode == "exchange":
            result = exchange(args.work.resolve(), args.skydream.resolve(), args.out.resolve(), args.cc,
                              args.darwin_headers.resolve() if args.darwin_headers else None)
        else:
            result = finish(args.work.resolve(), args.exchange_dir.resolve(), args.out.resolve(), args.llvm_bin.resolve())
    except (OSError, ValueError, metadata.PackageNotFoundError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Serialization fixture failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
