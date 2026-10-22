#!/usr/bin/env bash
# Only generic adapter code and a hand-written synthetic dispatcher are inputs.
# No title-derived layout, function list, object file, or game framework enters.
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 2
probe="$1"
platform="$2"
case "$platform" in
  iphoneos) target=arm64-apple-ios26.0; platform_name=iPhoneOS; platform_id=2 ;;
  iphonesimulator) target=arm64-apple-ios26.0-simulator; platform_name=iPhoneSimulator; platform_id=7 ;;
  *) printf '%s\n' 'Expected iphoneos or iphonesimulator.' >&2; exit 2 ;;
esac
harness_root="$(cd "$(dirname "$0")/.." && pwd)"
source="$harness_root/ios-probe/owned-abi"
sdk="$(xcrun --sdk "$platform" --show-sdk-path)"
compiler="$(xcrun --sdk "$platform" --find clang++)"
c_compiler="$(xcrun --sdk "$platform" --find clang)"
build="$probe/owned-abi-build"
framework="$probe/Frameworks/SyntheticGuest.framework"
test ! -e "$build"
test ! -e "$framework"
mkdir -p "$build" "$framework/Headers"
# Inspect load metadata from a normal link against the actual installed SDK.
# Its self-authored tiny executable stays here; no SDK stub is copied out.
printf '%s\n' 'int main(void) { return 0; }' > "$build/sdk-load-probe.c"
"$c_compiler" -target "$target" -isysroot "$sdk" -O2 \
  "$build/sdk-load-probe.c" -o "$build/sdk-load-probe"
metadata_stub="$source/system-load-metadata.tbd"
test -f "$metadata_stub"
cp "$source/PrivateGuestABI.h" "$framework/Headers/"
cp "$source/SyntheticGuestContext.h" "$framework/Headers/"
flags=(-target "$target" -isysroot "$sdk" -O2 -std=c++20
       -ffreestanding -fno-builtin -fno-stack-protector -fno-rtti
       -fexceptions -funwind-tables -fvisibility=hidden -ffp-model=strict
       -I "$source" -I "$probe/Support" -I "$probe/Support/simde"
       -I "$probe/cpu-fixture")
objects=()
for name in owned_abi.cpp SyntheticLayout.cpp jump_tail.S; do
  test -f "$source/$name"
  object="$build/$name.o"
  if [[ "$name" == *.cpp ]]; then
    "$compiler" "${flags[@]}" -c "$source/$name" -o "$object"
  else
    "$compiler" -target "$target" -isysroot "$sdk" -c "$source/$name" -o "$object"
  fi
  objects+=("$object")
done
# Freestanding compilation disables the builtin's implicit returns_twice.
# Verify the actual SDK call site, not just an unrelated declaration.
"$compiler" "${flags[@]}" -S -emit-llvm "$source/SyntheticLayout.cpp" -o "$build/synthetic-layout.ll"
python3 - "$build/synthetic-layout.ll" <<'PY'
from pathlib import Path
import re, sys
ir = Path(sys.argv[1]).read_text()
name = re.compile(r'@(?:"(?:\\01)?_?setjmp"|_?setjmp)\(')
attributes = dict(re.findall(r'^attributes #(\d+) = \{([^\n]*)\}', ir, re.M))
declarations = [line for line in ir.splitlines() if line.startswith('declare ') and name.search(line)]
calls = [line for line in ir.splitlines() if name.search(line) and re.search(r'\b(call|invoke)\b', line)]
def returns_twice(line):
    return bool(re.search(r'\breturns_twice\b', line)) or any(
        re.search(r'\breturns_twice\b', attributes.get(group, ''))
        for group in re.findall(r'#(\d+)\b', line))
assert len(declarations) == len(calls) == 1, (declarations, calls)
assert returns_twice(declarations[0]) and returns_twice(calls[0]), 'setjmp returns_twice is missing'
print('Synthetic SDK setjmp declaration and actual O2 call site retain returns_twice.')
PY
printf '%s\n' _PG_GetRequirementsV1 _PG_BindHostV1 _PG_PublishMemoryV1 \
  _PG_InitContextV1 _PG_CallGuestV1 > "$build/exports.list"
"$compiler" -target "$target" -isysroot "$sdk" -dynamiclib -nostdlib \
  -Wl,-undefined,error -Wl,-no_adhoc_codesign \
  -Wl,-install_name,@rpath/SyntheticGuest.framework/SyntheticGuest \
  -Wl,-exported_symbols_list,"$build/exports.list" \
  "${objects[@]}" "-Wl,-needed_library,$metadata_stub" -o "$framework/SyntheticGuest"
# The empty metadata stub supplies no implementation. Retain an intentionally
# missing import and require Apple's strict linker to reject that exact symbol.
cat > "$build/missing-import.cpp" <<'CPP'
extern "C" void probe_intentionally_missing_sdk_import(void);
extern "C" void probe_force_missing_import(void) {
    probe_intentionally_missing_sdk_import();
}
CPP
"$compiler" "${flags[@]}" -c "$build/missing-import.cpp" -o "$build/missing-import.o"
if "$compiler" -target "$target" -isysroot "$sdk" -dynamiclib -nostdlib \
  -Wl,-undefined,error -Wl,-no_adhoc_codesign \
  -Wl,-install_name,@rpath/SyntheticGuest.framework/SyntheticGuest \
  -Wl,-exported_symbols_list,"$build/exports.list" \
  -Wl,-u,_probe_intentionally_missing_sdk_import \
  "${objects[@]}" "$build/missing-import.o" "-Wl,-needed_library,$metadata_stub" \
  -o "$build/negative-must-not-link" > "$build/negative-link.stdout.log" 2> "$build/negative-link.stderr.log"; then
  printf '%s\n' 'Unexpected success: metadata stub resolved a missing import.' >&2
  exit 1
fi
python3 - "$build/negative-link.stderr.log" <<'PY'
from pathlib import Path
import re, sys
error = Path(sys.argv[1]).read_text()
assert re.search(r'undefined symbols?', error, re.I), 'negative link failed for an unexpected reason'
assert re.search(r'(?m)^\s*["\']?_probe_intentionally_missing_sdk_import["\']?(?:,|\s|$)', error), 'the intended missing import was not diagnosed'
print('Strict negative link rejected the intentionally missing import.')
PY
# These are pure Clang Mach-O objects, so Apple's SDK nm can read all members.
xcrun nm -u "$framework/SyntheticGuest" > "$build/undefined.txt"
test ! -s "$build/undefined.txt"
xcrun nm -gUj "$framework/SyntheticGuest" | LC_ALL=C sort > "$build/exports.actual"
LC_ALL=C sort "$build/exports.list" > "$build/exports.expected"
cmp "$build/exports.expected" "$build/exports.actual"
xcrun vtool -show-build "$framework/SyntheticGuest"
python3 - "$framework" "$platform_name" "$platform_id" "$build/sdk-load-probe" <<'PY'
from pathlib import Path
import plistlib, struct, sys
framework = Path(sys.argv[1])
system_name = '/usr/lib/libSystem.B.dylib'

def inspect(path, expected_kind):
    blob = path.read_bytes()
    assert len(blob) >= 32
    magic, cpu, subtype, kind, count, command_bytes, flags, reserved = struct.unpack_from('<8I', blob)
    assert (magic, cpu, subtype, kind) == (0xfeedfacf, 0x100000c, 0, expected_kind)
    end = 32 + command_bytes
    assert end <= len(blob) and count <= command_bytes // 8
    offset, platforms, dylibs, binds, chained, rebases, signatures = 32, [], [], [], [], [], []
    undefined_counts = []
    for _ in range(count):
        assert offset + 8 <= end
        cmd, size = struct.unpack_from('<II', blob, offset)
        assert size >= 8 and offset + size <= end
        if cmd == 0x32:  # LC_BUILD_VERSION
            assert size >= 24
            platforms.append(struct.unpack_from('<I', blob, offset + 8)[0])
        if cmd in (0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20):
            # LC_LOAD_DYLIB, weak, reexport, upward and lazy load variants.
            assert size >= 24
            name_at, stamp, current, compatible = struct.unpack_from('<4I', blob, offset + 8)
            assert 24 <= name_at < size
            name_end = blob.find(b'\0', offset + name_at, offset + size)
            assert name_end >= 0
            name = blob[offset + name_at:name_end].decode('utf-8')
            dylibs.append((cmd, name, current, compatible))
        if cmd in (0x22, 0x80000022):  # LC_DYLD_INFO[_ONLY]
            assert size >= 48
            values = struct.unpack_from('<10I', blob, offset + 8)
            for location, length in zip(values[::2], values[1::2]):
                assert not length or (location <= len(blob) and length <= len(blob) - location)
            rebases.append(values[1])
            binds.extend((values[3], values[5], values[7]))
        if cmd == 0x80000034:  # LC_DYLD_CHAINED_FIXUPS
            assert size >= 16
            location, length = struct.unpack_from('<II', blob, offset + 8)
            assert length >= 28 and location <= len(blob) and length <= len(blob) - location
            version, starts, imports, symbols, nimports, fmt, symfmt = struct.unpack_from('<7I', blob, location)
            assert version == 0 and all(value <= length for value in (starts, imports, symbols))
            chained.append(nimports)
        if cmd == 0xB:  # LC_DYSYMTAB
            assert size >= 80
            undefined_counts.append(struct.unpack_from('<I', blob, offset + 28)[0])
        if cmd == 0x1D:  # LC_CODE_SIGNATURE
            signatures.append(cmd)
        offset += size
    assert offset == end
    assert platforms == [int(sys.argv[3])], platforms
    return flags, dylibs, binds, chained, rebases, undefined_counts, signatures

# Confirm metadata version 1.0 against a normal link using the installed SDK.
sdk_flags, sdk_dylibs, *_ = inspect(Path(sys.argv[4]), 2)
sdk_system = [item for item in sdk_dylibs if item[1] == system_name]
assert len(sdk_system) == 1 and sdk_system[0][0] == 0xC, sdk_system
assert sdk_system[0][2] >= 0x10000 and sdk_system[0][3] >= 0x10000, sdk_system

flags, dylibs, binds, chained, rebases, undefined_counts, signatures = inspect(framework / 'SyntheticGuest', 6)
assert flags & 1 and flags & 0x80  # MH_NOUNDEFS and MH_TWOLEVEL.
assert dylibs == [(0xC, system_name, 0x10000, 0x10000)], dylibs
assert not any(binds) and not any(chained) and not any(undefined_counts)
assert not signatures
print('SDK libSystem current/compatibility:', sdk_system[0][2:])
print('Synthetic load metadata: one nonweak/nonreexport libSystem; bind/import counts zero; local rebase bytes:', rebases)
with (framework / 'Info.plist').open('xb') as stream:
    plistlib.dump({
        'CFBundleExecutable': 'SyntheticGuest',
        'CFBundleIdentifier': 'com.halc8312.syntheticguest',
        'CFBundleName': 'SyntheticGuest', 'CFBundlePackageType': 'FMWK',
        'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0',
        'MinimumOSVersion': '26.0', 'CFBundleSupportedPlatforms': [sys.argv[2]],
    }, stream)
PY
printf '%s\n' 'Synthetic framework: strict link, zero undefined symbols, exactly five PG API exports.'
