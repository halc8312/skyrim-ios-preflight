#!/bin/bash
# Real Apple SDK and a dedicated iOS Simulator. Only authored DLLs are used.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/skse-dll-sdk.XXXXXXXX")"
pushd "$here" >/dev/null

CC=clang python3 -B -m unittest -v test_probe_local_file test_probe_directory test_skse_plugin_vm test_pe_image \
    test_dependency_plan test_probe_asset_file
device_sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
sources=(pe_image.c x86_call.c x86_heap.c x86_winthread.c x86_winfile.c x86_winevent.c dependency_plan.c plugin_trial.c
    probe_local_file.c probe_directory.c probe_asset_file.c)
for source in "${sources[@]}"; do
    xcrun --sdk iphoneos clang -target arm64-apple-ios18.0 -isysroot "$device_sdk" \
        -std=c11 -O2 -Wall -Wextra -Werror -c "$source" -o "$work/${source%.c}.o"
done
xcrun --sdk iphoneos clang -target arm64-apple-ios18.0 -isysroot "$device_sdk" -fmodules -fobjc-arc \
    -Wall -Wextra -Werror -fsyntax-only mod_dll_probe.m
xcrun --sdk iphoneos clang -target arm64-apple-ios18.0 -isysroot "$device_sdk" -fmodules -fobjc-arc \
    -DSKYRIM_IOS_SKSE_DLL_PROBE=1 -DSKYRIM_IOS_CLASSIC_SKSE=1 -I"$here" -I"$root/ios/private-abi" \
    -fsyntax-only "$root/ios/runtime-port/mod_manager.m"

app="$work/DllCompatibility.app"
mkdir "$app"
python3 -B - "$app" <<'PY'
from pathlib import Path
import plistlib
import sys
from test_probe_local_file import supported_fixture
from test_probe_directory import file_asset_fixture, helper, primary
from test_pe_image import DATA, add_named_clock, zero_string

app = Path(sys.argv[1])
fixture = supported_fixture()
(app / "supported.dll").write_bytes(fixture)
add_named_clock(fixture)
zero_string(fixture, DATA + 0xa2, b"OwnedUnsupported")
(app / "unsupported.dll").write_bytes(fixture)
package = app / "ProbePackage" / "Data" / "SKSE" / "Plugins"
package.mkdir(parents=True)
(package / "Supported.dll").write_bytes(supported_fixture())
(package / "Unsupported.dll").write_bytes(fixture)
(package / "Paired.dll").write_bytes(primary())
(package / "OwnedWorker.dll").write_bytes(helper())
(package / "AssetProbe.dll").write_bytes(file_asset_fixture())
(package / "Settings.ini").write_bytes(b"Authored settings")
(app / "Info.plist").write_bytes(plistlib.dumps({
    "CFBundleIdentifier": "org.skyrimios.tests.dllcompatibility",
    "CFBundleName": "DLL Compatibility Fixture",
    "CFBundleExecutable": "DllCompatibility",
    "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
    "LSRequiresIPhoneOS": True,
    "MinimumOSVersion": "18.0",
    "UIDeviceFamily": [1],
    "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False},
    "UILaunchScreen": {},
}))
PY
simulator_sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
xcrun --sdk iphonesimulator clang -target "$(uname -m)-apple-ios18.0-simulator" -isysroot "$simulator_sdk" \
    -fobjc-arc -fmodules -O2 -Wall -Wextra -Werror "${sources[@]}" mod_dll_probe.m test_simulator.m \
    -framework UIKit -framework Foundation -framework UniformTypeIdentifiers -o "$app/DllCompatibility"
codesign --force --sign - "$app"
popd >/dev/null

device="$(python3 "$root/tests/mod-simulator/prepare.py" device)"
[[ "$device" =~ ^[0-9A-Fa-f-]{36}$ ]]
trap 'xcrun simctl shutdown "$device" >/dev/null 2>&1 || true' EXIT
xcrun simctl boot "$device"
xcrun simctl bootstatus "$device" -b
xcrun simctl install "$device" "$app"
bundle=org.skyrimios.tests.dllcompatibility
container="$(xcrun simctl get_app_container "$device" "$bundle" data)"
for language in english japanese; do
    if [[ "$language" == japanese ]]; then
        xcrun simctl launch --terminate-running-process "$device" "$bundle" -TestJapanese
    else
        xcrun simctl launch --terminate-running-process "$device" "$bundle"
    fi
    evidence="$container/Documents/$language.txt"
    for ((i=0; i<50; ++i)); do
        if [[ -s "$evidence" ]]; then break; fi
        sleep 1
    done
    grep -Fxq refused,passed,folder "$evidence"
    echo "DLL_SIMULATOR: ${language} verified the single-file results, the MOD folder and its read-only asset."
done
codesign --verify --strict "$app"
