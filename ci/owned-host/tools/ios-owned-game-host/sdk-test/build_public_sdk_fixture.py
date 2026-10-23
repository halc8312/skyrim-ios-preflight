"""Compile only authored public V1/V2 SDK entries with the real generic adapter.

No signing, private/generated guest input, Rust build, or SDK execution proof.
The existing public Mach-O link validator is supplied by the frozen CI harness.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import sys
import time

HERE = Path(__file__).resolve().parent
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
sdk = load("public_fixture_sdk_helpers", HERE / "run_sdk_tests.py")
BASE_EXPORTS = ("PG_PublishMemoryV1", "PG_InitContextV1", "PG_CallGuestV1", "skyrt_ppc_config", "skyrt_func_addrs")

def exports(version):
    if type(version) is not int or version not in (1, 2):
        raise ValueError("explicit ABI 1 or 2 required")
    return sorted("_" + name for name in (f"PG_GetRequirementsV{version}", f"PG_BindHostV{version}", *BASE_EXPORTS))

def validate_code_evidence(version, actual_exports, undefined, ir, symbols, disassembly):
    if actual_exports.splitlines() != exports(version) or undefined.strip():
        raise ValueError("exact seven selected exports and zero imports required")
    attributes = dict(re.findall(r"attributes #(\d+) = \{([^\n]*)", ir))
    declaration = re.search(r"^declare[^\n]*@setjmp\([^\n]*#(\d+)", ir, re.M)
    calls = re.findall(r"\b(?:call|invoke)[^\n]*@setjmp\([^\n]*#(\d+)", ir)
    if (not declaration or not calls or "returns_twice" not in attributes.get(declaration[1], "")
            or any("returns_twice" not in attributes.get(n, "") for n in calls)):
        raise ValueError("actual setjmp declaration/callsite must be returns_twice")
    for name in ("_setjmp", "_longjmp", "__Unwind_Resume"):
        if not re.search(r"(?m)^[0-9a-fA-F]+\s+[tT]\s+" + re.escape(name) + r"$", symbols):
            raise ValueError("local native tail missing")
        block = re.search(r"(?m)^" + re.escape(name) + r":\n((?:[0-9a-fA-F]{16}\t[^\n]*\n)+)", disassembly)
        if not block:
            raise ValueError("tail disassembly missing")
        lines = [line.split("\t")[1:] for line in block[1].splitlines()]
        operations = {line[0] for line in lines}
        if ("br" not in operations or operations & {"blr", "ret", "stp", "ldp", "sub"}
                or any(re.search(r"\b(?:sp|x29|x30)\b", " ".join(line)) for line in lines)
                or not re.search(r"\tbl\t" + re.escape(name) + r"(?:\n|$)", disassembly)):
            raise ValueError("native tail/callsite shape differs")
    if version == 2:
        for name in ("skyrt_xma_mmio_read_u32", "skyrt_xma_mmio_write_u32"):
            instruction = (r"(?m)^[ \t]*(?:%[-A-Za-z0-9$._]+\s*=\s*)?"
                           r"(?:(?:tail|musttail|notail)\s+)?(?:call|invoke)\b"
                           r"[^;\r\n]*\bi1[ \t]+@" + name + r"[ \t]*\(")
            if not re.search(instruction, ir):
                raise ValueError("actual V2 fixture must call both MMIO forwarders")

def source_plan(kit, version):
    exports(version)
    generic = kit / "tools/windows-ios-guest-probe/owned-guest-abi"
    host = kit / "tools/ios-owned-game-host"
    if version == 1:
        return [(generic / "owned_abi.cpp", "owned_abi.o"),
                (host / "native/PublicOwnedFixture.cpp", "fixture.o")]
    return [(generic / "owned_abi_v2.cpp", "owned_abi.o"),
            (generic / "private_layout_v2.cpp", "private_layout.o"),
            (host / "sdk-test/native/PublicOwnedFixtureV2.cpp", "fixture.o")]

def prepare_header(kit, version):
    exports(version)
    backend = kit / "tools/ios-resident-guest-probe/apple-backend"
    generic = kit / "tools/windows-ios-guest-probe/owned-guest-abi"
    atomic = load("public_fixture_atomic_overlay", backend / "prepare_fixture.py")
    original = (kit / "skyrim-recomp-web/cpp/ppc_context.h").read_bytes()
    header = atomic.overlay(original.decode().replace("\r\n", "\n"))
    if version == 2:
        old_path = sys.path.copy()
        sys.path[:0] = [str(generic), str(generic.parent)]
        try:
            mmio = load("public_fixture_mmio_overlay", generic / "mmio_overlay.py")
            header = mmio.apply_mmio_overlay(header)
        finally:
            sys.path[:] = old_path
    return original, header

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("kit", "simde", "output", "link-validator"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--platform", choices=("device", "simulator"), required=True)
    parser.add_argument("--abi-version", type=int, choices=(1, 2), default=1)
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        raise RuntimeError("public fixture compilation requires the Apple SDK")
    kit, simde, validator_path = (p.resolve(strict=True) for p in (args.kit, args.simde, args.link_validator))
    output = args.output.resolve()
    if any(output.is_relative_to(p) or p.is_relative_to(output) for p in (kit, simde)) or output.exists():
        raise ValueError("fresh public output outside input trees required")
    validator = load("public_fixture_link_validator", validator_path)
    generic = kit / "tools/windows-ios-guest-probe/owned-guest-abi"
    original, header = prepare_header(kit, args.abi_version)
    output.mkdir()
    (output / "include").mkdir()
    (output / "include/ppc_context.h").write_text(header)
    (output / "include/ppc_config.h").write_text(
        "#pragma once\n#define PPC_CONFIG_H_INCLUDED\n#define PPC_IMAGE_BASE 0x82000000u\n"
        "#define PPC_IMAGE_SIZE 0x2000u\n#define PPC_CODE_BASE 0x82001000u\n#define PPC_CODE_SIZE 0x10u\n")
    deadline = time.monotonic() + 180
    def run(command, name, cap=30):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("public fixture build deadline")
        code = sdk.run_bounded(command, output, output / name, min(cap, remaining))
        sdk.require_sdk_command_success(code, output / name, "public fixture SDK command failed: " + name)
    sdk_name = "iphoneos" if args.platform == "device" else "iphonesimulator"
    target = "arm64-apple-ios26.0" + ("-simulator" if args.platform == "simulator" else "")
    platform = 2 if args.platform == "device" else 7
    run(["xcrun", "--sdk", sdk_name, "--show-sdk-path"], "sdk-path", 15)
    sdk_path = (output / "sdk-path.stdout.log").read_text().strip()
    if not sdk_path or "\n" in sdk_path or not Path(sdk_path).is_dir():
        raise ValueError("single installed SDK path required")
    clang = ["xcrun", "--sdk", sdk_name, "clang++", "-target", target, "-isysroot", sdk_path]
    (output / "sdk-load-probe.c").write_text("int main(void) { return 0; }\n")
    run(["xcrun", "--sdk", sdk_name, "clang", "-target", target, "-isysroot", sdk_path,
         "-O2", str(output / "sdk-load-probe.c"), "-o", str(output / "sdk-load-probe")], "sdk-load-probe-build")
    reference = validator.inspect((output / "sdk-load-probe").read_bytes(), 2, platform)
    loads = reference["dylibs"]
    if len(loads) != 1 or loads[0][:2] != (0xC, "/usr/lib/libSystem.B.dylib") or min(loads[0][2:]) < 0x10000:
        raise ValueError("installed SDK libSystem load required")
    def version(word):
        return f"{word >> 16}.{word >> 8 & 255}.{word & 255}"
    template = (generic / "system-load-metadata.tbd").read_text()
    for key, word in zip(("current-version", "compatibility-version"), loads[0][2:]):
        anchor = key + ": 1.0\n"
        if template.count(anchor) != 1:
            raise ValueError("public load metadata template anchor mismatch")
        template = template.replace(anchor, key + ": " + version(word) + "\n")
    (output / "system-load-metadata.tbd").write_text(template)
    flags = ["-std=c++20", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
             "-fno-rtti", "-fexceptions", "-funwind-tables", "-ffp-model=strict", "-fvisibility=hidden",
             "-DSIMDE_ENABLE_NATIVE_ALIASES", "-I" + str(output / "include"), "-I" + str(generic), "-I" + str(simde)]
    if args.abi_version == 2:
        flags.append("-DSKYRT_XMA_MMIO_BRIDGE=1")
    plan = source_plan(kit, args.abi_version)
    for source, name in plan:
        run(clang + flags + ["-c", str(source), "-o", str(output / name)], "compile-" + name)
    run(clang + flags + ["-S", "-emit-llvm", str(plan[-1][0]), "-o", str(output / "fixture.ll")], "fixture-ir")
    run(clang + ["-c", str(generic / "jump_tail.S"), "-o", str(output / "jump_tail.o")], "jump-tail")
    framework = output / "PrivateGuest.framework"
    framework.mkdir()
    binary = framework / "PrivateGuest"
    (output / "exports.txt").write_text("\n".join(exports(args.abi_version)) + "\n")
    run(clang + ["-dynamiclib", "-nostdlib", *(str(output / name) for _, name in plan), str(output / "jump_tail.o"),
        "-Wl,-install_name,@rpath/PrivateGuest.framework/PrivateGuest", "-Wl,-exported_symbols_list," + str(output / "exports.txt"),
        "-Wl,-undefined,error", "-Wl,-no_adhoc_codesign", "-Wl,-needed_library," + str(output / "system-load-metadata.tbd"),
        "-o", str(binary)], "link")
    (framework / "Info.plist").write_bytes(plistlib.dumps(dict(CFBundleExecutable="PrivateGuest",
        CFBundleIdentifier="local.public.OwnedFixtureV" + str(args.abi_version), CFBundleName="PrivateGuest",
        CFBundlePackageType="FMWK", CFBundleShortVersionString="0.1", CFBundleVersion="1", MinimumOSVersion="26.0",
        CFBundleSupportedPlatforms=["iPhoneOS" if platform == 2 else "iPhoneSimulator"])))
    for name, command in [("exports", ["nm", "-gjU"]), ("undefined", ["nm", "-u"]),
                           ("symbols", ["nm", "-a"]), ("disassembly", ["otool", "-tvV"])]:
        run(["xcrun", *command, str(binary)], name)
    actual = "\n".join(sorted((output / "exports.stdout.log").read_text().splitlines())) + "\n"
    (output / "actual-exports.txt").write_text(actual)
    validate_code_evidence(args.abi_version, actual, (output / "undefined.stdout.log").read_text(),
        (output / "fixture.ll").read_text(), (output / "symbols.stdout.log").read_text(),
        (output / "disassembly.stdout.log").read_text())
    report = validator.verify(binary.read_bytes(), (output / "sdk-load-probe").read_bytes(), platform)
    report.update(abi_version=args.abi_version << 16, public_header_sha256=hashlib.sha256(original).hexdigest(),
                  sdk_runtime_executed=False)
    (output / "link-metadata.json").write_text(json.dumps(report, indent=2))
    if (kit / "skyrim-recomp-web/cpp/ppc_context.h").read_bytes() != original:
        raise ValueError("public source header changed")
    print(json.dumps(report, sort_keys=True))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
