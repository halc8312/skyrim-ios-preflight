#!/usr/bin/env bash
# No physical iPhone, Apple account, game files, JIT, or signing certificates.
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 4
work="$1"
source_dir="$2"
fixture_dir="$3"
simde_dir="$4"
harness_root="$(cd "$(dirname "$0")/.." && pwd)"
test -f "$source_dir/crates/xenos-texture/Cargo.toml"
test -f "$fixture_dir/ppc_recomp.0.cpp"
output="$harness_root/out/ios-simulator"
mkdir -p "$output"

# Use an already-installed iOS 26 runtime. Do not start a multi-GB runtime
# download or silently substitute a macOS test for an iOS Simulator test.
sim_selection="$(python3 - <<'PY'
import json, subprocess
def sim(*args):
    return json.loads(subprocess.check_output(['xcrun', 'simctl', 'list', *args, '-j']))
runtimes = [r for r in sim('runtimes')['runtimes']
            if r.get('isAvailable') and r.get('identifier', '').startswith('com.apple.CoreSimulator.SimRuntime.iOS-')
            and r.get('version', '').split('.')[0] == '26']
if not runtimes:
    raise SystemExit('No installed, available iOS 26 Simulator runtime; device execution remains untested.')
runtime = max(runtimes, key=lambda r: tuple(map(int, r['version'].split('.'))))
types = [d for d in sim('devicetypes')['devicetypes'] if d['name'].startswith('iPhone')]
device = next((d for d in types if d['name'] == 'iPhone 17 Pro Max'), None)
if device is None:
    raise SystemExit('iPhone 17 Pro Max Simulator device type is unavailable.')
print(runtime['identifier'])
print(device['identifier'])
print(runtime['version'])
PY
)"
runtime_id="$(printf '%s\n' "$sim_selection" | sed -n '1p')"
device_type="$(printf '%s\n' "$sim_selection" | sed -n '2p')"
runtime_version="$(printf '%s\n' "$sim_selection" | sed -n '3p')"
printf 'Simulator: iPhone 17 Pro Max, iOS %s (not a physical device)\n' "$runtime_version"

rustup target add --toolchain "$RUSTUP_TOOLCHAIN" aarch64-apple-ios-sim
cargo build --locked --release --lib --target aarch64-apple-ios-sim \
  --manifest-path "$work/bridge/Cargo.toml"
sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
compiler="$(xcrun --sdk iphonesimulator --find clang++)"
cmake -S "$source_dir/cpp" -B "$work/simulator-guest" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT="$sdk" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_CXX_COMPILER="$compiler" -DCMAKE_BUILD_TYPE=Release \
  -DXENON_RECOMP_OUT="$fixture_dir" -DSIMDE_DIR="$fixture_dir" \
  -DSKYRT_SHIM="$fixture_dir/imports.cpp" -DSKYRT_DISPATCH="$fixture_dir/dispatch.cpp"
cmake --build "$work/simulator-guest" --parallel 2
probe="$work/simulator-probe"
mkdir -p "$probe/Libraries" "$probe/bridge/include"
cp "$harness_root/ios-probe/project.yml" "$probe/"
cp -R "$harness_root/ios-probe/app" "$probe/app"
cp -R "$harness_root/ios-probe/tests" "$probe/tests"
bash "$harness_root/ci/prepare-cpu-support.sh" "$source_dir" "$simde_dir" "$probe"
cp "$work/bridge/include/XenosProbeBridge.h" "$probe/bridge/include/"
cp "$CARGO_TARGET_DIR/aarch64-apple-ios-sim/release/libxenos_probe_bridge.a" "$probe/Libraries/"
cp "$work/simulator-guest/libskyrim_guest.a" "$probe/Libraries/"
(
  cd "$probe"
  xcodegen generate --spec project.yml
)
device_id="$(xcrun simctl create 'Skyrim Source Probe' "$device_type" "$runtime_id")"
cleanup() {
  xcrun simctl shutdown "$device_id" >/dev/null 2>&1 || true
  xcrun simctl delete "$device_id" >/dev/null 2>&1 || true
}
trap cleanup EXIT
xcrun simctl boot "$device_id"
xcrun simctl bootstatus "$device_id" -b
xcodebuild -project "$probe/SkyrimProbe.xcodeproj" -scheme SkyrimProbe \
  -configuration Debug -destination "platform=iOS Simulator,id=$device_id" \
  -derivedDataPath "$work/SimDerivedData" -resultBundlePath "$output/SkyrimProbe.xcresult" \
  -parallel-testing-enabled NO CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO test \
  2>&1 | tee "$output/simulator-tests.log"
xcrun simctl launch --terminate-running-process "$device_id" com.halc8312.skyrimprobe
# A bounded UI capture after launch; the downloaded image is inspected by us.
sleep 2
xcrun simctl io "$device_id" screenshot "$output/SkyrimProbe-simulator.png"
printf '%s\n' 'iOS Simulator tests passed. Physical iPhone execution and Skyrim gameplay remain untested.'
