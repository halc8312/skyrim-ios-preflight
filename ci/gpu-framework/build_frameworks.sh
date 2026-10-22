#!/bin/bash
# Source-only Mac CI kit. Does not schedule cloud work, sign, install, or run apps.
# No game inputs, generated guest source, private objects, Apple SDK copies or
# credentials are accepted. Public source/dependency downloads happen only when
# this script is explicitly run later on an Apple Mac with official Xcode.
set -euo pipefail

[[ "$(uname -s)" == Darwin ]] || { echo 'Requires a Mac and official Xcode; no Linux/.so fallback.' >&2; exit 2; }
kit="$(cd -- "$(dirname -- "$0")" && pwd)"
work="${WGPU_PREFLIGHT_WORK_DIR:-${CM_BUILD_DIR:-$PWD}/.wgpu-preflight}"
output="${WGPU_PREFLIGHT_OUTPUT_DIR:-$work/out}"
source_dir="${WGPU_PUBLIC_SOURCE_DIR:-$work/source}"
toolchain="${WGPU_RUST_TOOLCHAIN:-nightly-2026-10-11}"
minimum="${WGPU_IPHONEOS_DEPLOYMENT_TARGET:-26.0}"
pin=6aed50955d934ac36049ba8d002034841633ae02
header_pin=673658bc2bd70ec39fc55ebe6bb0173cf6d0a603
[[ "$minimum" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo 'Invalid deployment target' >&2; exit 2; }
[[ ! -e "$output" ]] || { echo 'Output already exists; choose a fresh output directory.' >&2; exit 2; }
# xcrun must find the installed official SDKs before any source downloads.
device_sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
sim_sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
[[ -d "$device_sdk" && -d "$sim_sdk" ]] || exit 2
mkdir -p "$work" "$output"
host_source="$work/coexistence-host"
mkdir -p "$host_source/src"
cp "$kit/coexistence-host/Cargo.toml" "$kit/coexistence-host/Cargo.lock" "$host_source/"
cp "$kit/coexistence-host/src/lib.rs" "$host_source/src/lib.rs"
export CARGO_TARGET_DIR="$work/cache/target"
export RUSTUP_TOOLCHAIN="$toolchain"
export IPHONEOS_DEPLOYMENT_TARGET="$minimum"
export WGPU_NATIVE_VERSION=v29.0.1.1
export CARGO_PROFILE_RELEASE_LTO=false
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=8
export CARGO_PROFILE_RELEASE_PANIC=unwind
# Do not inherit caller RUSTFLAGS such as force_load/flat_namespace/allow-duplicate.
export RUSTFLAGS='-C lto=off -C embed-bitcode=no -C panic=unwind'
unset CARGO_ENCODED_RUSTFLAGS || true

if [[ ! -e "$source_dir" ]]; then
    git init "$source_dir"
    git -C "$source_dir" remote add origin https://github.com/gfx-rs/wgpu-native.git
    git -C "$source_dir" fetch --depth 1 origin "$pin"
    git -C "$source_dir" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$pin" ]] || { echo 'Unexpected cached wgpu-native commit' >&2; exit 2; }
[[ "$(git -C "$source_dir" config -f .gitmodules submodule.ffi/webgpu-headers.url)" == https://github.com/webgpu-native/webgpu-headers ]] || exit 2
[[ "$(git -C "$source_dir" ls-tree HEAD ffi/webgpu-headers | awk '{print $3}')" == "$header_pin" ]] || exit 2
git -C "$source_dir" submodule update --init --depth 1 ffi/webgpu-headers
[[ "$(git -C "$source_dir/ffi/webgpu-headers" rev-parse HEAD)" == "$header_pin" ]] || exit 2
[[ -z "$(git -C "$source_dir" status --porcelain --untracked-files=normal)" ]] || { echo 'Cached public source is modified' >&2; exit 2; }

if ! rustup run "$toolchain" rustc --version >/dev/null 2>&1; then
    rustup toolchain install "$toolchain" --profile minimal
fi
rustup target add --toolchain "$toolchain" aarch64-apple-ios aarch64-apple-ios-sim
rustc --version --verbose > "$output/rust-toolchain.txt"
xcodebuild -version > "$output/xcode-version.txt"
git -C "$source_dir" rev-parse HEAD > "$output/source-commit.txt"
git -C "$source_dir/ffi/webgpu-headers" rev-parse HEAD > "$output/headers-commit.txt"
python3 -m unittest discover -s "$kit" -p test_validate_framework.py -v

for variant in device simulator; do
    if [[ "$variant" == device ]]; then
        target=aarch64-apple-ios
        sdk="$device_sdk"
        sdk_name=iphoneos
        clang_target="arm64-apple-ios$minimum"
        platform=2
        bundle_platform=iPhoneOS
    else
        target=aarch64-apple-ios-sim
        sdk="$sim_sdk"
        sdk_name=iphonesimulator
        clang_target="arm64-apple-ios$minimum-simulator"
        platform=7
        bundle_platform=iPhoneSimulator
    fi
    export SDKROOT="$sdk"
    # bindgen uses Apple's real libclang and the target-specific xcrun SDK path.
    export LIBCLANG_PATH="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib"
    [[ -e "$LIBCLANG_PATH/libclang.dylib" ]] || { echo 'Official Xcode libclang not found' >&2; exit 2; }
    clang="$(xcrun --sdk "$sdk_name" --find clang)"
    cargo rustc --manifest-path "$source_dir/Cargo.toml" --locked --release --lib \
        --target "$target" --crate-type cdylib --no-default-features --features metal,wgsl \
        -- -C link-arg=-Wl,-headerpad_max_install_names 2>&1 | tee "$output/$variant-wgpu-build.log"
    framework="$output/$variant/WgpuNative.framework"
    mkdir -p "$framework/Headers" "$framework/Modules"
    cp "$CARGO_TARGET_DIR/$target/release/libwgpu_native.dylib" "$framework/WgpuNative"
    xcrun install_name_tool -id '@rpath/WgpuNative.framework/WgpuNative' "$framework/WgpuNative"
    cp "$source_dir/ffi/wgpu.h" "$source_dir/ffi/webgpu-headers/webgpu.h" "$framework/Headers/"
    printf 'framework module WgpuNative {\n  umbrella header "wgpu.h"\n  export *\n}\n' > "$framework/Modules/module.modulemap"
    python3 - "$source_dir" "$framework" "$minimum" "$bundle_platform" <<'PY'
from pathlib import Path
import plistlib
import shutil
import sys
source, framework = map(Path, sys.argv[1:3])
info = {
    "CFBundleExecutable": "WgpuNative", "CFBundleIdentifier": "org.local.publicprobe.wgpunative",
    "CFBundleName": "WgpuNative", "CFBundlePackageType": "FMWK",
    "CFBundleVersion": "29.0.1", "CFBundleShortVersionString": "29.0.1",
    "MinimumOSVersion": sys.argv[3], "CFBundleSupportedPlatforms": [sys.argv[4]],
    "WgpuNativeSourceTag": "v29.0.1.1",
}
with (framework / "Info.plist").open("xb") as output:
    plistlib.dump(info, output)
licenses = list(source.glob("LICENSE*"))
if not licenses:
    raise SystemExit("Missing upstream licenses")
for license_file in licenses:
    if license_file.is_file():
        shutil.copy2(license_file, framework / license_file.name)
header_license = source / "ffi/webgpu-headers/LICENSE"
if header_license.is_file():
    shutil.copy2(header_license, framework / "LICENSE.webgpu-headers")
PY
    python3 "$kit/validate_framework.py" "$framework/WgpuNative" --platform "$platform" \
        --minimum-os "$minimum" --output "$output/$variant-framework-metadata.json"
    xcrun otool -L "$framework/WgpuNative" > "$output/$variant-system-loads.txt"
    # The tiny scratch source is refreshed on every run so cargo emits the
    # native dependency report even when all heavy wgpu dependencies are cached.
    touch "$host_source/src/lib.rs"
    cargo rustc --manifest-path "$host_source/Cargo.toml" --locked --release --lib \
        --target "$target" -- --print=native-static-libs 2>&1 | tee "$output/$variant-core-build.log"
    # Parse rustc's actual native dependency flags into an argv array, never eval.
    python3 - "$output" "$variant" "$clang" "$clang_target" "$sdk" "$kit" "$CARGO_TARGET_DIR" "$target" <<'PY'
from pathlib import Path
import re
import shlex
import subprocess
import sys
output, variant, clang, target, sdk, kit, cache, rust_target = sys.argv[1:]
output, kit, cache = map(Path, (output, kit, cache))
text = (output / (variant + "-core-build.log")).read_text()
matches = re.findall(r"native-static-libs:\s*([^\r\n]+)", text)
if len(matches) != 1:
    raise SystemExit("Expected one rustc native-static-libs report")
flags = shlex.split(matches[0])
index = 0
while index < len(flags):
    flag = flags[index]
    if re.fullmatch(r"-l[A-Za-z0-9_+.-]+", flag):
        index += 1
    elif flag == "-framework" and index + 1 < len(flags) and re.fullmatch(r"[A-Za-z0-9_]+", flags[index + 1]):
        index += 2
    else:
        raise SystemExit("Unexpected native link dependency flag")
framework_parent = output / variant
binary = framework_parent / "WgpuNative.framework" / "WgpuNative"
command = [clang, "-target", target, "-isysroot", sdk, "-std=c17",
    "-I", str(binary.parent / "Headers"), str(kit / "coexistence.c"),
    str(cache / rust_target / "release/libpublic_core_coexistence.a"),
    "-F", str(framework_parent), "-framework", "WgpuNative",
    "-Wl,-undefined,error", "-Wl,-rpath,@executable_path/Frameworks", *flags,
    "-o", str(framework_parent / "CoexistenceLinkProbe")]
subprocess.run(command, check=True)
PY
done
python3 - "$output" <<'PY'
from pathlib import Path
import json
import sys
output = Path(sys.argv[1])
with (output / "preflight-result.json").open("x") as stream:
    json.dump({"public_source_only": True, "device_and_simulator_frameworks_built": True,
        "synthetic_rust_staticlib_and_framework_strict_links": 2,
        "real_runtime_staticlib_link_verified": False, "gpu_runtime_verified": False,
        "coexistence_main_executed": False, "app_packaged_or_signed": False,
        "sdk_files_exported": False}, stream, indent=2)
    stream.write("\n")
PY
echo 'PUBLIC framework metadata and two strict links passed; no app/GPU execution performed.'
