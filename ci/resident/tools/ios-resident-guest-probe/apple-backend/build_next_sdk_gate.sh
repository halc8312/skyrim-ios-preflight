#!/bin/bash
# A future source-only Mac SDK gate. This script is NOT a cloud dispatch and
# deliberately does not sign/run/package an app or alter an existing probe.
set -euo pipefail
if [[ $# -ne 5 ]]; then
  echo 'usage: build_next_sdk_gate.sh PUBLIC_SOURCE GENERIC_ABI SIMDE NEW_OUTPUT device|simulator' >&2
  exit 2
fi
here="$(cd "$(dirname "$0")" && pwd)"
source="$(cd "$1" && pwd)"
generic="$(cd "$2" && pwd)"
simde="$(cd "$3" && pwd)"
out="$4"
case "$5" in
  device) sdk=iphoneos; target=arm64-apple-ios26.0; rust_target=aarch64-apple-ios; platform=2;;
  simulator) sdk=iphonesimulator; target=arm64-apple-ios26.0-simulator; rust_target=aarch64-apple-ios-sim; platform=7;;
  *) exit 2;;
esac
test "$(uname -s)" = Darwin
test ! -e "$out"
test "$source" = "$(cd "$here/../../../skyrim-recomp-web" && pwd)"
mkdir "$out"
out="$(cd "$out" && pwd)"
python3 "$here/prepare_fixture.py" --source "$source" --generic "$generic" --output "$out/include"
sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
clang="$(xcrun --sdk "$sdk" --find clang++)"
common=(-target "$target" -isysroot "$sdk_path" -std=c++20 -O2 -fPIC -fexceptions -funwind-tables -ffp-model=strict -fno-builtin -fno-stack-protector -fvisibility=hidden -DSIMDE_ENABLE_NATIVE_ALIASES -I"$out/include" -I"$generic" -I"$simde" -I"$here/native")
"$clang" "${common[@]}" -c "$generic/owned_abi.cpp" -o "$out/owned_abi.o"
"$clang" -target "$target" -isysroot "$sdk_path" -c "$generic/jump_tail.S" -o "$out/jump_tail.o"
"$clang" "${common[@]}" -c "$here/native/ResidentAppleFixture.cpp" -o "$out/fixture.o"
"$clang" "${common[@]}" -S -emit-llvm "$here/native/ResidentAppleFixture.cpp" -o "$out/fixture.ll"
framework="$out/ResidentAppleFixture.framework"
mkdir "$framework"
printf '%s\n' _PG_GetRequirementsV1 _PG_BindHostV1 _PG_PublishMemoryV1 _PG_InitContextV1 _PG_CallGuestV1 _skyrt_ppc_config _skyrt_func_addrs > "$out/exports.txt"
"$clang" -target "$target" -isysroot "$sdk_path" -dynamiclib "$out/owned_abi.o" "$out/jump_tail.o" "$out/fixture.o" -Wl,-install_name,@rpath/ResidentAppleFixture.framework/ResidentAppleFixture -Wl,-exported_symbols_list,"$out/exports.txt" -Wl,-undefined,error -Wl,-no_adhoc_codesign -o "$framework/ResidentAppleFixture"
python3 - "$framework" <<'PY'
from pathlib import Path
import plistlib,sys
path=Path(sys.argv[1])
(path/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':'ResidentAppleFixture','CFBundleIdentifier':'local.public.ResidentAppleFixture','CFBundleName':'ResidentAppleFixture','CFBundlePackageType':'FMWK','CFBundleShortVersionString':'0.1','CFBundleVersion':'1','MinimumOSVersion':'26.0'}))
PY
# Compile the native provider separately for the future test app. It is not in
# the guest image, so native SDK pointers cannot resolve to that image's shims.
"$clang" "${common[@]}" -fobjc-arc -c "$here/native/NativeHost.mm" -o "$out/NativeHost.o"
test -f "$here/Cargo.lock"
# The standalone Cargo paths intentionally require the public workspace at its
# normal relative location. CI must copy the whole allowed public layout.
CARGO_TARGET_DIR="$out/cargo-target" cargo +nightly-2026-10-11 build --locked --release --lib --features apple-runtime --target "$rust_target" --manifest-path "$here/Cargo.toml"
xcrun nm -gjU "$framework/ResidentAppleFixture" | LC_ALL=C sort > "$out/actual-exports.txt"
LC_ALL=C sort "$out/exports.txt" > "$out/expected-exports.txt"
cmp "$out/actual-exports.txt" "$out/expected-exports.txt"
xcrun nm -u "$framework/ResidentAppleFixture" > "$out/undefined.txt"
xcrun nm -a "$framework/ResidentAppleFixture" > "$out/symbols.txt"
xcrun otool -tvV "$framework/ResidentAppleFixture" > "$out/disassembly.txt"
python3 - "$out" <<'PY'
import re,sys
from pathlib import Path
out=Path(sys.argv[1]);ir=(out/'fixture.ll').read_text()
attrs=dict(re.findall(r'attributes #(\d+) = \{([^\n]*)',ir))
declaration=re.search(r'^declare[^\n]*@setjmp\([^\n]*#(\d+)',ir,re.M)
calls=re.findall(r'\b(?:call|invoke)[^\n]*@setjmp\([^\n]*#(\d+)',ir)
if not declaration or 'returns_twice' not in attrs.get(declaration[1],'') or not calls or any('returns_twice' not in attrs.get(number,'') for number in calls):
    raise SystemExit('actual fixture setjmp declaration/callsite lacks returns_twice')
undefined=(out/'undefined.txt').read_text();symbols=(out/'symbols.txt').read_text()
for name in ('_setjmp','_longjmp','__Unwind_Resume'):
    if re.search(r'(?m)\b'+re.escape(name)+r'$',undefined) or not re.search(r'(?m)^[0-9a-fA-F]+\s+[tT]\s+'+re.escape(name)+r'$',symbols):
        raise SystemExit('owned jump/unwind tail not locally defined: '+name)
# Disassembly is retained as public source evidence for independent BR-only and
# resolved-callsite review. This script does not infer runtime execution from it.
PY
echo "SDK_COMPILE_ONLY platform=$platform; SDK runtime/XCTest/signing required next"
