#!/bin/bash
# Compile authored fixtures with the exact patched translator and real iOS Metal
# compiler. This checks shader compilation, not an actual rendered game frame.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${SKYRIM_IOS_DEPS_ROOT:-$ROOT/local/ios-dependencies-build9}"
CROSS="$WORK/sources/SPIRV-Cross-ios-samplerfix"
OUT="${1:?fresh output directory required}"
mkdir "$OUT"
# Run the actual configuration comparator from the patched dependency stage.
# Missing inputs are errors here, not a silently skipped library regression.
MVK_CONVERTER="$WORK/sources/MoltenVK-ios-cachefix/MoltenVKShaderConverter/MoltenVKShaderConverter"
[[ -f "$MVK_CONVERTER/SPIRVToMSLConverter.cpp" && -f "$CROSS/spirv_msl.hpp" ]]
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_CONVERTER_SOURCE="$MVK_CONVERTER" SKYRIM_SPIRV_CROSS_SOURCE="$CROSS" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_mvk_cache_key.py -v \
  2>&1 | tee "$OUT/mvk-cache-key.log"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_GPU_SOURCE="$WORK/sources/MoltenVK/MoltenVK/MoltenVK/GPUObjects" \
  SKYRIM_MVK_PATCHED_GPU_SOURCE="$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK/GPUObjects" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_mvk_hot_cache.py -v \
  2>&1 | tee "$OUT/mvk-hot-cache.log"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_GPU_SOURCE="$WORK/sources/MoltenVK/MoltenVK/MoltenVK/GPUObjects" \
  SKYRIM_MVK_PATCHED_GPU_SOURCE="$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK/GPUObjects" \
  SKYRIM_MVK_CODEC_SOURCE="$WORK/sources/MoltenVK/MoltenVK/MoltenVK/Utility/MVKCodec.h" \
  SKYRIM_MVK_CONVERTER_SOURCE="$MVK_CONVERTER" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_mvk_compressed_copy.py -v \
  2>&1 | tee "$OUT/mvk-compressed-copy.log"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_GPU_SOURCE="$WORK/sources/MoltenVK/MoltenVK/MoltenVK/GPUObjects" \
  SKYRIM_MVK_PATCHED_GPU_SOURCE="$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK/GPUObjects" \
  SKYRIM_MVK_CODEC_SOURCE="$WORK/sources/MoltenVK/MoltenVK/MoltenVK/Utility/MVKCodec.h" \
  SKYRIM_MVK_CONVERTER_SOURCE="$MVK_CONVERTER" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_mvk_library.py -v \
  2>&1 | tee "$OUT/mvk-library.log"
# Verify the rejected lifecycle adapter cannot enter the shipped dependency.
# Its original real-Metal failure remains in experiments/gpu_lifecycle_gate_test.py.
MVK_CORE="$WORK/sources/MoltenVK/MoltenVK/MoltenVK"
[[ -f "$MVK_CORE/GPUObjects/MVKQueue.mm" ]]
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_LIFECYCLE_SOURCE="$MVK_CORE" \
  SKYRIM_MVK_LIFECYCLE_PATCHED_SOURCE="$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_lifecycle_baseline.py -v \
  2>&1 | tee "$OUT/gpu-lifecycle.log"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_GPU_SOURCE="$MVK_CORE/GPUObjects" \
  SKYRIM_MVK_PATCHED_GPU_SOURCE="$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK/GPUObjects" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_mvk_compile_timing.py -v \
  2>&1 | tee "$OUT/mvk-compile-timing.log"
# Exercise the actual final composition in both OFF/ON runtime modes. These
# authored CPU boundaries complement, but do not replace, real Metal checks.
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_MVK_GPU_SOURCE="$MVK_CORE/GPUObjects" \
  SKYRIM_MVK_PATCHED_GPU_SOURCE="$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK/GPUObjects" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_mvk_module_cold.py -v \
  2>&1 | tee "$OUT/mvk-module-cold.log"
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
# Compile the pinned public SM3 translator once for the build Mac. These are
# authored zero-boundary/ordinary inputs, not private game shaders.
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT python3 \
  "$ROOT/tests/dxso-float/build.py" "$OUT/build/dxso-float"
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_DXSO_FLOAT_FIXTURE="$OUT/build/dxso-float/dxso-float" \
  SKYRIM_DXSO_FLOAT_OUTPUT="$OUT/fixtures/dxso" \
  python3 -m unittest discover -s "$ROOT/tests" -p test_float_compatibility.py -v
fixtures=(color-first depth-first distinct-bindings)
for operation in rcp rsq nrm log pow; do
  name="float-$operation-runtime"
  cp "$OUT/fixtures/dxso/$operation-runtime-1.spv" "$OUT/fixtures/$name.spv"
  fixtures+=("$name")
done
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
# Installation can finish before xcrun sees the mounted toolchain (observed
# on build19). Retry only that exact missing-component result, at most 11
# actual compiles with 20 seconds total sleep. Shader diagnostics fail at once.
compile_after_component() {
  local name="$1" attempt
  for attempt in {1..11}; do
    if compile_fixture "$name" "after-component-$attempt"; then
      return 0
    fi
    if ! grep -Fq 'missing Metal Toolchain' "$OUT/$name-after-component-$attempt.log"; then
      return 1
    fi
    (( attempt < 11 )) || return 1
    sleep 2
  done
  return 1
}
fixture_failed=0
component_attempted=0
for name in "${fixtures[@]}"; do
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
    compile_after_component "$name" || fixture_failed=1
  else
    fixture_failed=1
  fi
done
(( fixture_failed == 0 )) || { echo 'Required Metal shader checks failed.' >&2; exit 1; }
echo 'METAL_SHADER_FIXTURE: 3 alias + 5 runtime-input SM3 arithmetic iPhoneOS compilations passed; game rendering unverified'
# Verify the numeric results too: compiling finite-compatibility expressions
# alone cannot prove their behavior under Metal fast-math assumptions. The
# authored Mac render has its own scope and never counts as iPhone gameplay.
arithmetic_status=0
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT python3 \
  "$ROOT/tests/dxso-float/run_metal_check.py" "$OUT/fixtures" \
  "$OUT/build/metal-arithmetic" || arithmetic_status=$?
mkdir "$OUT/arithmetic-results"
for report in compile.log run.log result.json; do
  [[ ! -f "$OUT/build/metal-arithmetic/$report" ]] || \
    cp "$OUT/build/metal-arithmetic/$report" "$OUT/arithmetic-results/$report"
done
(( arithmetic_status == 0 )) || { echo 'Required authored Metal arithmetic readback failed or unavailable.' >&2; exit 1; }
echo 'METAL_ARITHMETIC: 20 fast/safe Mac GPU cases passed; physical iPhone and water rendering unverified'
