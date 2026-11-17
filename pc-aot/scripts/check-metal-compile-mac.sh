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
        if path.is_file() and path.suffix in {'.json', '.log', '.txt', '.metal', '.air', '.metallib', '.mm', '.py'}:
            archive.write(path, path.relative_to(root))
PY
  exit "$status"
}
trap archive_evidence EXIT
sw_vers
xcodebuild -version
xcrun --sdk macosx --show-sdk-version
python3 --version
git rev-parse HEAD
unset IPHONEOS_DEPLOYMENT_TARGET SDKROOT MACOSX_DEPLOYMENT_TARGET

python3 -m unittest discover -s tests -p 'test_metal_compile_*.py' -v

# A version probe alone does not establish that Xcode's optional component can
# compile. Preserve each actual attempt, and retry only the known missing-tool
# diagnostic after one official component download.
compile_probe() {
  local attempt="$1" status=0
  xcrun --sdk macosx metal -c -std=macos-metal2.4 -mmacosx-version-min=15.0 \
    -DPROBE_BIAS=3 -fno-fast-math experiments/metal-compile/fixture.metal \
    -o "$RUN/probe-$attempt.air" > "$RUN/probe-$attempt.log" 2>&1 || status=$?
  cat "$RUN/probe-$attempt.log"
  echo "METAL_READINESS attempt=$attempt status=$status"
  (( status == 0 )) && test -s "$RUN/probe-$attempt.air"
}
if ! compile_probe initial; then
  grep -Fq 'missing Metal Toolchain' "$RUN/probe-initial.log" || exit 1
  component_status=0
  xcodebuild -downloadComponent metalToolchain > "$RUN/metal-toolchain.log" 2>&1 || component_status=$?
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

python3 experiments/metal-compile/offline.py --target macos --deployment 15.0 \
  --repeat 3 --output "$RUN/offline"
python3 experiments/metal-compile/runtime.py --repeat 3 --output "$RUN/runtime" \
  --library "$RUN/offline/trial-01/fixture.metallib"
echo 'METAL_COMPILE_CHECKS authored_library_comparison=passed iphone_verified=0 performance_improvement_verified=0'
