#!/usr/bin/env python3
"""Compare static TLS in one self-authored x86 DLL on Windows and pinned Skydream."""

import argparse
import hashlib
from importlib.metadata import version
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
DLL = "OwnedTls.dll"
DIGEST = "2cd951e32fc005a9232d8beccd27fcfa1cdf1d274e32a8d8b919636196991e27"
NATIVE_LINES = 22
BASES = (0x100000, 0x200000)


def run(args, where, env=None):
    return subprocess.run(args, cwd=where, env=env, check=True, stdout=subprocess.PIPE, timeout=90).stdout


def owned(path):
    if not path.is_file() or path.is_symlink() or path.stat().st_size != 3584:
        raise ValueError("Expected the 3,584-byte self-authored i386 test image")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != DIGEST:
        raise ValueError("This experiment accepts only its fixed self-authored image")
    return data


def validate(rows):
    values = [0x11111111, 0x22222222, 0x33333333]
    counts = [4, 8, 12]
    expected = [
        f"context {i} 13579bdf 0 13579bdf {value:08x} {count}"
        for i, (value, count) in enumerate(zip(values, counts))
    ]
    expected.append("loading context after threads 11111111 4")
    phases = [
        (1, 0x13579BDF, 0),
        (2, 0x13579BDF, 0),
        (3, values[1], counts[1]),
        (2, 0x13579BDF, 0),
        (3, values[2], counts[2]),
        (0, values[0], counts[0]),
    ]
    for reason, value, count in phases:
        for who in (1, 2, 3):
            expected.append(f"notify {who} {reason} {value:08x} {count}")
    if len(rows) != NATIVE_LINES or rows != expected:
        raise ValueError(f"Unexpected thread values or TLS/DllMain order: {rows!r}")
    return rows


def windows(work, llvm, kernel32):
    if os.name != "nt":
        raise ValueError("The Windows reference requires the native Windows loader")
    work.mkdir(parents=True, exist_ok=False)
    cc, link = llvm / "clang.exe", llvm / "lld-link.exe"
    flags = ["--target=i686-pc-windows-msvc", "-std=c11", "-Wall", "-Wextra", "-Werror"]
    run(
        [
            cc,
            *flags,
            "-ffreestanding",
            "-fno-stack-protector",
            "-O1",
            "-g0",
            "-c",
            HERE / "tls_fixture.c",
            "-o",
            "OwnedTls.obj",
        ],
        work,
    )
    # Force a conventional import table on Windows 10. The authored functions do
    # not invoke this import, on Windows or on the synthetic target.
    run(
        [
            link,
            "/dll",
            "/nodefaultlib",
            "/machine:x86",
            "/timestamp:0",
            "/base:0x100000",
            "/entry:DllMain@12",
            "/include:__tls_used",
            "/include:__imp__MulDiv@12",
            kernel32,
            "/out:" + DLL,
            "/implib:OwnedTls.lib",
            "OwnedTls.obj",
        ],
        work,
    )
    owned(work / DLL)
    run(
        [cc, *flags, "-fuse-ld=lld", "-O2", HERE / "native_windows.c", "-lkernel32", "-o", "oracle.exe"],
        work,
    )
    meta = run([llvm / "llvm-readobj.exe", "--coff-tls-directory", "--coff-imports", DLL], work).decode()
    assembly = run(
        [llvm / "llvm-objdump.exe", "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", DLL], work
    ).decode()
    if (
        "Format: COFF-i386" not in meta
        or "SizeOfZeroFill: 0xC" not in meta
        or "IMAGE_SCN_ALIGN_4BYTES" not in meta
        or "Symbol: MulDiv" not in meta
        or assembly.count("fs:[0x2c]") != 5
    ):
        raise ValueError("Unexpected SDK TLS directory, imports or emitted x86 FS reads")
    first, *rows = run([work / "oracle.exe"], work).decode("ascii").splitlines()
    if not re.fullmatch(r"Windows assigned module slot [0-9]+", first):
        raise ValueError("Windows did not report the assigned module slot")
    validate(rows)
    (work / "native.txt").write_text("\n".join(rows) + "\n", encoding="ascii", newline="\n")
    print(first)
    print("Windows passed three execution contexts, two independent workers and 18 ordered notifications")


def stage(source, upstream):
    for path in (
        "tools/discover.py",
        "tools/lift.py",
        "tools/peimg.py",
        "runtime/x86.h",
        "runtime/rt_table.h",
        "runtime/lift_prelude.h",
    ):
        target = source / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(
            run(
                ["git", "-c", f"safe.directory={upstream}", "-C", upstream, "show", f"{PIN}:{path}"], source
            )
        )
    run(["git", "init", "--quiet"], source)
    for name in ("0001-auxiliary-carry", "0005-call-flags", "0006-inline-ordered-stores"):
        run(["git", "apply", ROOT / f"patches/skydream/{name}.patch"], source)


def layout(dll, base, graph, dest, peimg):
    pe, actual, image = peimg.load(str(dll), base)
    try:
        directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[9]
        if actual != base or (directory.VirtualAddress, directory.Size) != (0x200C, 24):
            raise ValueError("Changed TLS directory placement")
        raw, end, index, table, zero, flags = struct.unpack_from("<6I", image, directory.VirtualAddress)
        callback_a, callback_b, terminator = struct.unpack_from("<3I", image, table - base)
        if (
            raw - base,
            end - base,
            index - base,
            table - base,
            zero,
            flags,
            callback_a - base,
            callback_b - base,
            terminator,
        ) != (0x4000, 0x400C, 0x3000, 0x2000, 12, 0x300000, 0x11B0, 0x11D0, 0):
            raise ValueError("TLS pointer, count, order, alignment or relocation differs from the fixture")
        relocs = {r.rva for block in pe.DIRECTORY_ENTRY_BASERELOC for r in block.entries if r.type == 3}
        if not {0x2000, 0x2004, 0x200C, 0x2010, 0x2014, 0x2018}.issubset(relocs):
            raise ValueError("Relocations for the six authored TLS VAs are required")
        original = bytes(image[raw - base : end - base])
        if original != struct.pack("<3I", 0, 0x13579BDF, 0):
            raise ValueError("Changed initialized or zero word in the authored template")
        exports = peimg.exports(pe, base)
        expected = {"VALUE": "ReadValue", "COUNT": "ReadCount", "SET": "SetPair", "BIND": "BindTrace"}
        if set(exports) != set(expected.values()):
            raise ValueError("Unexpected authored exports")
        targets = {key: exports[name] for key, name in expected.items()}
        targets.update(
            ENTRY=base + pe.OPTIONAL_HEADER.AddressOfEntryPoint,
            CALLBACK_A=callback_a,
            CALLBACK_B=callback_b,
        )
        functions = {int(key, 16) for key in graph["functions"]}
        if not set(targets.values()).issubset(functions):
            raise ValueError(f"Missing translated TLS functions {set(targets.values()) - functions}")
        data = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".data")
        static = image[data.VirtualAddress : data.VirtualAddress + data.Misc_VirtualSize]
        lines = [f"#define OWNED_TLS_{name} f_{address:X}" for name, address in targets.items()]
        constants = {
            "BASE": base,
            "INDEX": index,
            "ZERO": zero,
            "ALIGN": 1 << (((flags >> 20) & 15) - 1),
            "DATA": base + data.VirtualAddress,
            "IMAGE_END": base + pe.OPTIONAL_HEADER.SizeOfImage,
        }
        lines += [f"#define OWNED_TLS_{name} {number}u" for name, number in constants.items()]
        for name, blob in (("template", original), ("data", static)):
            lines.append(
                f"static const unsigned char owned_{name}[] = {{" + ",".join(map(str, blob)) + "};"
            )
        (dest / "owned_tls_layout.h").write_text("\n".join(lines) + "\n", encoding="ascii")
    finally:
        pe.close()


def aot(work, reference, upstream, cc, sanitize):
    if (version("capstone"), version("pefile")) != ("5.0.9", "2024.8.26"):
        raise ValueError("Use the pinned translation environment")
    expected = validate((reference / "native.txt").read_text(encoding="ascii").splitlines())
    data = owned(reference / DLL)
    work.mkdir(parents=True, exist_ok=False)
    dll = work / DLL
    dll.write_bytes(data)
    source = work / "source"
    stage(source, upstream)
    sys.path.insert(0, str(source / "tools"))
    import peimg

    env = dict(os.environ, DISCOVER_JOBS="1", LIFT_FILES="1", LIFT_TSO="1", LIFT_MEMORDER="0")
    flags = ["-std=gnu11", "-O1", "-fwrapv", "-fno-strict-aliasing", "-DLB_STRICT_ORDER"]
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
            [sys.executable, "-B", source / "tools/lift.py", dll, graph, generated, "ownedtls", hex(base)],
            where,
            env,
        )
        if json.loads((generated / "ownedtls_rejected.json").read_text()):
            raise ValueError("The pinned translator could not translate an authored instruction")
        layout(dll, base, json.loads(graph.read_text()), generated, peimg)
        exe = where / "translated"
        run(
            [
                cc,
                *flags,
                "-I",
                source / "runtime",
                "-I",
                generated,
                generated / "ownedtls_00.c",
                HERE / "translated.c",
                "-lm",
                "-o",
                exe,
            ],
            where,
        )
        for slot in (0, 5):
            rows = run([exe, str(slot)], where).decode("ascii").splitlines()
            if validate(rows) != expected:
                raise ValueError(f"Native and translated traces differ at base {base:#x}, slot {slot}")
    print("Two i386 layouts × two synthetic module slots each matched all 22 Windows trace rows")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    win = sub.add_parser("windows")
    win.add_argument("work", type=Path)
    win.add_argument("--llvm", type=Path, default=Path("C:/Program Files/LLVM/bin"))
    win.add_argument(
        "--kernel32-lib",
        type=Path,
        default=Path("C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/um/x86/kernel32.lib"),
    )
    cross = sub.add_parser("aot")
    cross.add_argument("work", type=Path)
    cross.add_argument("--reference", type=Path, required=True)
    cross.add_argument("--skydream", type=Path, required=True)
    cross.add_argument("--cc", default="cc")
    cross.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    try:
        if args.mode == "windows":
            windows(args.work.resolve(), args.llvm.resolve(), args.kernel32_lib.resolve())
        else:
            aot(
                args.work.resolve(),
                args.reference.resolve(),
                args.skydream.resolve(),
                args.cc,
                args.sanitize,
            )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
