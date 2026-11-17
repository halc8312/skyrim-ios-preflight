#!/bin/bash
# Authored library-factory comparison only; no app, game, signing or phone inputs.
set -euo pipefail
umask 077
[[ "$(uname -s)" == Darwin ]] || { echo 'Apple host required' >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
python3 scripts/check-public-tree.py
mkdir -p local/metal-compile-mac
RUN="$(mktemp -d "$ROOT/local/metal-compile-mac/run-XXXXXX")"
exec > >(tee "$RUN/checks.log") 2>&1
archive_evidence() {
  local status=$?
  trap - EXIT
  python3 - "$RUN" <<'PY'
from pathlib import Path
import sys, zipfile
root = Path(sys.argv[1])
with zipfile.ZipFile(root / 'metal-compile-checks.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        if path.is_file() and path.suffix in {'.json', '.log', '.txt', '.metal', '.air', '.metallib', '.mm', '.py', '.h', '.cpp', '.sh'}:
            archive.write(path, path.relative_to(root))
PY
  exit "$status"
}
trap archive_evidence EXIT
# Leave time for evidence publication before the 15-minute CI ceiling. These
# bounds supervise our child tree, not external Apple compilation services.
WORKFLOW_DEADLINE=$((SECONDS + 720))
READINESS_DEADLINE=$WORKFLOW_DEADLINE
run_bounded() {
  local label="$1" limit="$2" remaining=$((READINESS_DEADLINE - SECONDS))
  local workflow_remaining=$(( ${WORKFLOW_DEADLINE:-$READINESS_DEADLINE} - SECONDS ))
  shift 2
  (( remaining <= workflow_remaining )) || remaining=$workflow_remaining
  (( remaining > 0 )) || { echo 'Metal workflow or readiness budget exhausted' >&2; return 124; }
  (( limit <= remaining )) || limit=$remaining
  python3 - "$ROOT" "$RUN" "$label" "$limit" "$@" <<'PY'
import importlib.util, os, pathlib, sys
root, output, label, timeout, *command = sys.argv[1:]
spec = importlib.util.spec_from_file_location('metal_command_control', pathlib.Path(root) / 'experiments/metal-compile/offline.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)
event, stdout, stderr = control.run_command(command, label, pathlib.Path(output), {'commands': []}, os.environ.copy(), float(timeout))
print(stdout, end='')
print(stderr, end='', file=sys.stderr)
raise SystemExit(124 if event['timed_out'] else event['returncode'] if event['returncode'] is not None else 1)
PY
}

remaining_budget() {
  local remaining=$((WORKFLOW_DEADLINE - SECONDS))
  (( remaining > 0 )) || { echo 'Metal workflow budget exhausted' >&2; return 124; }
  printf '%s\n' "$remaining"
}

run_bounded host-version 20 sw_vers
python3 - <<'PY'
import platform, sys
version = platform.mac_ver()[0]
if not version or int(version.split('.')[0]) < 26:
    print('Metal 4 fixture requires macOS 26 or later; host is unsupported', file=sys.stderr)
    raise SystemExit(78)
PY
run_bounded xcode-version 20 xcodebuild -version
run_bounded sdk-version 20 xcrun --sdk macosx --show-sdk-version
python3 --version
git rev-parse HEAD
unset IPHONEOS_DEPLOYMENT_TARGET SDKROOT MACOSX_DEPLOYMENT_TARGET

TEST_BUDGET="$(remaining_budget)"
(( TEST_BUDGET <= 240 )) || TEST_BUDGET=240
SKYRIM_METAL_REQUEST_OUTPUT="$RUN/request-lifetime" \
SKYRIM_METAL_SUPERVISOR_TEST_OUTPUT="$RUN/supervisor-controls" \
  python3 scripts/supervise-metal-tests.py --output "$RUN/test-supervision" --budget "$TEST_BUDGET"

READINESS_DEADLINE=$((SECONDS + 240))
# A version probe alone does not establish that Xcode's optional component can
# compile. Preserve each actual attempt, and retry only the known missing-tool
# diagnostic after one official component download.
compile_probe() {
  local attempt="$1" status=0
  run_bounded "probe-$attempt" 20 xcrun --sdk macosx metal -c -std=macos-metal2.4 -mmacosx-version-min=15.0 \
    -DPROBE_BIAS=3 -fno-fast-math "$ROOT/experiments/metal-compile/fixture.metal" \
    -o "$RUN/probe-$attempt.air" > "$RUN/probe-$attempt.log" 2>&1 || status=$?
  cat "$RUN/probe-$attempt.log"
  echo "METAL_READINESS attempt=$attempt status=$status"
  (( status == 0 )) && test -s "$RUN/probe-$attempt.air"
}
if ! compile_probe initial; then
  grep -Fq 'missing Metal Toolchain' "$RUN/probe-initial.log" || exit 1
  component_status=0
  run_bounded metal-toolchain 180 xcodebuild -downloadComponent metalToolchain > "$RUN/metal-toolchain.log" 2>&1 || component_status=$?
  cat "$RUN/metal-toolchain.log"
  echo "METAL_COMPONENT download_status=$component_status"
  ready=0
  for attempt in {1..11}; do
    if compile_probe "after-component-$attempt"; then ready=1; break; fi
    grep -Fq 'missing Metal Toolchain' "$RUN/probe-after-component-$attempt.log" || exit 1
    (( attempt == 11 )) || sleep 2
  done
  (( ready == 1 )) || exit 1
fi

READINESS_DEADLINE=$WORKFLOW_DEADLINE
# Compile the same authored adapter for the real iPhone SDK without linking,
# signing, packaging or installing an application. This proves API availability
# and ARC/C++ syntax only; no iPhone or Metal execution is implied.
IOS_SDK="$(run_bounded iphone-sdk-path 20 xcrun --sdk iphoneos --show-sdk-path)"
test -d "$IOS_SDK"
run_bounded ios26-adapter-compile 90 xcrun --sdk iphoneos clang++ \
  -target arm64-apple-ios26.0 -isysroot "$IOS_SDK" -std=c++17 -O2 -g -fobjc-arc \
  -Wall -Wextra -Werror -c "$ROOT/experiments/metal-compile/runtime.mm" -o "$RUN/runtime-ios26.o"
python3 - "$RUN" <<'PY'
from pathlib import Path
import hashlib, json, sys
root = Path(sys.argv[1])
data = (root / 'runtime-ios26.o').read_bytes()
if not data:
    raise SystemExit('iPhone SDK object is empty')
(root / 'ios-sdk-compile.json').write_text(json.dumps({
    'scope': 'authored-adapter-object-compilation', 'target': 'arm64-apple-ios26.0',
    'object_bytes': len(data), 'object_sha256': hashlib.sha256(data).hexdigest(),
    'app_built': False, 'physical_iphone_verified': False, 'gpu_execution_verified': False
}, indent=2) + '\n')
PY

python3 experiments/metal-compile/options_probe.py --budget "$(remaining_budget)" --output "$RUN/options"
python3 experiments/metal-compile/offline.py --target macos --deployment 15.0 \
  --repeat 3 --timeout 30 --budget "$(remaining_budget)" --output "$RUN/offline"
python3 experiments/metal-compile/runtime.py --repeat 3 --async-library --probe-library-factory --budget "$(remaining_budget)" --output "$RUN/runtime" \
  --library "$RUN/offline/trial-01/fixture.metallib"
echo 'METAL_COMPILE_CHECKS authored_library_comparison=passed iphone_verified=0 performance_improvement_verified=0'
