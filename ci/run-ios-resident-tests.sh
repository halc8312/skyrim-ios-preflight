#!/usr/bin/env bash
# Separate public synthetic resident app: one Rust runtime, one fresh process.
# No private game framework, layout, translated code, XEX or key is an input.
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 2
work="$1"
simde="$2"
harness="$(cd "$(dirname "$0")/.." && pwd)"
kit="$work/resident-public-kit"
output="$harness/out/resident-sdk"
test ! -e "$output"
mkdir "$output"
xcodebuild -version > "$output/apple-tools.txt"
xcrun --sdk iphoneos --show-sdk-version >> "$output/apple-tools.txt"
xcrun --sdk iphonesimulator --show-sdk-version >> "$output/apple-tools.txt"
rustc --version >> "$output/apple-tools.txt"
python3 "$harness/ci/prepare-resident-kit.py" "$harness/ci/resident" "$kit" \
  | tee "$output/public-stage.log"
cp "$harness/ci/resident/public-source-manifest.json" "$output/"
backend="$kit/tools/ios-resident-guest-probe/apple-backend"
generic="$kit/tools/windows-ios-guest-probe/owned-guest-abi"
source="$kit/skyrim-recomp-web"
python3 -m unittest discover -s "$backend" -p 'test_prepare_fixture.py' -v \
  2>&1 | tee "$output/public-header-cli-tests.log"
rustup target add --toolchain "$RUSTUP_TOOLCHAIN" aarch64-apple-ios aarch64-apple-ios-sim
CARGO_TARGET_DIR="$work/resident-pure-target" \
  cargo +nightly-2026-10-11 test --locked --no-default-features \
    --manifest-path "$backend/Cargo.toml" -- --test-threads=1 \
    2>&1 | tee "$output/public-policy-tests.log"
python3 -m unittest discover -s "$backend/sdk-test" -p 'test_*.py' -v \
  2>&1 | tee "$output/public-driver-tests.log"
for variant in device simulator; do
  bash "$backend/build_next_sdk_gate.sh" "$source" "$generic" "$simde" \
    "$work/resident-$variant" "$variant" 2>&1 | tee "$output/build-$variant.log"
  xcrun vtool -show-build "$work/resident-$variant/ResidentAppleFixture.framework/ResidentAppleFixture" \
    > "$output/platform-$variant.txt"
  cp "$work/resident-$variant/ResidentAppleFixture.framework/ResidentAppleFixture" \
    "$output/ResidentAppleFixture-$variant-unsigned.bin"
  for record in fixture.ll actual-exports.txt undefined.txt symbols.txt disassembly.txt; do
    cp "$work/resident-$variant/$record" "$output/$variant-$record"
  done
done

# SDK test sources are copied individually by the exact allowlist above. Only
# the resulting simulator Rust archive enters this new app (no existing bridge).
stage="$backend/sdk-test"
mkdir -p "$stage/include" "$stage/native" "$stage/Libraries" "$stage/Frameworks" "$stage/Resources"
cp "$generic/PrivateGuestABI.h" "$stage/include/"
cp "$backend/native/ResidentAppleProbe.h" "$stage/include/"
cp "$backend/native/NativeHost.h" "$backend/native/NativeHost.mm" "$stage/native/"
cp "$work/resident-simulator/cargo-target/aarch64-apple-ios-sim/release/libios_resident_apple_backend.a" \
  "$stage/Libraries/"
cp -R "$work/resident-simulator/ResidentAppleFixture.framework" "$stage/Frameworks/"
framework="$stage/Frameworks/ResidentAppleFixture.framework"
/usr/bin/codesign --force --sign - --timestamp=none "$framework" > "$output/codesign.log" 2>&1
/usr/bin/codesign --verify --strict --verbose=2 "$framework" > "$output/codesign-verify.log" 2>&1
cp "$framework/ResidentAppleFixture" "$output/ResidentAppleFixture-simulator-signed.bin"
# This trusted input is independently captured after signing and before app
# packaging. Runtime must never derive its expected hash from the candidate.
python3 - "$framework/ResidentAppleFixture" "$stage/Resources/ResidentAppleFixture.signed-sha256.txt" <<'PY'
import hashlib, sys
from pathlib import Path
original = Path(sys.argv[1]).read_bytes()
with Path(sys.argv[2]).open('xb') as stream:
    stream.write(hashlib.sha256(original).hexdigest().encode('ascii') + b'\n')
PY
cp "$stage/Resources/ResidentAppleFixture.signed-sha256.txt" "$output/"

# Use installed iOS 26 and the requested device type, never a runtime download.
selection="$(python3 - <<'PY'
import json, subprocess
def listing(category):
    return json.loads(subprocess.check_output(['xcrun', 'simctl', 'list', category, '-j']))[category]
runtimes = [r for r in listing('runtimes') if r.get('isAvailable') and
            r.get('identifier', '').startswith('com.apple.CoreSimulator.SimRuntime.iOS-') and
            r.get('version', '').split('.')[0] == '26']
if not runtimes:
    raise SystemExit('No installed available iOS 26 runtime for resident test.')
runtime = max(runtimes, key=lambda r: tuple(map(int, r['version'].split('.'))))
device = next((d for d in listing('devicetypes') if d['name'] == 'iPhone 17 Pro Max'), None)
if device is None:
    raise SystemExit('Required iPhone 17 Pro Max Simulator type unavailable.')
print(runtime['identifier']); print(device['identifier']); print(runtime['version'])
PY
)"
runtime_id="$(printf '%s\n' "$selection" | sed -n '1p')"
device_type="$(printf '%s\n' "$selection" | sed -n '2p')"
runtime_version="$(printf '%s\n' "$selection" | sed -n '3p')"
printf 'Separate resident Simulator: iPhone 17 Pro Max, iOS %s\n' "$runtime_version" \
  | tee "$output/simulator-selection.log"
device_id="$(xcrun simctl create 'Resident Public SDK Probe' "$device_type" "$runtime_id")"
cleanup() {
  xcrun simctl shutdown "$device_id" >/dev/null 2>&1 || true
  xcrun simctl delete "$device_id" >/dev/null 2>&1 || true
}
trap cleanup EXIT
xcrun simctl boot "$device_id"
xcrun simctl bootstatus "$device_id" -b
python3 "$stage/run_sdk_tests.py" --stage "$stage" --output "$output/xctest" \
  --destination "platform=iOS Simulator,id=$device_id" 2>&1 | tee "$output/resident-sdk-driver.log"
printf '%s\n' 'Public resident Kernel XCTest passed; game, private framework and physical iPhone remain untested.'
