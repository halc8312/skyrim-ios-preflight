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
cmake -S "$ROOT/tests/metal-alias" -B "$OUT/build" -DCMAKE_BUILD_TYPE=Release \
  "-DSPIRV_CROSS_SOURCE=$CROSS"
cmake --build "$OUT/build" --parallel 2 --target sampler-alias
for name in color-first depth-first distinct-bindings; do
  "$OUT/build/sampler-alias" "$OUT/fixtures/$name.spv" "$OUT/fixtures/$name.metal"
  xcrun --sdk iphoneos metal -std=ios-metal3.1 -c "$OUT/fixtures/$name.metal" -o "$OUT/fixtures/$name.air"
done
echo 'METAL_ALIAS_FIXTURE: 3 real iPhoneOS shader compilations passed; game rendering unverified'
