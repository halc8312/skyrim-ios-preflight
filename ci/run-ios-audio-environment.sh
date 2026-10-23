#!/usr/bin/env bash
set -euo pipefail
test "$(uname -s)" = Darwin
harness="$(cd "$(dirname "$0")/.." && pwd)"
driver="$harness/out/audio-environment-driver"
output="$harness/out/audio-environment"
queue_output="$harness/out/audioqueue-environment"
test ! -e "$driver"
test ! -e "$output"
test ! -e "$queue_output"
mkdir -p "$driver"
xcodebuild -version > "$driver/apple-tools.txt"
xcrun --sdk iphonesimulator --show-sdk-version >> "$driver/apple-tools.txt"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_audio_simulator_selection.py' -v \
  2>&1 | tee "$driver/public-simulator-selection-tests.log"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_probe_ios_audio_environment.py' -v \
  2>&1 | tee "$driver/public-supervisor-tests.log"
python3 -B -m unittest discover -s "$harness/ci" -p 'test_probe_ios_audioqueue_environment.py' -v \
  2>&1 | tee "$driver/public-audioqueue-supervisor-tests.log"
simulator_version="${SKYRT_AUDIO_SIMULATOR_VERSION-26.5}"
selection="$(python3 - "$harness/ci" "$simulator_version" "$driver/simulator-selection.json" <<'PY'
import json, subprocess, sys
sys.path.insert(0, sys.argv[1])
from audio_simulator_selection import runtime_identifier, select_simulator

version = sys.argv[2]
runtime_identifier(version) # Reject an unknown explicit version before any listing.
def listing(kind):
    # This short workflow reaches CoreSimulator before Xcode has warmed it.
    # A fresh CI service exceeded 30s before any app install. Keep this
    # environment-only cold start separate from every app/audio deadline.
    limit = 60 if kind == 'runtimes' else 10
    return json.loads(subprocess.check_output(['xcrun', 'simctl', 'list', kind, '-j'], timeout=limit))[kind]
runtimes = listing('runtimes')
device_types = listing('devicetypes')
# Runtime identifiers and OS versions are public environment metadata. Retain
# them even when strict selection refuses; no device UDID or app data is read.
inventory = dict(schema=1, requested_version=version, runtimes=[
    {key: row.get(key) for key in ('identifier', 'version', 'isAvailable', 'buildversion')}
    for row in runtimes if isinstance(row, dict)
    and isinstance(row.get('identifier'), str)
    and row['identifier'].startswith('com.apple.CoreSimulator.SimRuntime.iOS-')])
with open(sys.argv[3].replace('simulator-selection.json', 'simulator-runtime-inventory.json'),
          'x', encoding='ascii', newline='\n') as output:
    output.write(json.dumps(inventory, sort_keys=True) + '\n')
selected = select_simulator(runtimes, device_types, version)
with open(sys.argv[3], 'x', encoding='ascii', newline='\n') as output:
    output.write(json.dumps(selected, sort_keys=True) + '\n')
print(selected['runtime_identifier'])
print(selected['device_type_identifier'])
PY
)"
runtime_id="$(printf '%s\n' "$selection" | sed -n '1p')"
device_type="$(printf '%s\n' "$selection" | sed -n '2p')"
device_id="$(xcrun simctl create 'Public Minimal Audio Diagnostic' "$device_type" "$runtime_id")"
[[ "$device_id" =~ ^[[:xdigit:]]{8}(-[[:xdigit:]]{4}){3}-[[:xdigit:]]{12}$ ]]
cleanup() {
  # This UUID comes only from the Simulator created by this invocation.
  xcrun simctl shutdown "$device_id" >/dev/null 2>&1 || true
  xcrun simctl delete "$device_id" >/dev/null 2>&1 || true
}
trap cleanup EXIT
xcrun simctl boot "$device_id"
xcrun simctl bootstatus "$device_id" -b
python3 "$harness/ci/probe-host-audio.py" --phase pre_gui \
  | tee "$driver/host-audio-before-gui.log"
python3 - "$device_id" <<'PY' | tee "$driver/simulator-gui-launch.log"
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
  | tee "$driver/host-audio-after-gui.log"
# Native Mac hardware is an environment comparison, never a Simulator pass.
if python3 "$harness/ci/probe-macos-output.py" 2>&1 | tee "$driver/public-macos-output.log"; then
  :
else
  printf '%s\n' 'PUBLIC_MACOS_OUTPUT_DRIVER_UNAVAILABLE {"schema":1,"diagnostic_only":true,"simulator_audio_verified":false}'
fi
# This diagnostic has no game, Rust host, XCTest, device IPA or signing account.
# It cannot replace any of the required owned-host integration assertions.
engine_failed=0
if python3 "$harness/ci/probe-ios-audio-environment.py" --udid "$device_id" --output "$output" \
  --simulator-version "$simulator_version" \
  2>&1 | tee "$driver/public-ios-audio-environment.log"; then
  :
else
  engine_failed=1
fi
# Separate API comparison on the SAME selected Simulator. A passing queue
# never replaces a failed engine result or any production integration gate.
queue_failed=0
if python3 "$harness/ci/probe-ios-audioqueue-environment.py" --udid "$device_id" --output "$queue_output" \
  --simulator-version "$simulator_version" \
  2>&1 | tee "$driver/public-ios-audioqueue-environment.log"; then
  :
else
  queue_failed=1
fi
if [[ "$engine_failed" -ne 0 || "$queue_failed" -ne 0 ]]; then
  exit 1
fi
