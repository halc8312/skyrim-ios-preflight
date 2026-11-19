"""Compare an authored Classic SKSE test DLL on Windows and after static translation."""
import argparse
import hashlib
from importlib import metadata
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
DLL_NAME = "classic_fixture.dll"
# LLVM 19.1.1, reproducible COFF link with /timestamp:0. No other DLL is executed or translated.
FIXTURE_SHA256 = "821fb1528772fe54a46c449893dbde4042499de33a7bf2866122459d7ef08897"
ROWS = [
    "0 1 1 1 17 ClassicProbe 1 1", "1 0 -1 1 17 ClassicProbe 0 0", "2 0 -1 1 17 ClassicProbe 0 0",
    "3 0 -1 1 17 ClassicProbe 0 0", "4 1 0 1 17 ClassicProbe 1 0", "5 1 0 1 17 ClassicProbe 1 1",
    "6 1 0 1 17 ClassicProbe 0 0", "7 1 0 1 17 ClassicProbe 0 0", "8 0 -1 0 0 - 0 0", "9 0 -1 0 0 - 0 0",
]


def run(*args, cwd, env=None):
    return subprocess.run([str(arg) for arg in args], cwd=cwd, check=True, stdout=subprocess.PIPE,
                          timeout=90, env=env).stdout


def authored_bytes(dll, expected_hash):
    if dll.is_symlink() or not dll.is_file():
        raise ValueError("expected the authored Classic test DLL")
    with dll.open("rb") as source:
        data = source.read(2561)  # The reproducible sample occupies 2560 bytes.
    if len(data) != 2560 or hashlib.sha256(data).hexdigest() != expected_hash:
        raise ValueError("only the reproducible, authored Classic test DLL is accepted")
    return data


def rows(output):
    lines = output.decode("ascii").splitlines()
    if lines != ROWS:
        raise ValueError(f"unexpected Query/Load results: {lines!r}")
    return lines


def build_windows_dll(work, llvm, source, stem):
    if sys.platform != "win32":
        raise ValueError("the native reference must run on Windows")
    work.mkdir(parents=True, exist_ok=False)
    clang = llvm / "clang.exe"
    linker = llvm / "lld-link.exe"
    run(clang, "--target=i686-pc-windows-msvc", "-std=c11", "-O1", "-g0", "-fno-stack-protector",
        "-c", source, "-o", "fixture.obj", cwd=work)
    run(linker, "/dll", "/noentry", "/nodefaultlib", "/machine:x86", "/timestamp:0", "/base:0x100000",
        "/out:" + stem + ".dll", "/implib:" + stem + ".lib", "fixture.obj", cwd=work)
    return clang


def windows(work, llvm):
    clang = build_windows_dll(work, llvm, HERE / "fixture.c", "classic_fixture")
    run(clang, "--target=i686-pc-windows-msvc", "-std=c11", "-O1", "-fuse-ld=lld", HERE / "windows_host.c",
        "classic_fixture.lib", "-o", "windows_reference.exe", cwd=work)
    authored_bytes(work / DLL_NAME, FIXTURE_SHA256)
    result = {"dll_sha256": FIXTURE_SHA256, "rows": rows(run(work / "windows_reference.exe", cwd=work))}
    (work / "windows.json").write_text(json.dumps(result, indent=2) + "\n", encoding="ascii")
    return result


def stage(skydream, source):
    files = ("tools/discover.py", "tools/lift.py", "tools/peimg.py", "runtime/x86.h", "runtime/rt_table.h",
             "runtime/lift_prelude.h")
    for name in files:
        dest = source / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(run("git", "-C", skydream, "show", PIN + ":" + name, cwd=source))
    # Use the build's flag and memory-order adapters on fresh public sources.
    run("git", "init", "--quiet", cwd=source)
    for name in ("0001-auxiliary-carry.patch", "0005-call-flags.patch", "0006-inline-ordered-stores.patch"):
        run("git", "apply", ROOT / "patches/skydream" / name, cwd=source)
    # pefile omits this attribute on images with no Win32 imports. The authored DLL uses only SKSE callbacks.
    script = source / "tools/discover.py"
    text = script.read_text(encoding="utf-8")
    old = "for imp in pe.DIRECTORY_ENTRY_IMPORT:"
    if text.count(old) != 1:
        raise ValueError("unrecognized discovery import-table loop")
    script.write_text(text.replace(old, 'for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", ()):'),
                      encoding="utf-8")


def write_layout(dll, graph, generated):
    import pefile

    expected = {"SKSEPlugin_Query", "SKSEPlugin_Load"}
    exports = {name: int(address, 16) for name, address in graph["exports"].items()}
    if (set(exports) != expected or len(set(exports.values())) != 2 or
            {int(address, 16) for address in graph["functions"]} != set(exports.values()) or
            graph["iat"] or graph["indirect_jumps"]):
        raise ValueError("unexpected fixture exports, functions, imports or unresolved jumps")
    with pefile.PE(str(dll)) as pe:
        base = pe.OPTIONAL_HEADER.ImageBase
        rodata = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".rdata")
        contents = rodata.get_data(length=rodata.Misc_VirtualSize)
        address = base + rodata.VirtualAddress
        end = base + pe.OPTIONAL_HEADER.SizeOfImage
        if not contents or not 0x800c <= address < address + len(contents) <= end <= 2 * 1024 * 1024:
            raise ValueError("unexpected fixed fixture image layout")
    (generated / "fixture_layout.h").write_text(
        f"#define FIXTURE_QUERY f_{exports['SKSEPlugin_Query']:X}\n"
        f"#define FIXTURE_LOAD f_{exports['SKSEPlugin_Load']:X}\n"
        f"#define FIXTURE_RODATA {address}u\n#define FIXTURE_IMAGE_END {end}u\n"
        "static const unsigned char fixture_rodata[] = {" +
        ",".join(str(byte) for byte in contents) + "};\n", encoding="ascii")


def verify_ios_object(path):
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError("truncated Mach-O object")
    magic, cpu, subtype, kind, count, size = struct.unpack_from("<6I", data)
    end = 32 + size
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1) or end > len(data):
        raise ValueError("expected an arm64 Mach-O object")
    offset, platforms = 32, []
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError("truncated Mach-O load command")
        cmd, length = struct.unpack_from("<II", data, offset)
        if length < 8 or length % 8 or offset + length > end:
            raise ValueError("invalid Mach-O command length")
        if cmd == 0x32:
            if length < 24:
                raise ValueError("truncated Mach-O build version")
            platforms.append(struct.unpack_from("<II", data, offset + 8))
        offset += length
    if offset != end or platforms != [(2, 18 << 16)]:
        raise ValueError("expected iOS device minimum 18.0")


def lift_authored(data, name, module, skydream, out):
    if (metadata.version("capstone"), metadata.version("pefile")) != ("5.0.9", "2024.8.26"):
        raise ValueError("use the project's pinned translation environment")
    out.mkdir(parents=True, exist_ok=False)
    dll = out / name
    dll.write_bytes(data)  # Discovery, lifting and the read-only image use the same verified snapshot.
    source = out / "source"
    source.mkdir()
    stage(skydream, source)
    env = dict(os.environ, DISCOVER_JOBS="1", LIFT_FILES="1", LIFT_TSO="1", LIFT_MEMORDER="0")
    graph_file = out / "functions.json"
    generated = out / "generated"
    run(sys.executable, "-B", source / "tools/discover.py", dll, graph_file, cwd=out, env=env)
    run(sys.executable, "-B", source / "tools/lift.py", dll, graph_file, generated, module, cwd=out, env=env)
    if json.loads((generated / (module + "_rejected.json")).read_text()):
        raise ValueError("an authored export was rejected by the lifter")
    flags = ["-std=gnu11", "-O1", "-fno-strict-aliasing", "-fwrapv", "-DLB_STRICT_ORDER",
             "-I", generated, "-I", source / "runtime"]
    return dll, json.loads(graph_file.read_text()), generated, flags


def device_objects(cc, darwin_headers, inputs, flags, out):
    objects = []
    if darwin_headers:
        builtin = Path(run(cc, "-print-resource-dir", cwd=out).decode().strip()) / "include"
        for unit in inputs:
            obj = out / (unit.stem + "-ios.o")
            run(cc, "--target=arm64-apple-ios18.0", "-nostdinc", "-isystem", builtin, "-isystem", darwin_headers,
                *flags, "-c", unit, "-o", obj, cwd=out)
            verify_ios_object(obj)
            objects.append(obj.name)
    return objects


def compare(work, skydream, out, cc="clang", darwin_headers=None):
    data = authored_bytes(work / DLL_NAME, FIXTURE_SHA256)
    native = json.loads((work / "windows.json").read_text(encoding="ascii"))
    if native != {"dll_sha256": FIXTURE_SHA256, "rows": ROWS}:
        raise ValueError("matching Windows reference is required")
    dll, graph, generated, flags = lift_authored(data, DLL_NAME, "fixture", skydream, out)
    write_layout(dll, graph, generated)
    inputs = (generated / "fixture_00.c", HERE / "translated_host.c")
    run(cc, *flags, *inputs, "-lm", "-o", out / "translated_reference", cwd=out)
    result = rows(run(out / "translated_reference", cwd=out))
    if result != native["rows"]:
        raise ValueError("translated output differs from the original Windows DLL")
    ios_objects = device_objects(cc, darwin_headers, inputs, flags, out)
    return {"dll_sha256": FIXTURE_SHA256, "matching_cases": len(result), "ios_device_objects": ios_objects,
            "iphone_execution_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    win = sub.add_parser("windows", help="build and run the authored DLL in a new Windows folder")
    win.add_argument("work", type=Path)
    win.add_argument("--llvm-bin", type=Path, default=Path("C:/Program Files/LLVM/bin"))
    comp = sub.add_parser("compare", help="translate that same DLL and compare on a POSIX host")
    comp.add_argument("work", type=Path)
    comp.add_argument("out", type=Path)
    comp.add_argument("--skydream", type=Path, required=True)
    comp.add_argument("--cc", default="clang")
    comp.add_argument("--darwin-headers", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "windows":
            result = windows(args.work.resolve(), args.llvm_bin.resolve())
        else:
            result = compare(args.work.resolve(), args.skydream.resolve(), args.out.resolve(), args.cc,
                             args.darwin_headers.resolve() if args.darwin_headers else None)
    except (OSError, ValueError, metadata.PackageNotFoundError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Fixture comparison failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
