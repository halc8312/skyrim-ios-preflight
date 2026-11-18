#!/bin/bash
# Run the MOD settings UI suite with simulator libraries and public SDL headers.
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo "The iOS Simulator requires macOS." >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
base="${SKYRIM_IOS_DEPS_ROOT:-$root/local/ios-dependencies-build15}"
mkdir -p local/mod-simulator-checks
run="$(mktemp -d "$root/local/mod-simulator-checks/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
artifacts="$run/artifacts"
mkdir -p "$artifacts"
exec > >(tee "$artifacts/ci.log") 2>&1

# Read the exact renderer source pin without running its iPhone build recipe.
pin="$(awk '$1 == "checkout" && $2 == "SDL" { n++; print $3, $4, $5 } END { if (n != 1) exit 1 }' \
    ios/dependencies/build-dependencies.sh)"
read -r url ref revision <<< "$pin"
[[ "$url" == https://github.com/libsdl-org/SDL.git && "$revision" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Invalid pinned SDL source." >&2; exit 1
}
source_dir="$base/sources/SDL"
if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
    source_dir="$run/SDL"
    git clone --quiet --no-checkout --depth 1 --branch "$ref" "$url" "$source_dir"
fi
[[ "$(git -C "$source_dir" rev-parse --verify "$revision^{commit}")" == "$revision" ]]
sdl="$run/sdl-headers"
mkdir -p "$sdl/include/SDL2"
git -C "$source_dir" archive "$revision:include" | tar -xf - -C "$sdl/include/SDL2"
# The pinned SDL_config.h dispatches to the checked-in iOS configuration.
sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
xcrun --sdk iphonesimulator clang -target "arm64-apple-ios${IPHONEOS_DEPLOYMENT_TARGET:-18.0}-simulator" \
    -isysroot "$sdk" -I"$sdl/include" -fsyntax-only -x objective-c - <<'OBJC'
#include <TargetConditionals.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#if !TARGET_OS_SIMULATOR || !defined(__IPHONEOS__) || !SDL_VIDEO_DRIVER_UIKIT
#error Expected the SDL UIKit configuration for the iOS Simulator
#endif
void checkWindow(SDL_SysWMinfo *info) { (void)info->info.uikit.window; }
OBJC

bash scripts/fetch-mod-codecs.sh
bash ios/dependencies/build-mod-archive-dependencies.sh iphonesimulator
status=0
bash scripts/check-mod-simulator.sh "$run/check" "$sdl" "$base/mod-archive/iphonesimulator/prefix" || status=$?
for entry in evidence ModScreens.xcresult; do
    if [[ -d "$run/check/$entry" ]]; then
        ditto -c -k --keepParent "$run/check/$entry" "$artifacts/$entry.zip"
    fi
done
(( status == 0 )) || exit "$status"
test -s "$artifacts/ModScreens.xcresult.zip"
test -s "$artifacts/evidence.zip"
