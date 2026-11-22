#!/bin/bash
# Real Apple SDK check of the guest hook library's thread-local frame receipt.
#
# Source only: the library, its authored fixture headers and the existing frame
# receipt test. No game data, generated guest code, signing material or device
# identifier is used. This does not build the real guest framework; it checks
# the one construct the Windows cross-link cannot validate with Apple's tools.
#
# 1. The library compiles and links for iPhoneOS arm64 with Apple's linker.
# 2. The linked image imports __tlv_bootstrap, and build_title_guest.py's import
#    gate accepts it for this object (nm output format of the real SDK).
# 3. The same sources run in an iOS Simulator, where the receipt test checks that
#    another thread cannot see this thread's frame receipt.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/guest-tls-sdk.XXXXXXXX")"
abi="$root/ios/private-abi"
fixture="$root/tests/native/test_highheel_frame_receipt.c"
library="$abi/pc_guest_mod_hooks_library.c"

xcrun --sdk iphoneos --show-sdk-version
clang_version="$(xcrun --sdk iphoneos clang --version)"
echo "${clang_version%%$'\n'*}"

mkdir "$work/headers"
(cd "$root/tests" && python3 -B - "$work/headers" <<'PY'
from pathlib import Path
import sys
from test_mod_hooks_binding import write_fixture_headers
write_fixture_headers(Path(sys.argv[1]))
PY
)

# The scene group is the one that owns the thread-local receipt.
flags=(-std=gnu11 -O2 -Wall -Wextra -Werror -DPC_GUEST_MOD_HOOKS_COMPILED=65536u
       -I "$abi" -I "$root/experiments/fuz-voice" -I "$work/headers")

device_sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
device_target=arm64-apple-ios18.0
xcrun --sdk iphoneos clang -target "$device_target" -isysroot "$device_sdk" "${flags[@]}" \
    -c "$library" -o "$work/pc_guest_mod_hooks_library.o"
xcrun --sdk iphoneos clang -target "$device_target" -isysroot "$device_sdk" "${flags[@]}" \
    "$fixture" "$library" -o "$work/receipt-device"

# Read each tool's whole output before matching: grep -q can end early and
# make an upstream tool fail with SIGPIPE under pipefail.
build_info="$(xcrun vtool -show-build "$work/receipt-device")"
echo "$build_info"
grep -Eq 'platform +IOS$' <<<"$build_info"
undefined_symbols="$(xcrun nm -u "$work/receipt-device")"
grep -Eq '^ *__tlv_bootstrap$' <<<"$undefined_symbols"
load_commands="$(xcrun otool -l "$work/receipt-device")"
grep -Fq '__thread_vars' <<<"$load_commands"

# The guest build's own gate must accept the symbol from this object, using the
# real SDK's nm, and must not widen the allowance to anything else.
python3 -B - "$root/scripts" "$work/pc_guest_mod_hooks_library.o" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
import build_title_guest as builder

tool = builder.toolchain()
assert tool["official_apple_sdk_used"], "The real iPhoneOS SDK is required"
imports, unknown = builder.import_symbols(tool, [Path(sys.argv[2])])
assert "__tlv_bootstrap" in imports, imports
assert "__tlv_bootstrap" not in unknown, unknown
print("GUEST_TLS_SDK: import gate accepts __tlv_bootstrap for the hook library only")
PY

simulator_sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
xcrun --sdk iphonesimulator clang -target "$(uname -m)-apple-ios18.0-simulator" -isysroot "$simulator_sdk" \
    "${flags[@]}" "$fixture" "$library" -o "$work/receipt-simulator"

device="$(python3 "$root/tests/mod-simulator/prepare.py" device)"
[[ "$device" =~ ^[0-9A-Fa-f-]{36}$ ]]
trap 'xcrun simctl shutdown "$device" >/dev/null 2>&1 || true' EXIT
xcrun simctl boot "$device"
xcrun simctl bootstatus "$device" -b
xcrun simctl spawn "$device" "$work/receipt-simulator"

echo "GUEST_TLS_SDK: iPhoneOS link imports __tlv_bootstrap from libSystem, and the thread-local frame receipt passed in an iOS Simulator."
