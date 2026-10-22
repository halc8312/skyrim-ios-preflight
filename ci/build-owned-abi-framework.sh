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
build="$probe/owned-abi-build"
framework="$probe/Frameworks/SyntheticGuest.framework"
test ! -e "$build"
test ! -e "$framework"
mkdir -p "$build" "$framework/Headers"
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
  "${objects[@]}" -o "$framework/SyntheticGuest"
# These are pure Clang Mach-O objects, so Apple's SDK nm can read all members.
xcrun nm -u "$framework/SyntheticGuest" > "$build/undefined.txt"
test ! -s "$build/undefined.txt"
xcrun nm -gUj "$framework/SyntheticGuest" | LC_ALL=C sort > "$build/exports.actual"
LC_ALL=C sort "$build/exports.list" > "$build/exports.expected"
cmp "$build/exports.expected" "$build/exports.actual"
xcrun vtool -show-build "$framework/SyntheticGuest"
python3 - "$framework" "$platform_name" "$platform_id" <<'PY'
from pathlib import Path
import plistlib, struct, sys
framework = Path(sys.argv[1])
blob = (framework / 'SyntheticGuest').read_bytes()
magic, cpu, subtype, kind, count, commands, flags, reserved = struct.unpack_from('<8I', blob)
assert (magic, cpu, subtype, kind) == (0xfeedfacf, 0x100000c, 0, 6)
assert flags & 1 and flags & 0x80  # MH_NOUNDEFS and MH_TWOLEVEL.
offset = 32
platforms = []
for _ in range(count):
    cmd, size = struct.unpack_from('<II', blob, offset)
    assert size >= 8 and offset + size <= 32 + commands
    if cmd == 0x32:
        platforms.append(struct.unpack_from('<I', blob, offset + 8)[0])
    offset += size
assert platforms == [int(sys.argv[3])], platforms
assert offset == 32 + commands
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
