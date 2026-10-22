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

xcodebuild -version
# Xcode 26 distributes the offline Metal compiler as a separate component.
# A fresh hosted machine may include the SDK but not this compiler.
if ! xcrun --sdk iphoneos metal --version >/dev/null 2>&1; then
  xcodebuild -downloadComponent MetalToolchain
fi
xcrun --sdk iphoneos metal --version
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

# Override the upstream WebAssembly nightly: these four source-only crates
# already passed their Windows tests with Rust 1.88.0.
export RUSTUP_TOOLCHAIN=1.88.0
export CARGO_TARGET_DIR="$work/rust-target"
export IPHONEOS_DEPLOYMENT_TARGET=26.0
if ! command -v rustup >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location \
    https://sh.rustup.rs -o "$work/rustup-init.sh"
  sh "$work/rustup-init.sh" -y --profile minimal --default-toolchain 1.88.0 --no-modify-path
  export PATH="$HOME/.cargo/bin:$PATH"
fi
rustup toolchain install 1.88.0 --profile minimal
rustup target add --toolchain 1.88.0 aarch64-apple-ios
packages=(-p xex -p xenos-texture -p xenos-shader -p xenos-state)
(
  cd "$source_dir"
  cargo test --locked "${packages[@]}"
  cargo build --locked --lib --target aarch64-apple-ios "${packages[@]}"
)

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

# The Rust C bridge depends only on the pinned checkout's texture crate.
# Only these explicit source directories enter the diagnostic app workspace.
cp -R "$harness_root/ios-probe/bridge" "$work/bridge"
cargo test --locked --manifest-path "$work/bridge/Cargo.toml"
cargo build --locked --release --lib --target aarch64-apple-ios \
  --manifest-path "$work/bridge/Cargo.toml"
probe_dir="$work/ios-probe"
mkdir -p "$probe_dir/Libraries" "$probe_dir/bridge/include"
cp "$harness_root/ios-probe/project.yml" "$probe_dir/project.yml"
cp -R "$harness_root/ios-probe/app" "$probe_dir/app"
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
cp -R "$app" "$package_dir/Payload/"
(
  cd "$package_dir"
  zip -q -r "$probe_output/SkyrimProbe-0.1.0-unsigned.ipa" Payload
)
shasum -a 256 "$probe_output/SkyrimProbe-0.1.0-unsigned.ipa"
printf '%s\n' 'Source-only diagnostic IPA built. Skyrim execution, a complete iOS host, IPA signing and device behavior were not tested.'
