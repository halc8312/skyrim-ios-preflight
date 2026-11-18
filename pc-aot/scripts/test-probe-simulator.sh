#!/bin/bash
# Optional ARM64 Simulator evidence. Never downloads runtimes or signs for a device.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/local/simulator/run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$OUT"
exec 3>&1 4>&2
exec > >(tee "$OUT/runner.log") 2>&1
RUNNER_TEE_PID=$!
SIMULATOR_UDID=""
CORE_PASSED=false
UI_PASSED=false
export OUT CORE_PASSED UI_PASSED
finish() {
  result=$?
  trap - EXIT
  # Once preservation starts, finish the bounded operations instead of losing
  # evidence to a second soft cancellation signal. A hard runner kill is final.
  trap '' INT TERM
  set +e
  if [ -n "$SIMULATOR_UDID" ]; then
    python3 "$ROOT/scripts/run-bounded.py" 20 xcrun simctl io "$SIMULATOR_UDID" screenshot "$OUT/final-simulator-screen.png"
    CONTAINER="$(python3 "$ROOT/scripts/run-bounded.py" 20 xcrun simctl get_app_container "$SIMULATOR_UDID" org.skyrimios.probe data 2>/dev/null)"
    if [ -n "$CONTAINER" ] && [ -d "$CONTAINER/Library/Application Support/SkyrimProbe" ]; then
      mkdir -p "$OUT/app-logs"
      for app_log in "$CONTAINER/Library/Application Support/SkyrimProbe"/session-*.log; do
        if [ -f "$app_log" ] && [ ! -L "$app_log" ]; then cp "$app_log" "$OUT/app-logs/"; fi
      done
    fi
  fi
  if [ -d "$OUT/ProbeUITests.xcresult" ]; then
    mkdir -p "$OUT/attachments"
    python3 "$ROOT/scripts/run-bounded.py" 30 xcrun xcresulttool export attachments --path "$OUT/ProbeUITests.xcresult" --output-path "$OUT/attachments" > "$OUT/attachment-export.log" 2>&1
  fi
  printf 'Simulator evidence retained at %s\n' "$OUT"
  # Process-substitution tee is asynchronous and not covered by pipefail. Close
  # its input, wait for the log to flush, and propagate a logging failure.
  exec 1>&3 2>&4 3>&- 4>&-
  wait "$RUNNER_TEE_PID"
  log_result=$?
  if [ "$log_result" -ne 0 ] && [ "$result" -eq 0 ]; then result=3; fi
  export CORE_PASSED UI_PASSED
  python3 - "$result" <<'PY'
import json, os, pathlib, platform, sys
p = pathlib.Path(os.environ['OUT'])
scope = {
    'scope': 'ARM64 iOS Simulator only',
    'device_proof': False, 'gameplay_proof': False, 'audio_audibility_verified': False,
    'xcode_required': '26.6', 'runtime_required': 'iOS 26.5',
    'simulator_model': 'iPhone 17 Pro Max', 'host_arch': platform.machine(),
    'exit_code': int(sys.argv[1]),
    'native_macos_core_passed': os.environ['CORE_PASSED'] == 'true',
    'ui_smoke_passed': os.environ['UI_PASSED'] == 'true',
    'va_reservation_success': 'Inspect VA PASS/FAIL in the preserved XCTest text attachment; UI success only asserts a visible result.',
    'visual_geometry': 'Viewport presence and Metal pipeline readiness asserted; inspect preserved screenshots for geometry.',
    'status': 'passed' if int(sys.argv[1]) == 0 else 'failed',
}
for filename, key in [('source-commit.txt', 'git_commit'), ('xcode-version.txt', 'xcode'), ('sdk-version.txt', 'simulator_sdk')]:
    if (p / filename).exists():
        scope[key] = (p / filename).read_text().strip()
(p / 'scope.json').write_text(json.dumps(scope, indent=2) + '\n')
PY
  scope_result=$?
  if [ "$scope_result" -ne 0 ] && [ "$result" -eq 0 ]; then result=3; fi
  exit "$result"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

[ "$(uname -s)" = Darwin ] || { printf 'Requires macOS; no Apple execution on this host.\n'; exit 2; }
[ "$(uname -m)" = arm64 ] || { printf 'Requires an ARM64 Mac.\n'; exit 2; }
xcodebuild -version | tee "$OUT/xcode-version.txt"
grep -q '^Xcode 26\.6$' "$OUT/xcode-version.txt" || { printf 'Select the already-installed Xcode 26.6 before running.\n'; exit 2; }
grep -q '^Build version 17F113$' "$OUT/xcode-version.txt" || { printf 'Xcode build identifier mismatch.\n'; exit 2; }
git -C "$ROOT" rev-parse HEAD > "$OUT/source-commit.txt"
xcrun --sdk iphonesimulator --show-sdk-version > "$OUT/sdk-version.txt"
xcrun simctl list runtimes --json > "$OUT/runtimes.json"
xcrun simctl list devices available --json > "$OUT/devices.json"
SIMULATOR_UDID="$(python3 - "$OUT" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
runtimes = json.loads((p / 'runtimes.json').read_text())['runtimes']
runtime = next((r for r in runtimes if r.get('name') == 'iOS 26.5' and r.get('isAvailable')), None)
if runtime is None:
    sys.exit('Required installed iOS 26.5 runtime is unavailable; no runtime will be downloaded.')
devices = json.loads((p / 'devices.json').read_text())['devices'].get(runtime['identifier'], [])
device = next((d for d in devices if d.get('name') == 'iPhone 17 Pro Max' and d.get('isAvailable')), None)
if device is None:
    sys.exit('Required existing iPhone 17 Pro Max simulator is unavailable; no device will be created.')
print(device['udid'])
PY
)"
# Boot only the selected installed simulator; a prebooted device needs no mutation.
if ! xcrun simctl list devices booted --json | python3 -c 'import json,sys; wanted=sys.argv[1]; data=json.load(sys.stdin); sys.exit(0 if any(d["udid"] == wanted for v in data["devices"].values() for d in v) else 1)' "$SIMULATOR_UDID"; then
  python3 "$ROOT/scripts/run-bounded.py" 30 xcrun simctl boot "$SIMULATOR_UDID"
fi
# The verified cold boot took about six minutes; allow ten before preserving
# failure evidence. The UI command separately has a six-minute deadline.
python3 "$ROOT/scripts/run-bounded.py" 600 xcrun simctl bootstatus "$SIMULATOR_UDID" -b

# Exercise the same portable implementation as native macOS ARM64, separately.
xcrun --sdk macosx clang -arch arm64 -std=c11 -Wall -Wextra -Werror \
  -I "$ROOT/native" "$ROOT/native/guest_memory.c" "$ROOT/tests/native/test_guest_memory.c" \
  -o "$OUT/test_guest_memory" 2>&1 | tee "$OUT/native-build.log"
"$OUT/test_guest_memory" 2>&1 | tee "$OUT/native-test.log"
CORE_PASSED=true

python3 "$ROOT/scripts/run-bounded.py" 360 xcodebuild test -project "$ROOT/ios/SkyrimProbe.xcodeproj" -scheme SkyrimProbeUITests \
  -configuration Debug -sdk iphonesimulator -destination "platform=iOS Simulator,id=$SIMULATOR_UDID" \
  -jobs 2 -destination-timeout 60 -parallel-testing-enabled NO -maximum-concurrent-test-simulator-destinations 1 \
  -derivedDataPath "$OUT/DerivedData" -resultBundlePath "$OUT/ProbeUITests.xcresult" \
  CODE_SIGNING_ALLOWED=NO ARCHS=arm64 2>&1 | tee "$OUT/xcodebuild-test.log"
UI_PASSED=true
