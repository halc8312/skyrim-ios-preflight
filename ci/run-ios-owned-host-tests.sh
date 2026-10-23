#!/usr/bin/env bash
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 2
work="$1"
simde="$2"
harness="$(cd "$(dirname "$0")/.." && pwd)"
output="$harness/out/owned-host"
kit="$work/owned-host-public-kit"
test ! -e "$output"
mkdir "$output"
xcodebuild -version > "$output/apple-tools.txt"
xcrun --sdk iphoneos --show-sdk-version >> "$output/apple-tools.txt"
xcrun --sdk iphonesimulator --show-sdk-version >> "$output/apple-tools.txt"
rustc --version >> "$output/apple-tools.txt"
python3 "$harness/ci/prepare-owned-host-kit.py" "$harness/ci/owned-host" "$kit" \
  | tee "$output/public-stage.log"
cp "$harness/ci/owned-host/public-source-manifest.json" "$output/"
host="$kit/tools/ios-owned-game-host"
backend="$kit/tools/ios-resident-guest-probe/apple-backend"
generic="$kit/tools/windows-ios-guest-probe/owned-guest-abi"
export CARGO_TARGET_DIR="$work/owned-host-cargo-target"
cargo +nightly-2026-10-11 test --locked --no-default-features --features fixture-tools --lib \
  --manifest-path "$host/Cargo.toml" -- --test-threads=1 2>&1 | tee "$output/public-host-tests.log"
# The narrowed public workspace has its own frozen lock for dependency tests.
cargo +nightly-2026-10-11 test --locked \
  --manifest-path "$kit/skyrim-recomp-web/Cargo.toml" -p kernel-hle --lib \
  xma::init_tests::eager_context_base_and_exported_slot_lifetime -- --exact --test-threads=1 \
  2>&1 | tee "$output/public-xma-init-tests.log"
cargo +nightly-2026-10-11 test --locked \
  --manifest-path "$kit/skyrim-recomp-web/Cargo.toml" -p kernel-hle --lib \
  audio::tests -- --test-threads=1 2>&1 | tee "$output/public-audio-observer-tests.log"
cargo +nightly-2026-10-11 test --locked --lib \
  --manifest-path "$kit/tools/ios-signed-image-policy/Cargo.toml" -- --test-threads=1 \
  2>&1 | tee "$output/public-signed-policy-tests.log"
python3 -B -m unittest discover -s "$host/sdk-test" -p 'test_*.py' -v \
  2>&1 | tee "$output/public-driver-tests.log"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT xcrun --sdk macosx \
  clang++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror \
  "$host/sdk-test/audio_render_gate_test.cpp" -o "$work/audio-render-gate-test"
"$work/audio-render-gate-test" 2>&1 | tee "$output/public-audio-render-gate-tests.log"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT xcrun --sdk macosx \
  clang++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror \
  "$host/sdk-test/audio_control_gate_test.cpp" -o "$work/audio-control-gate-test"
"$work/audio-control-gate-test" 2>&1 | tee "$output/public-audio-control-gate-tests.log"
python3 -B -m unittest discover -s "$backend/sdk-test" -p 'test_*.py' -v \
  2>&1 | tee "$output/resident-marker-tests.log"
python3 -B -m unittest discover -s "$backend" -p 'test_prepare_fixture.py' -v \
  2>&1 | tee "$output/public-header-cli-tests.log"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_validate_owned_host_fixture.py' -v \
  2>&1 | tee "$output/public-link-parser-tests.log"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_package_owned_host_ipa.py' -v \
  2>&1 | tee "$output/public-unsigned-package-tests.log"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_prepare_owned_host_kit.py' -v \
  2>&1 | tee "$output/public-source-refusal-tests.log"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_watch_public_audio_hang.py' -v \
  2>&1 | tee "$output/public-audio-hang-helper-tests.log"

# The already reviewed pinned public source builder retains its 228-export,
# Metal/WGSL, matching-platform and strict synthetic coexistence gates.
WGPU_PREFLIGHT_WORK_DIR="$work/wgpu-public" \
WGPU_PREFLIGHT_OUTPUT_DIR="$output/gpu-framework" \
  bash "$harness/ci/gpu-framework/build_frameworks.sh" 2>&1 | tee "$output/public-wgpu-build.log"
export RUSTFLAGS='-C lto=off -C embed-bitcode=no -C panic=unwind'
unset CARGO_ENCODED_RUSTFLAGS || true

for variant in device simulator; do
  host_features=metal-renderer
  if [[ "$variant" == simulator ]]; then host_features=metal-renderer,fixture-tools; fi
  if [[ "$variant" == device ]]; then rust_target=aarch64-apple-ios; else rust_target=aarch64-apple-ios-sim; fi
  built="$work/owned-fixture-$variant"
  bash "$harness/ci/build-owned-host-fixture.sh" "$kit" "$simde" "$built" "$variant" \
    2>&1 | tee "$output/fixture-$variant-build.log"
  # Expectations come from the reviewed fixed public fixture and unsigned
  # image BEFORE any signature is added, never from a runtime query/candidate.
  resources="$work/owned-resources-$variant"
  cargo +nightly-2026-10-11 run --locked --offline --features fixture-tools \
    --manifest-path "$host/Cargo.toml" --bin public_fixture_resources -- \
    "$built/PrivateGuest.framework/PrivateGuest" "$resources" \
    2>&1 | tee "$output/public-$variant-resource-build.log"
  cp "$resources/OwnedGuestBinding.json" "$output/$variant-OwnedGuestBinding.json"
  cp "$built/PrivateGuest.framework/PrivateGuest" "$output/$variant-public-unsigned-fixture.bin"
  for record in fixture.ll actual-exports.txt undefined.txt symbols.txt disassembly.txt link-metadata.json; do
    cp "$built/$record" "$output/$variant-$record"
  done
  WGPU_NATIVE_FRAMEWORK_DIR="$output/gpu-framework/$variant" \
    cargo +nightly-2026-10-11 rustc --locked --release --lib --features "$host_features" \
      --target "$rust_target" --manifest-path "$host/Cargo.toml" -- --print=native-static-libs \
      2>&1 | tee "$output/host-$variant-build.log"
  stage="$work/owned-app-$variant"
  mkdir "$stage"
  cp "$host/project.yml" "$stage/"
  cp -R "$host/app" "$host/sdk-test" "$stage/"
  mkdir "$stage/native" "$stage/include" "$stage/Libraries" "$stage/Frameworks" "$stage/Resources"
  mkdir "$stage/Resources/PublicFixture"
  cp "$host/native/OwnedHostNative.h" "$host/native/OwnedHostNative.mm" "$host/native/OwnedHost.h" "$stage/native/"
  cp "$host/native/DeviceGraphics.h" "$host/native/OwnedHostAudio.h" \
    "$host/native/OwnedHostAudio.mm" "$host/native/OwnedHostAudioRenderGate.h" \
    "$host/native/OwnedHostAudioControlGate.h" \
    "$host/native/GraphicsPresentationProbe.h" "$stage/native/"
  cp "$generic/PrivateGuestABI.h" "$stage/include/"
  cp "$CARGO_TARGET_DIR/$rust_target/release/libios_owned_game_host.a" "$stage/Libraries/"
  cp -R "$built/PrivateGuest.framework" "$output/gpu-framework/$variant/WgpuNative.framework" "$stage/Frameworks/"
  cp "$resources/OwnedGuestBinding.json" "$stage/Resources/"
  cp "$resources/default.xex" "$stage/Resources/PublicFixture/"
  if [[ "$variant" == device ]]; then
    # Strip only an automatically signed PUBLIC dependency, explicitly and
    # before the all-images unsigned audit. Never alter a private input here.
    for framework in "$stage/Frameworks/PrivateGuest.framework" "$stage/Frameworks/WgpuNative.framework"; do
      name="$(basename "$framework" .framework)"
      if /usr/bin/codesign --display --verbose=2 "$framework" > "$output/device-$name-signature-before.log" 2>&1; then
        /usr/bin/codesign --remove-signature "$framework" > "$output/device-$name-remove-signature.log" 2>&1
      fi
    done
    (cd "$stage"; xcodegen generate --spec project.yml) > "$output/device-xcodegen.log" 2>&1
    xcodebuild -project "$stage/OwnedGameHost.xcodeproj" -scheme OwnedGameHost \
      -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
      -derivedDataPath "$work/owned-device-derived" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
      build > "$output/device-xcodebuild.log" 2>&1
    app="$work/owned-device-derived/Build/Products/Release-iphoneos/OwnedGameHost.app"
    # A possible Apple linker auto-signature is removed explicitly from this
    # public app; the packager subsequently refuses every remaining sign marker.
    if /usr/bin/codesign --display --verbose=2 "$app" > "$output/device-app-signature-before.log" 2>&1; then
      /usr/bin/codesign --remove-signature "$app" > "$output/device-app-remove-signature.log" 2>&1
    fi
    python3 "$harness/ci/package-owned-host-ipa.py" "$app" \
      "$output/OwnedGameHost-0.1.0-unsigned.ipa" "$output/device-ipa-metadata.json" \
      | tee "$output/device-ipa-audit.log"
  else
    for framework in "$stage/Frameworks/PrivateGuest.framework" "$stage/Frameworks/WgpuNative.framework"; do
      name="$(basename "$framework" .framework)"
      /usr/bin/codesign --force --sign - --timestamp=none "$framework" > "$output/simulator-$name-codesign.log" 2>&1
      /usr/bin/codesign --verify --strict --verbose=2 "$framework" > "$output/simulator-$name-codesign-verify.log" 2>&1
    done
    cp "$stage/Frameworks/PrivateGuest.framework/PrivateGuest" "$output/simulator-public-signed-fixture.bin"
  fi
done

selection="$(python3 - <<'PY'
import json,subprocess
def listing(kind):return json.loads(subprocess.check_output(['xcrun','simctl','list',kind,'-j']))[kind]
runtime=next((r for r in listing('runtimes') if r.get('isAvailable') and r.get('version')=='26.5'
              and r.get('identifier','').startswith('com.apple.CoreSimulator.SimRuntime.iOS-')),None)
device=next((d for d in listing('devicetypes') if d['name']=='iPhone 17 Pro Max'),None)
if not runtime or not device:raise SystemExit('Required installed iOS 26.5/iPhone 17 Pro Max Simulator unavailable.')
print(runtime['identifier']);print(device['identifier']);print(runtime['version'])
PY
)"
runtime_id="$(printf '%s\n' "$selection" | sed -n '1p')"
device_type="$(printf '%s\n' "$selection" | sed -n '2p')"
printf '%s\n' 'Separate public owned-host Simulator: iPhone 17 Pro Max, iOS 26.5' \
  | tee "$output/simulator-selection.log"
device_id="$(xcrun simctl create 'Owned Host Public SDK Probe' "$device_type" "$runtime_id")"
audio_watch_pid=
finish_audio_watch() {
  if [[ -n "$audio_watch_pid" ]]; then
    # Ask the optional observer to stop waiting for new work, but let an already
    # admitted capture finish. The SDK's own assertions and 5s native timeout
    # are unchanged. Only our Bash child is signalled; never the tested app.
    # Recheck the live job list before every signal: Bash may have reaped it.
    if jobs -pr | grep -Fx -- "$audio_watch_pid" >/dev/null; then
      kill -USR1 "$audio_watch_pid" >/dev/null 2>&1 || true
      local audio_watch_grace_end=$((SECONDS + 18))
      while [[ "$SECONDS" -lt "$audio_watch_grace_end" ]] && \
          jobs -pr | grep -Fx -- "$audio_watch_pid" >/dev/null; do
        sleep 0.1
      done
    fi
    if jobs -pr | grep -Fx -- "$audio_watch_pid" >/dev/null; then
      kill -TERM "$audio_watch_pid" >/dev/null 2>&1 || true
      local audio_watch_term_end=$((SECONDS + 2))
      while [[ "$SECONDS" -lt "$audio_watch_term_end" ]] && \
          jobs -pr | grep -Fx -- "$audio_watch_pid" >/dev/null; do
        sleep 0.1
      done
    fi
    if jobs -pr | grep -Fx -- "$audio_watch_pid" >/dev/null; then
      kill -KILL "$audio_watch_pid" >/dev/null 2>&1 || true
    fi
    wait "$audio_watch_pid" || true
    audio_watch_pid=
  fi
}
cleanup() {
  finish_audio_watch
  xcrun simctl shutdown "$device_id" >/dev/null 2>&1 || true
  xcrun simctl delete "$device_id" >/dev/null 2>&1 || true
}
trap cleanup EXIT
xcrun simctl boot "$device_id"
xcrun simctl bootstatus "$device_id" -b
python3 "$harness/ci/probe-host-audio.py" --phase pre_gui \
  | tee "$output/host-audio-before-gui.log"
# Controlled environment comparison: preserve the same source, runtime, UDID
# and required hardware-output tests, but also run Apple's Simulator host app.
python3 - "$device_id" <<'PY' | tee "$output/simulator-gui-launch.log"
from pathlib import Path
import json, subprocess, sys, time
device = sys.argv[1]
developer = subprocess.check_output(['xcode-select', '-p'], timeout=5).decode().strip()
app = Path(developer) / 'Applications/Simulator.app'
if not app.is_dir():
    raise RuntimeError('configured_apple_simulator_app_missing')
subprocess.run(['/usr/bin/open', '-a', str(app), '--args', '-CurrentDeviceUDID', device],
               check=True, capture_output=True, timeout=10)
deadline = time.monotonic() + 10
running = False
while time.monotonic() < deadline:
    running = subprocess.run(['pgrep', '-x', 'Simulator'], capture_output=True, timeout=2).returncode == 0
    if running:
        break
    time.sleep(0.2)
devices = json.loads(subprocess.check_output(['xcrun', 'simctl', 'list', 'devices', '-j'], timeout=10))['devices']
same = any(value.get('udid') == device and value.get('state') == 'Booted'
           for group in devices.values() for value in group)
print('PUBLIC_SIMULATOR_GUI ' + json.dumps(dict(schema=1, apple_host_process_running=running,
    same_selected_device_booted=same, hardware_audio_verified=False)))
if not running or not same:
    raise RuntimeError('apple_simulator_gui_environment_unconfirmed')
PY
python3 "$harness/ci/probe-host-audio.py" --phase post_gui \
  | tee "$output/host-audio-after-gui.log"
# Environment comparison only: exercise real native Mac output, which can also
# warm host audio services. Simulator assertions below remain mandatory.
if python3 "$harness/ci/probe-macos-output.py" 2>&1 | tee "$output/public-macos-output.log"; then
  :
else
  printf '%s\n' 'PUBLIC_MACOS_OUTPUT_DRIVER_UNAVAILABLE {"schema":1,"diagnostic_only":true,"simulator_audio_verified":false}'
fi
python3 "$host/sdk-test/run_sdk_tests.py" --stage "$work/owned-app-simulator" \
  --output "$output/xctest" --destination "platform=iOS Simulator,id=$device_id" \
  2>&1 | tee "$output/owned-host-sdk-driver.log"
python3 "$host/sdk-test/run_mobile_controls_tests.py" --stage "$work/owned-app-simulator" \
  --built-output "$output/xctest" --output "$output/xctest-mobile" \
  --destination "platform=iOS Simulator,id=$device_id" \
  2>&1 | tee "$output/mobile-controls-sdk-driver.log"
# Optional, public-process-only evidence for the unresolved lazy output-node
# initialization. This does not replace or change any mandatory SDK assertion.
python3 "$harness/ci/watch-public-audio-hang.py" \
  --audio-dir "$output/xctest-device-features/audio" \
  --output "$output/audio-hang" --udid "$device_id" \
  > "$output/audio-hang-driver.log" 2>&1 &
audio_watch_pid=$!
python3 - "$output/audio-hang/watch-ready.json" <<'PY'
from pathlib import Path
import json, time, sys
ready = Path(sys.argv[1])
deadline = time.monotonic() + 4
available = False
while time.monotonic() < deadline:
    try:
        available = ready.is_file() and ready.read_bytes() == b'{"schema":1,"ready":1}\n'
    except OSError:
        available = False
    if available:
        break
    time.sleep(0.05)
print('PUBLIC_AUDIO_WATCH_READY ' + json.dumps(dict(schema=1, ready=available,
    sdk_assertions_unchanged=True)))
PY
if python3 "$host/sdk-test/run_device_features_tests.py" --stage "$work/owned-app-simulator" \
  --built-output "$output/xctest" --output "$output/xctest-device-features" \
  --destination "platform=iOS Simulator,id=$device_id" \
  2>&1 | tee "$output/device-features-sdk-driver.log"; then
  finish_audio_watch
  head -c 65536 "$output/audio-hang-driver.log"
else
  finish_audio_watch
  head -c 65536 "$output/audio-hang-driver.log"
  # The normal runner retains complete logs but only prints their tail. Keep
  # the FIRST failure and numeric native phases visible when download is not
  # available; every failed SDK gate still exits nonzero.
  python3 - "$output/xctest-device-features/audio" <<'PY'
from pathlib import Path
import json, re, stat, sys
root = Path(sys.argv[1])
for stream in ('stdout', 'stderr'):
    path = root / ('xcodebuild.' + stream + '.log')
    try:
        if not stat.S_ISREG(path.lstat().st_mode):
            raise OSError('not_regular')
        with path.open('rb') as source:
            raw = source.read(8 * 1024 * 1024 + 1)
    except OSError:
        print('PUBLIC_AUDIO_FAILURE_DETAIL ' + json.dumps(dict(stream=stream, available=False)))
        continue
    text = raw[:8 * 1024 * 1024].decode('utf-8', 'replace')
    phases = re.findall(r'PUBLIC_AUDIO_PHASE (\d+) (\d+)', text)
    assertions = [line[:1200] for line in text.splitlines() if 'error: -[OwnedGameHostTests.OwnedHostAudioTests' in line]
    rpc = [line[:1200] for line in text.splitlines() if 'Apparently deadlocked' in line or 'No factory registered' in line]
    print('PUBLIC_AUDIO_FAILURE_DETAIL ' + json.dumps(dict(stream=stream, available=True,
        input_truncated=len(raw) > 8 * 1024 * 1024, numeric_phases=phases[:128],
        first_assertions=assertions[:8], rpc_lines=rpc[:8])))
PY
  exit 1
fi
printf '%s\n' 'Public owned-host fullchain passed; game, private framework and physical iPhone remain untested.'
