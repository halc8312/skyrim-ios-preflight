#!/usr/bin/env bash
# No game executable, ROM, archive, key, or translated game code is an input.
# This builds library components, synthetic stubs, and an unsigned diagnostic IPA.
# It never builds Skyrim gameplay or accepts proprietary game data.
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
  printf '%s\n' 'This preflight requires a macOS host with the iPhoneOS SDK.' >&2
  exit 2
fi

harness_root="$(cd "$(dirname "$0")/.." && pwd)"
source_commit=a0d62fa1b47e215ffec26ce043df436cba342a38
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/skyrim-ios-preflight.XXXXXX")"
source_dir="$work/source"
fixture_dir="$work/synthetic"
output_dir="$harness_root/out/ios-synthetic"
mkdir -p "$fixture_dir" "$output_dir"
export HOMEBREW_NO_AUTO_UPDATE=1

# Fetch a fixed public source revision into an empty workspace. There is no
# path argument through which files from the PC's game installation can enter.
git init -q "$source_dir"
git -C "$source_dir" remote add origin https://github.com/1-TB/skyrim-recomp-web.git
git -C "$source_dir" fetch --depth 1 origin "$source_commit"
git -C "$source_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$source_dir" rev-parse HEAD)" = "$source_commit"
git -C "$source_dir" apply --check "$harness_root/ci/native-arm64.patch"
git -C "$source_dir" apply "$harness_root/ci/native-arm64.patch"
git -C "$source_dir" apply --check "$harness_root/ci/arm-fpcr-mapping.patch"
git -C "$source_dir" apply "$harness_root/ci/arm-fpcr-mapping.patch"

# Only pinned public SIMDe headers enter the C++ CPU fixture.
simde_commit=a532a12ca9bbdc5e6547eb602e6256b71a5377d4
simde_dir="$work/simde"
git init -q "$simde_dir"
git -C "$simde_dir" remote add origin https://github.com/simd-everywhere/simde-no-tests.git
git -C "$simde_dir" fetch --depth 1 origin "$simde_commit"
git -C "$simde_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$simde_dir" rev-parse HEAD)" = "$simde_commit"

xcodebuild -version
# The diagnostic app uses Metal's normal makeLibrary(source:options:) API.
# No separate offline Metal compiler or CPU JIT entitlement is needed.
sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
compiler="$(xcrun --sdk iphoneos --find clang++)"
missing_tools=()
for tool in cmake ninja xcodegen; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    missing_tools+=("$tool")
  fi
done
if [[ ${#missing_tools[@]} -gt 0 ]]; then
  brew install "${missing_tools[@]}"
fi
cmake --version
ninja --version

# Use the upstream pinned toolchain: the complete runtime also relies on newer
# std implementations (Once: Default), beyond the four original library crates.
export RUSTUP_TOOLCHAIN=nightly-2026-10-11
export CARGO_TARGET_DIR="$work/rust-target"
export IPHONEOS_DEPLOYMENT_TARGET=26.0
if ! command -v rustup >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location \
    https://sh.rustup.rs -o "$work/rustup-init.sh"
  sh "$work/rustup-init.sh" -y --profile minimal --default-toolchain "$RUSTUP_TOOLCHAIN" --no-modify-path
  export PATH="$HOME/.cargo/bin:$PATH"
fi
rustup toolchain install "$RUSTUP_TOOLCHAIN" --profile minimal
rustup target add --toolchain "$RUSTUP_TOOLCHAIN" aarch64-apple-ios
# Use the same LLVM producer/reader for archives containing Rust bitcode.
rustup component add --toolchain "$RUSTUP_TOOLCHAIN" llvm-tools-preview
rust_host="$(rustc -vV | sed -n 's/^host: //p')"
rust_nm="$(rustc --print sysroot)/lib/rustlib/$rust_host/bin/llvm-nm"
test -x "$rust_nm"
native_output="$harness_root/out/native-tests"
mkdir -p "$native_output"
# Reproduce the existing Darwin decommit contract before applying the repair.
# A build error is not accepted as evidence of the memory bug.
set +e
(
  cd "$source_dir"
  cargo test --locked -p runtime-core --test guest_memory decommit_zeroes_contents -- --exact
) 2>&1 | tee "$native_output/darwin-baseline.log"
baseline_status=${PIPESTATUS[0]}
set -e
if [[ "$baseline_status" -eq 0 ]]; then
  printf '%s\n' 'Baseline decommit test passed on this host; no failing baseline is claimed.'
elif [[ "$baseline_status" -eq 101 ]] && grep -q 'test decommit_zeroes_contents .*FAILED' "$native_output/darwin-baseline.log"; then
  printf '%s\n' 'Reproduced the Darwin memory zeroing failure in the upstream baseline.'
else
  printf '%s\n' 'Baseline could not be evaluated; stopping rather than treating a build error as a reproduced bug.' >&2
  exit 1
fi
git -C "$source_dir" apply --check "$harness_root/ci/apple-memory.patch"
git -C "$source_dir" apply "$harness_root/ci/apple-memory.patch"
git -C "$source_dir" apply --check "$harness_root/ci/vfs-host-casing.patch"
git -C "$source_dir" apply "$harness_root/ci/vfs-host-casing.patch"
packages=(-p xex -p xenos-texture -p xenos-shader -p xenos-state
          -p runtime-core -p vfs -p kernel-hle -p d3d-hle -p xma-decoder)
(
  cd "$source_dir"
  # This optional export-table test requires another checkout and otherwise
  # silently returns success, so keep it explicitly filtered out.
  cargo test --locked --no-fail-fast "${packages[@]}" -- --test-threads=1 --skip export_ordinals_match_export_tables
  cargo build --locked --lib --target aarch64-apple-ios "${packages[@]}"
) 2>&1 | tee "$native_output/native-tests.log"

cat > "$fixture_dir/ppc_recomp.0.cpp" <<'CPP'
extern "C" unsigned long long guest_probe(unsigned long long x) {
    return (x << 7) ^ 0x1234ULL;
}
CPP
printf '%s\n' 'extern "C" unsigned synthetic_import() { return 17; }' > "$fixture_dir/imports.cpp"
printf '%s\n' 'extern "C" unsigned synthetic_dispatch() { return 23; }' > "$fixture_dir/dispatch.cpp"

cmake -S "$source_dir/cpp" -B "$work/ios-guest" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_SYSTEM_PROCESSOR=arm64 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$sdk" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_CXX_COMPILER="$compiler" \
  -DCMAKE_BUILD_TYPE=Release \
  -DXENON_RECOMP_OUT="$fixture_dir" \
  -DSIMDE_DIR="$fixture_dir" \
  -DSKYRT_SHIM="$fixture_dir/imports.cpp" \
  -DSKYRT_DISPATCH="$fixture_dir/dispatch.cpp"
cmake --build "$work/ios-guest" --parallel 2
xcrun lipo "$work/ios-guest/libskyrim_guest.a" -verify_arch arm64
cp "$work/ios-guest/libskyrim_guest.a" "$output_dir/libskyrim_guest.a"

# One Rust staticlib contains the texture probe and public CPU/runtime libraries.
# Only these explicit source directories enter the diagnostic app workspace.
cp -R "$harness_root/ios-probe/bridge" "$work/bridge"
cargo test --locked --manifest-path "$work/bridge/Cargo.toml" -- --test-threads=1 \
  2>&1 | tee "$native_output/bridge-tests.log"
cargo build --locked --release --lib --target aarch64-apple-ios \
  --manifest-path "$work/bridge/Cargo.toml"
probe_dir="$work/ios-probe"
mkdir -p "$probe_dir/Libraries" "$probe_dir/bridge/include"
cp "$harness_root/ios-probe/project.yml" "$probe_dir/project.yml"
cp -R "$harness_root/ios-probe/app" "$probe_dir/app"
cp -R "$harness_root/ios-probe/tests" "$probe_dir/tests"
bash "$harness_root/ci/prepare-cpu-support.sh" "$source_dir" "$simde_dir" "$probe_dir"
bash "$harness_root/ci/build-owned-abi-framework.sh" "$probe_dir" iphoneos
cp "$work/bridge/include/XenosProbeBridge.h" "$probe_dir/bridge/include/"
cp "$CARGO_TARGET_DIR/aarch64-apple-ios/release/libxenos_probe_bridge.a" "$probe_dir/Libraries/"
cp "$output_dir/libskyrim_guest.a" "$probe_dir/Libraries/"
xcrun lipo "$probe_dir/Libraries/libxenos_probe_bridge.a" -verify_arch arm64
(
  cd "$probe_dir"
  xcodegen generate --spec project.yml
  xcodebuild -project SkyrimProbe.xcodeproj -scheme SkyrimProbe \
    -configuration Release -destination 'generic/platform=iOS' \
    -derivedDataPath "$work/DerivedData" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
)
app="$work/DerivedData/Build/Products/Release-iphoneos/SkyrimProbe.app"
test -d "$app"
xcrun lipo "$app/SkyrimProbe" -verify_arch arm64
xcrun vtool -show-build "$app/SkyrimProbe"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Info.plist")" = com.halc8312.skyrimprobe
test ! -e "$app/embedded.mobileprovision"
test ! -d "$app/_CodeSignature"
package_dir="$work/package"
probe_output="$harness_root/out/ios-probe"
mkdir -p "$package_dir/Payload" "$probe_output"
cp "$probe_dir/Libraries/libxenos_probe_bridge.a" "$probe_output/"
"$rust_nm" --extern-only --defined-only --no-demangle "$probe_output/libxenos_probe_bridge.a" > "$probe_output/runtime-exports.txt"
shasum -a 256 "$probe_output/libxenos_probe_bridge.a"
cp -R "$app" "$package_dir/Payload/"
(
  cd "$package_dir"
  zip -q -r "$probe_output/SkyrimProbe-0.2.0-unsigned.ipa" Payload
)
shasum -a 256 "$probe_output/SkyrimProbe-0.2.0-unsigned.ipa"
bash "$harness_root/ci/run-ios-simulator-tests.sh" "$work" "$source_dir" "$fixture_dir" "$simde_dir"
printf '%s\n' 'Source-only diagnostic IPA and Simulator tests finished. Skyrim execution, a complete iOS host, IPA signing and physical iPhone behavior were not tested.'
