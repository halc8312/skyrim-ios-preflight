#!/usr/bin/env python3
"""Compare one authored x86 COM caller with Windows and two fixed Skydream AOT layouts."""

import argparse
import hashlib
from importlib.metadata import version
import json
import os
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
DLL = "OwnedDepth.dll"
DLL_BYTES = 3072
DLL_SHA256 = "cc7c58a5554cbefa28d4781a6332d5c9735ba9e8a253fd4a84cbd6d021c101ef"
BASES = (0x00100000, 0x00200000)
EXPECTED = (
    "0 00000000 00000000 1 AQR",
    "1 00000000 00000001 1 AQR",
    "2 00000000 00000002 1 AQR",
    "3 00000001 82345678 1 AQR",
    "4 88760868 a5b6c7d8 1 AQR",
    "5 8876086c a5b6c7d8 1 AQR",
    "6 8876086c a5b6c7d8 1",
)


def run(args, cwd, env=None):
    return subprocess.run(args, cwd=cwd, env=env, check=True, stdout=subprocess.PIPE, timeout=90).stdout


def authored_file(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("The comparison requires the authored Windows build")
    with path.open("rb") as source:
        data = source.read(DLL_BYTES + 1)
    if len(data) != DLL_BYTES or hashlib.sha256(data).hexdigest() != DLL_SHA256:
        raise ValueError("The Windows image does not match the fixed self-authored fixture")
    return data


def rows(data):
    result = tuple(line.rstrip() for line in data.decode("ascii").splitlines())
    if result != EXPECTED:
        raise ValueError(f"Unexpected HRESULT, value, refcount or virtual method order: {result!r}")
    return result


def windows(llvm, work):
    if os.name != "nt":
        raise ValueError("The reference requires Windows and its D3D9 implementation")
    work.mkdir(parents=True, exist_ok=False)
    cc, cxx, link = (llvm / name for name in ("clang.exe", "clang++.exe", "lld-link.exe"))
    target = "--target=i686-pc-windows-msvc"
    run(
        [
            cc,
            target,
            "-ffreestanding",
            "-fno-stack-protector",
            "-std=c11",
            "-O1",
            "-g0",
            "-c",
            HERE / "probe_dll.c",
            "-o",
            "owned.obj",
        ],
        work,
    )
    run(
        [
            link,
            "/dll",
            "/noentry",
            "/nodefaultlib",
            "/machine:x86",
            "/timestamp:0",
            "/base:0x100000",
            "/out:" + DLL,
            "/implib:OwnedDepth.lib",
            "owned.obj",
        ],
        work,
    )
    authored_file(work / DLL)
    run([cxx, target, "-std=c++17", "-O2", "-c", HERE / "d3d9_backend.cpp", "-o", "backend.obj"], work)
    run(
        [
            cc,
            target,
            "-std=c11",
            "-O2",
            "-fuse-ld=lld",
            HERE / "native_windows.c",
            HERE / "device_bridge.c",
            "backend.obj",
            "OwnedDepth.lib",
            "-ld3d9",
            "-luser32",
            "-o",
            "oracle.exe",
        ],
        work,
    )
    result = rows(run([work / "oracle.exe"], work))
    (work / "native.txt").write_text("\n".join(result) + "\n", encoding="ascii", newline="\n")
    print("Seven Windows x86 traces and two real D3D9 device depth settings matched")


def stage(source, upstream):
    files = (
        "tools/discover.py",
        "tools/lift.py",
        "tools/peimg.py",
        "runtime/x86.h",
        "runtime/rt_table.h",
        "runtime/lift_prelude.h",
    )
    for name in files:
        dest = source / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(
            run(
                ["git", "-c", f"safe.directory={upstream}", "-C", upstream, "show", f"{PIN}:{name}"], source
            )
        )
    run(["git", "init", "--quiet"], source)
    for patch in (
        "0001-auxiliary-carry.patch",
        "0005-call-flags.patch",
        "0006-inline-ordered-stores.patch",
    ):
        run(["git", "apply", ROOT / "patches/skydream" / patch], source)
    # An import-free fixture legitimately has no pefile import-directory attribute.
    path = source / "tools/discover.py"
    text = path.read_text(encoding="utf-8")
    old = "for imp in pe.DIRECTORY_ENTRY_IMPORT:"
    if text.count(old) != 1:
        raise ValueError("Unexpected pinned discovery source")
    path.write_text(
        text.replace(old, 'for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", ()):'), encoding="utf-8"
    )


def write_layout(dll, base, generated, functions, peimg):
    pe, actual, image = peimg.load(str(dll), base)
    try:
        exports = peimg.exports(pe, base)
        entry = base + 0x1000
        if (
            actual != base
            or exports != {"ReadDepthState": entry}
            or set(map(lambda key: int(key, 16), functions)) != {entry}
        ):
            raise ValueError("Expected exactly the authored export and its single translated body")
        relocs = sorted(
            r.rva for block in pe.DIRECTORY_ENTRY_BASERELOC for r in block.entries if r.type == 3
        )
        if relocs != [0x101E, 0x3000]:
            raise ValueError("Expected two real PE32 HIGHLOW relocations")
        words = tuple(int.from_bytes(image[rva : rva + 4], "little") for rva in relocs)
        if words != (base + 0x3000, base + 0x2000):
            raise ValueError("The code operand or data pointer did not relocate to the chosen guest base")
        lines = [f"#define OWNED_DEPTH_END {base + len(image)}u", f"#define OWNED_DEPTH_ENTRY f_{entry:X}"]
        for section in pe.sections:
            name = section.Name.rstrip(b"\0").decode("ascii")
            if name not in (".rdata", ".data"):
                continue
            label = name[1:]
            begin = section.VirtualAddress
            contents = image[begin : begin + section.Misc_VirtualSize]
            lines.extend(
                [
                    f"#define OWNED_DEPTH_{label.upper()} {base + begin}u",
                    f"static const unsigned char owned_depth_{label}[]={{"
                    + ",".join(str(v) for v in contents)
                    + "};",
                ]
            )
        (generated / "owned_depth_layout.h").write_text("\n".join(lines) + "\n", encoding="ascii")
    finally:
        pe.close()


def aot(upstream, reference, work, cc, sanitize):
    if (version("capstone"), version("pefile")) != ("5.0.9", "2024.8.26"):
        raise ValueError("Use the project's pinned translation environment")
    native = rows((reference / "native.txt").read_bytes())
    data = authored_file(reference / DLL)
    work.mkdir(parents=True, exist_ok=False)
    dll = work / DLL
    dll.write_bytes(data)
    source = work / "source"
    stage(source, upstream)
    sys.path.insert(0, str(source / "tools"))
    import peimg

    env = dict(os.environ, DISCOVER_JOBS="1", LIFT_FILES="1", LIFT_TSO="1", LIFT_MEMORDER="0")
    flags = ["-std=gnu11", "-O1", "-fno-strict-aliasing", "-fwrapv", "-DLB_STRICT_ORDER"]
    if sanitize:
        flags.extend(
            ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        )
    for base in BASES:
        where = work / f"at-{base:x}"
        where.mkdir()
        graph, generated = where / "functions.json", where / "generated"
        run([sys.executable, "-B", source / "tools/discover.py", dll, graph, hex(base)], where, env)
        run(
            [
                sys.executable,
                "-B",
                source / "tools/lift.py",
                dll,
                graph,
                generated,
                "owneddepth",
                hex(base),
            ],
            where,
            env,
        )
        if json.loads((generated / "owneddepth_rejected.json").read_text()):
            raise ValueError("The pinned lifter rejected an authored instruction")
        write_layout(dll, base, generated, json.loads(graph.read_text())["functions"], peimg)
        exe = where / "translated"
        run(
            [
                cc,
                *flags,
                "-I",
                source / "runtime",
                "-I",
                generated,
                generated / "owneddepth_00.c",
                HERE / "device_bridge.c",
                HERE / "translated_fixture.c",
                "-lm",
                "-o",
                exe,
            ],
            where,
        )
        if rows(run([exe], where)) != native:
            raise ValueError(f"The translated trace at {base:#x} differs from native Windows")
    print("Two fixed i386 image layouts each matched all Windows traces, EAX, preserved registers and ESP")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    win = modes.add_parser("windows")
    win.add_argument("work", type=Path)
    win.add_argument("--llvm", type=Path, default=Path("C:/Program Files/LLVM/bin"))
    cross = modes.add_parser("aot")
    cross.add_argument("work", type=Path)
    cross.add_argument("--skydream", type=Path, required=True)
    cross.add_argument("--reference", type=Path, required=True)
    cross.add_argument("--cc", default="cc")
    cross.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    try:
        if args.mode == "windows":
            windows(args.llvm.resolve(), args.work.resolve())
        else:
            aot(
                args.skydream.resolve(),
                args.reference.resolve(),
                args.work.resolve(),
                args.cc,
                args.sanitize,
            )
    except (OSError, ValueError, subprocess.CalledProcessError, RuntimeError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
