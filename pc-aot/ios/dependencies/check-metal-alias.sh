#!/bin/bash
# Compile authored fixtures with the exact patched translator and real iOS Metal
# compiler. This checks shader compilation, not an actual rendered game frame.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${SKYRIM_IOS_DEPS_ROOT:-$ROOT/local/ios-dependencies-displayfix}"
CROSS="$WORK/sources/SPIRV-Cross-ios-samplerfix"
OUT="${1:?fresh output directory required}"
mkdir "$OUT"
python3 "$ROOT/tests/graphics_alias_fixture.py" "$OUT/fixtures"
# This executable runs on the build Mac, not on the phone. The workflow's
# IPHONEOS_DEPLOYMENT_TARGET otherwise makes Apple clang silently select iOS.
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT cmake \
  -S "$ROOT/tests/metal-alias" -B "$OUT/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_OSX_SYSROOT=macosx \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  "-DSPIRV_CROSS_SOURCE=$CROSS"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT cmake \
  --build "$OUT/build" --parallel 2 --target sampler-alias
xcrun vtool -show-build "$OUT/build/sampler-alias" | tee "$OUT/host-platform.txt"
grep -q 'platform MACOS' "$OUT/host-platform.txt"
# Readiness is determined by a real compile. A version probe does not prove
# that the optional component can compile the actual iPhoneOS shaders.
compile_fixture() {
  local name="$1" attempt="$2" status=0
  local log="$OUT/$name-$attempt.log"
  xcrun --sdk iphoneos metal -std=metal3.1 -c "$OUT/fixtures/$name.metal" \
    -o "$OUT/fixtures/$name.air" > "$log" 2>&1 || status=$?
  cat "$log"
  echo "METAL_COMPILE fixture=$name attempt=$attempt status=$status"
  if (( status == 0 )); then
    test -s "$OUT/fixtures/$name.air" || return 1
  fi
  return "$status"
}
fixture_failed=0
component_attempted=0
for name in color-first depth-first distinct-bindings; do
  if ! "$OUT/build/sampler-alias" "$OUT/fixtures/$name.spv" "$OUT/fixtures/$name.metal"; then
    echo "METAL_TRANSLATION fixture=$name failed" >&2
    fixture_failed=1
    continue
  fi
  if compile_fixture "$name" initial; then
    continue
  fi
  # A single official component installation is allowed only for the explicit
  # missing-toolchain error. Shader errors are never hidden by an install/retry.
  if (( component_attempted == 0 )) && grep -Fq 'missing Metal Toolchain' "$OUT/$name-initial.log"; then
    component_attempted=1
    download_status=0
    xcodebuild -downloadComponent metalToolchain > "$OUT/metal-toolchain.log" 2>&1 || download_status=$?
    cat "$OUT/metal-toolchain.log"
    echo "METAL_COMPONENT download_status=$download_status"
    compile_fixture "$name" after-component || fixture_failed=1
  else
    fixture_failed=1
  fi
done
(( fixture_failed == 0 )) || { echo 'Required Metal shader checks failed.' >&2; exit 1; }
echo 'METAL_ALIAS_FIXTURE: 3 real iPhoneOS shader compilations passed; game rendering unverified'
