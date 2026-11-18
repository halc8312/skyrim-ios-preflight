"""Compare pinned lifter-emitted C with real native x86 arithmetic, without game data.

Run with the existing approved WSL Python environment. No dependency installation,
Unicorn, JIT, vendor mutation, or game execution. Exact pinned archives are extracted
into fresh private baseline/patched trees. Exit 0 means patched tests pass and the
baseline fails as expected; 1 means regression/missing baseline reproduction;
2 means harness failure. Every invocation retains all raw baseline failures.
"""
from __future__ import annotations
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tarfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/skydream"
PIN = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
OPS = ("add", "sub", "adc", "sbb", "inc", "dec", "cmp", "neg")
BITS = (8, 16, 32)
FLAGS = {"CF": 0x1, "PF": 0x4, "AF": 0x10, "ZF": 0x40, "SF": 0x80, "OF": 0x800}
MASK = sum(FLAGS.values())
PATCH = ROOT / "patches/skydream/0001-auxiliary-carry.patch"

def run(command, directory, stem, *, input_path=None, env=None):
    (directory / f"{stem}.command.json").write_text(json.dumps(list(map(str, command)), indent=2) + "\n")
    with (directory / f"{stem}.stdout").open("wb") as stdout, (directory / f"{stem}.stderr").open("wb") as stderr:
        source = input_path.open("rb") if input_path else subprocess.DEVNULL
        try:
            result = subprocess.run(command, stdin=source, stdout=stdout, stderr=stderr, env=env, timeout=120)
        finally:
            if input_path:
                source.close()
    if result.returncode:
        raise RuntimeError(f"{stem} failed with exit {result.returncode}; see retained raw logs")

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def synthetic_pe(directory):
    code = bytearray(b"\xcc" * 1024)
    functions, records = {}, []
    opcodes = {"add": (0x00, 0xc8), "sub": (0x28, 0xc8), "adc": (0x10, 0xc8),
               "sbb": (0x18, 0xc8), "inc": (0xfe, 0xc0), "dec": (0xfe, 0xc8),
               "cmp": (0x38, 0xc8), "neg": (0xf6, 0xd8)}
    def add_record(op, bits, instruction):
        index = len(records)
        snippet = b"\x9d" + instruction + b"\x9c\x5a\xc3"
        address = 0x401000 + index * 16
        code[index * 16:index * 16 + len(snippet)] = snippet
        functions[hex(address)] = [[address, address + len(snippet)]]
        records.append({"index": index, "op": op, "bits": bits, "address": address,
                        "instruction_hex": instruction.hex(), "snippet_hex": snippet.hex()})
    for op in OPS:
        for bits in BITS:
            first, modrm = opcodes[op]
            instruction = (b"\x66" if bits == 16 else b"") + bytes((first + (bits != 8), modrm))
            add_record(op, bits, instruction)
    for op, instruction in [("flags_roundtrip", "90"), ("lahf", "9f"), ("sahf", "9e"),
                            ("clc", "f8"), ("stc", "f9"), ("cmc", "f5"), ("mov", "89c8"),
                            ("comiss", "0f2fc1"), ("ucomiss", "0f2ec1"),
                            ("comisd", "660f2fc1"), ("ucomisd", "660f2ec1"),
                            ("fcomi", "dbf1"), ("fucomi", "dbe9"),
                            ("fcomip", "dff1"), ("fucomip", "dfe9")]:
        add_record(op, 32, bytes.fromhex(instruction))
    image = bytearray(1536)
    image[:2] = b"MZ"; struct.pack_into("<I", image, 0x3c, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x14c, 1, 0, 0, 0, 224, 0x102)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x10b)
    for offset, value in {4:1024, 16:0x1000, 20:0x1000, 28:0x400000, 32:0x1000,
                          36:512, 56:0x2000, 60:512, 72:0x100000, 76:0x1000,
                          80:0x100000, 84:0x1000, 92:16}.items():
        struct.pack_into("<I", image, optional + offset, value)
    struct.pack_into("<H", image, optional + 68, 3)
    struct.pack_into("<8sIIIIIIHHI", image, optional + 224, b".text\0\0\0", 1024, 0x1000,
                     1024, 512, 0, 0, 0, 0, 0x60000020)
    image[512:] = code
    (directory / "synthetic.exe").write_bytes(image)
    (directory / "functions.json").write_text(json.dumps({"functions": functions, "iat": {}, "jump_tables": {}}, indent=2))
    (directory / "instructions.json").write_text(json.dumps(records, indent=2))
    return records

def make_cases(records):
    cases = []
    all_flags = [sum(value for bit, value in enumerate(FLAGS.values()) if i & (1 << bit)) for i in range(64)]
    for record in records:
        bits = record["bits"]; mask = (1 << bits) - 1; sign = 1 << (bits - 1)
        boundary = sorted({v & mask for v in (0, 1, 2, 0xf, 0x10, 0x7f, 0x80, 0xff, 0x100, sign-1, sign, sign+1, mask-1, mask)})
        op = record["op"]
        seeds = (0, 1, MASK ^ 1, MASK)
        lefts, rights = boundary, ([0] if op in ("inc", "dec", "neg") else boundary)
        if op in ("flags_roundtrip", "lahf", "clc", "stc", "cmc", "mov"):
            lefts, rights, seeds = [0, 0xa55a5a5a, 0xffffffff], [0x12345678], all_flags
        elif op == "sahf":
            lefts, rights, seeds = [0xa55a005a | (v << 8) for v in range(256)], [0], all_flags
        elif record["index"] >= 31:
            # Finite exactly representable values; NaN/unmasked exceptions are outside this patch test.
            lefts = rights = [0, 1, 2, 0xffffffff, 0xfffffffe]
            seeds = all_flags
        for lhs in lefts:
            for rhs in rights:
                for seed in seeds:
                    cases.append({"id": len(cases), "instruction": record["index"], "op": record["op"], "bits": bits,
                                  "lhs": lhs | (0xa55a5a5a & ~mask), "rhs": rhs | (0x3cc3c3c3 & ~mask), "seed_flags": seed})
    return cases

def main():
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory = ROOT / "local/tests" / f"lifter-flags-{stamp}-{os.getpid()}"
    directory.mkdir(parents=True, exist_ok=False)
    report = {"scope": "synthetic x86_64 host semantic comparison only", "game_execution": False,
              "arm64_or_iphone_proof": False, "vendor_pin": PIN, "host": platform.platform(),
              "python": sys.executable, "status": "started", "optimizations": {},
              "limitations": ["Register forms only; no game data or ARM64 execution",
                  "Defined AF cases only; undefined logic/shift/multiply AF is not asserted",
                  "Finite floating compares only; NaNs and exception delivery are not covered",
                  "Existing function-local Flags and register-only SPILL/RELOAD do not preserve arithmetic flags across calls; not fixed or validated here"]}
    try:
        if os.name != "posix" or platform.machine() != "x86_64":
            raise RuntimeError("Requires existing POSIX x86_64 toolchain (WSL supported)")
        head = subprocess.check_output(["git", "-C", str(VENDOR), "rev-parse", "HEAD"], text=True).strip()
        if head != PIN:
            raise RuntimeError(f"Unexpected vendor revision {head}")
        report["dependencies"] = {name: importlib.metadata.version(name) for name in ("capstone", "pefile")}
        if report["dependencies"] != {"capstone": "5.0.9", "pefile": "2024.8.26"}:
            raise RuntimeError("Approved Capstone 5.0.9 / pefile 2024.8.26 metadata versions required")
        report["patch_sha256"] = sha(PATCH)
        report["source_hashes"] = {str(p.relative_to(ROOT)):sha(p) for p in [VENDOR/"tools/lift.py", VENDOR/"tools/peimg.py", VENDOR/"runtime/x86.h", VENDOR/"runtime/lift_prelude.h", ROOT/"tests/native_x86_flags.c"]}
        records = synthetic_pe(directory)
        cases = make_cases(records)
        report["cases_per_optimization"] = len(cases)
        (directory / "cases.json").write_text(json.dumps(cases, indent=2))
        input_path = directory / "cases.txt"
        input_path.write_text("".join(f"{c['id']} {c['instruction']} {c['lhs']:x} {c['rhs']:x} {c['seed_flags']:x}\n" for c in cases))
        environment = dict(os.environ, LIFT_FILES="1", LIFT_MEMORDER="1", LIFT_TSO="1", PYTHONDONTWRITEBYTECODE="1")
        run(["cc", "--version"], directory, "compiler-version")
        archive = directory / "pinned-source.tar"
        run(["git", "-c", "core.autocrlf=false", "-C", str(VENDOR), "archive", "--format=tar", "-o", str(archive), PIN], directory, "archive-pin")
        report["variant_source_hashes"] = {}
        for variant in ("baseline", "patched"):
            work = directory / variant; work.mkdir()
            source = work / "source"; source.mkdir()
            with tarfile.open(archive) as bundle:
                bundle.extractall(source, filter="data")
            # A fresh local repository isolates git apply from the parent workspace.
            run(["git", "-C", str(source), "init", "-q"], work, "init-private-source")
            run(["git", "-C", str(source), "config", "core.autocrlf", "false"], work, "source-line-endings")
            if variant == "patched":
                run(["git", "-C", str(source), "apply", "--check", str(PATCH)], work, "patch-check")
                run(["git", "-C", str(source), "apply", str(PATCH)], work, "patch-apply")
            report["variant_source_hashes"][variant] = {p:sha(source/p) for p in ["runtime/x86.h", "tools/lift.py", "runtime/lift_prelude.h", "tools/peimg.py"]}
            if variant == "patched" and any(report["variant_source_hashes"]["baseline"][p] == report["variant_source_hashes"]["patched"][p] for p in ("runtime/x86.h", "tools/lift.py")):
                raise RuntimeError("Patch did not modify both required private source files")
            emitted = work / "emitted"
            run([sys.executable, str(source/"tools/lift.py"), str(directory/"synthetic.exe"), str(directory/"functions.json"), str(emitted)], work, "lift", env=environment)
            rejected = json.loads((emitted / "exe_rejected.json").read_text())
            if rejected:
                raise RuntimeError(f"Synthetic functions rejected: {rejected}")
            (emitted / "function_table.inc").write_text("static void (*const lifted_functions[])(CPU *) = {\n" + "".join(f"f_{r['address']:X},\n" for r in records) + "};\n")
            report[variant + "_emitted_sha256"] = sha(emitted / "exe_00.c")
            for optimization in ("O0", "O2"):
                executable = work / f"flags-{optimization}"
                run(["cc", f"-{optimization}", "-std=gnu11", "-mno-red-zone", "-Wall", "-Wextra", "-Werror",
                     "-isystem", str(source/"runtime"), "-I", str(emitted), str(ROOT/"tests/native_x86_flags.c"),
                     str(emitted/"exe_00.c"), "-lm", "-o", str(executable)], work, f"compile-{optimization}")
                run([str(executable)], work, f"native-and-lifted-{optimization}", input_path=input_path)
                rows = [json.loads(line) for line in (work/f"native-and-lifted-{optimization}.stdout").read_text().splitlines()]
                if len(rows) != len(cases) or [r["id"] for r in rows] != list(range(len(cases))):
                    raise RuntimeError("Incomplete/out-of-order harness result stream")
                mismatches = []
                flag_counts = dict.fromkeys(FLAGS, 0)
                result_count = preservation_count = 0
                for case, actual in zip(cases, rows):
                    different = (actual["native_flags"] ^ actual["lifted_flags"]) & MASK
                    result_bad = actual["native_eax"] != actual["lifted_eax"]
                    preservation_bad = actual["lifted_ecx"] != case["rhs"] or actual["lifted_esp"] != 0x108
                    if different or result_bad or preservation_bad:
                        names = [name for name, mask in FLAGS.items() if different & mask]
                        for name in names: flag_counts[name] += 1
                        result_count += result_bad; preservation_count += preservation_bad
                        mismatches.append(dict(case, **actual, differing_defined_flags=names,
                                               result_mismatch=result_bad, wrapper_preservation_mismatch=preservation_bad))
                (work/f"mismatches-{optimization}.json").write_text(json.dumps(mismatches, indent=2) + "\n")
                report["optimizations"][variant + "/" + optimization] = {"cases":len(cases), "mismatches":len(mismatches),
                    "flag_mismatch_counts":flag_counts, "result_mismatches":result_count,
                    "wrapper_preservation_mismatches":preservation_count, "first_mismatches":mismatches[:8]}
        good = all(report["optimizations"]["patched/"+o]["mismatches"] == 0 and report["optimizations"]["baseline/"+o]["mismatches"] > 0 for o in ("O0", "O2"))
        report["status"] = "patched_passed_baseline_mismatch" if good else "regression_or_missing_baseline_failure"
        code = 0 if good else 1
    except Exception as error:
        report["status"] = "harness_failure"; report["error"] = str(error); code = 2
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    summary = {k:{name:value for name,value in v.items() if name != "first_mismatches"} for k,v in report["optimizations"].items()}
    print(json.dumps({"status":report["status"], "report":str(directory/"report.json"), "summary":summary}, indent=2))
    return code

if __name__ == "__main__":
    raise SystemExit(main())
