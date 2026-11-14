#!/bin/bash
# Build public sources only. This script does not sign, upload, or start CI.
set -euo pipefail
umask 077
[[ "$(uname -s)" == Darwin ]] || { echo 'Requires macOS and the Apple iPhoneOS SDK.' >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${SKYRIM_IOS_DEPS_ROOT:-$ROOT/local/ios-dependencies-build6}"
PREFIX="$WORK/prefix"
DEPLOYMENT="${IPHONEOS_DEPLOYMENT_TARGET:-18.0}"
JOBS="${JOBS:-2}"
for tool in xcrun xcodebuild cmake git curl tar make perl python3 flex bison; do
  command -v "$tool" >/dev/null || { echo "Missing host tool: $tool" >&2; exit 2; }
done
# vkd3d 2.0 uses GNU Make grouped targets (&:), introduced in Make 4.3.
# Apple's /usr/bin/make is 3.81; Homebrew's make formula installs gmake.
BUILD_MAKE="${SKYRIM_GNU_MAKE:-gmake}"
command -v "$BUILD_MAKE" >/dev/null || { echo 'GNU Make >=4.3 required (brew install make).' >&2; exit 2; }
MAKE_VERSION="$("$BUILD_MAKE" --version | head -n1)"
python3 - "$MAKE_VERSION" <<'PY'
import re, sys
match = re.search(r"GNU Make (\d+)\.(\d+)", sys.argv[1])
if not match or tuple(map(int, match.groups())) < (4, 3):
    raise SystemExit("GNU Make >=4.3 is required for vkd3d 2.0")
PY
# vkd3d HLSL grammar uses Bison features newer than Apple's bundled Bison 2.3.
case "$(bison --version | head -n1)" in *' 3.'*) ;; *) echo 'Put Bison 3.x in PATH before building vkd3d.' >&2; exit 2 ;; esac
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"
AR="$(xcrun --sdk iphoneos --find ar)"
RANLIB="$(xcrun --sdk iphoneos --find ranlib)"
FLAGS="-target arm64-apple-ios${DEPLOYMENT} -isysroot $SDK -O2"
mkdir -p "$WORK/sources" "$WORK/build" "$PREFIX/lib" "$PREFIX/include"
mkdir -p "$WORK/logs"
RUN_LOG="$(mktemp -d "$WORK/logs/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
exec > >(tee "$RUN_LOG/build.log") 2>&1
preserve_failure() {
  local result=$?
  trap - EXIT
  if [[ "$result" != 0 ]]; then
    [[ ! -f "$WORK/build/ffmpeg/ffbuild/config.log" ]] || cp "$WORK/build/ffmpeg/ffbuild/config.log" "$RUN_LOG/ffmpeg-config.log" || true
    [[ ! -f "$WORK/build/vkd3d/config.log" ]] || cp "$WORK/build/vkd3d/config.log" "$RUN_LOG/vkd3d-config.log" || true
    [[ ! -f "$WORK/sources/MoltenVK/dependenciesbuild.log" ]] || cp "$WORK/sources/MoltenVK/dependenciesbuild.log" "$RUN_LOG/moltenvk-dependencies.log" || true
    [[ ! -f "$WORK/sources/MoltenVK-ios-cachefix/dependenciesbuild.log" ]] || cp "$WORK/sources/MoltenVK-ios-cachefix/dependenciesbuild.log" "$RUN_LOG/moltenvk-cachefix-dependencies.log" || true
  fi
  exit "$result"
}
trap preserve_failure EXIT
BASE_CONFIG="SDL-5d249570393f7a37e037abf22cd6012a4cc56a71 FFmpeg-db69d06eeeab4f46da15030a80d539efb4503ca8 MoltenVK-458870543b9bcf6b0edd6f90aaa776707b310d96 vkd3d-2.0 ios-$DEPLOYMENT $(xcodebuild -version) $(xcrun --sdk iphoneos --show-sdk-version)"
CONFIG="$BASE_CONFIG $(shasum -a 256 "$0" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/patch_spirv_cross.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/patch_moltenvk.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/patch_mvk_hot_cache.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/mvk_hot_cache.cpp.in" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/patch_mvk_compressed_copy.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/patch_mvk_library.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/verify_lifecycle_baseline.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/dependencies/patch_mvk_compile_timing.py" | cut -d' ' -f1) $(shasum -a 256 "$ROOT/ios/runtime-port/performance_metrics.h" | cut -d' ' -f1)"
KEY="$(printf '%s' "$CONFIG" | shasum -a 256 | cut -d' ' -f1)"
if [[ -f "$WORK/configuration.sha256" && "$(cat "$WORK/configuration.sha256")" != "$KEY" ]]; then
  echo 'Dependency configuration changed; use a new SKYRIM_IOS_DEPS_ROOT to preserve previous build evidence.' >&2
  exit 2
fi
printf '%s\n' "$KEY" > "$WORK/configuration.sha256"

# Preserve the prior prefix. Only reuse unchanged libraries when its complete
# build-configuration key (including SDK and original script) matches exactly.
OLD_WORK="$ROOT/local/ios-dependencies"
OLD_KEY="$(printf '%s' "$BASE_CONFIG 65e19eb11f760c5c9ea9b84414bbb315c09fe8dd7abbb6b76a50863c66d08b0f" | shasum -a 256 | cut -d' ' -f1)"
if [[ ! -f "$WORK/reused-unchanged.done" && -f "$OLD_WORK/configuration.sha256" && "$(cat "$OLD_WORK/configuration.sha256")" == "$OLD_KEY" && -f "$OLD_WORK/build/moltenvk.done" ]]; then
  cp -R "$OLD_WORK/prefix/." "$PREFIX/"
  for dep in sdl ffmpeg vkd3d; do
    [[ -f "$OLD_WORK/build/$dep.done" ]] && cp "$OLD_WORK/build/$dep.done" "$WORK/build/$dep.done"
  done
  touch "$WORK/reused-unchanged.done"
fi

# Reuse only the unchanged SDL/FFmpeg/vkd3d archives from the installed build5
# configuration when the SDK/deployment and exact prior scripts also match.
# Its MoltenVK archive is copied with the prefix but must be rebuilt below.
DISPLAY_WORK="$ROOT/local/ios-dependencies-displayfix"
DISPLAY_KEY="$(printf '%s' "$BASE_CONFIG c96b20bea396e30131754af03b5c4989f20143b0867eea035a3452481eefe045 c097f6393f53603b69420e087cafcb4f4784daad71375eda31d7c9f66373c3bb" | shasum -a 256 | cut -d' ' -f1)"
if [[ ! -f "$WORK/reused-display-unchanged.done" && ! -f "$WORK/build/moltenvk.done" && -f "$DISPLAY_WORK/configuration.sha256" && "$(cat "$DISPLAY_WORK/configuration.sha256")" == "$DISPLAY_KEY" && -f "$DISPLAY_WORK/build/moltenvk.done" ]]; then
  cp -R "$DISPLAY_WORK/prefix/." "$PREFIX/"
  for dep in sdl ffmpeg vkd3d; do
    [[ -f "$DISPLAY_WORK/build/$dep.done" ]] && cp "$DISPLAY_WORK/build/$dep.done" "$WORK/build/$dep.done"
  done
  touch "$WORK/reused-display-unchanged.done"
fi

checkout() {
  local name="$1" url="$2" ref="$3" expected="$4" dir="$WORK/sources/$1"
  if [[ ! -d "$dir/.git" ]]; then
    git clone --depth 1 --branch "$ref" "$url" "$dir"
  fi
  [[ "$(git -C "$dir" rev-parse HEAD)" == "$expected" ]] || { echo "Unexpected public source revision: $name" >&2; exit 2; }
  [[ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ]] || { echo "Modified dependency source: $name" >&2; exit 2; }
}
archive() {
  local url="$1" path="$2" digest="$3"
  [[ -f "$path" ]] || curl --fail --location --retry 3 "$url" -o "$path"
  [[ "$(shasum -a 256 "$path" | cut -d' ' -f1)" == "$digest" ]] || { echo "Source SHA256 mismatch: $path" >&2; exit 2; }
}

checkout SDL https://github.com/libsdl-org/SDL.git release-2.32.10 5d249570393f7a37e037abf22cd6012a4cc56a71
checkout FFmpeg https://github.com/FFmpeg/FFmpeg.git n7.1.1 db69d06eeeab4f46da15030a80d539efb4503ca8
checkout MoltenVK https://github.com/KhronosGroup/MoltenVK.git v1.4.0 458870543b9bcf6b0edd6f90aaa776707b310d96
archive https://dl.winehq.org/vkd3d/source/vkd3d-2.0.tar.xz "$WORK/sources/vkd3d-2.0.tar.xz" 9ad29bb236808186a47ec66e853b21e6fca59b9dc62a9474b05d5e3eda2710ef
archive https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/6efe5ae6b7c6ad29da3f2cc3f164b9c3d72b78b1 "$WORK/sources/spirv.tar.gz" d035de73b4085301bf921dc66997ce993fbf7c0d78f3308e67b6be2513a8b88c
archive https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/v1.3.228 "$WORK/sources/vulkan.tar.gz" e5a4dcdfb9361ee8d1b9401ed337b65e7c34a54224b99a9cde303a650a9cb350
archive https://cpan.metacpan.org/authors/id/I/IS/ISHIGAKI/JSON-4.10.tar.gz "$WORK/sources/json.tar.gz" df8b5143d9a7de99c47b55f1a170bd1f69f711935c186a6dc0ab56dd05758e35
[[ -d "$WORK/sources/vkd3d-2.0" ]] || tar -xJf "$WORK/sources/vkd3d-2.0.tar.xz" -C "$WORK/sources"
[[ -d "$WORK/sources/SPIRV-Headers-6efe5ae6b7c6ad29da3f2cc3f164b9c3d72b78b1" ]] || tar -xzf "$WORK/sources/spirv.tar.gz" -C "$WORK/sources"
[[ -d "$WORK/sources/Vulkan-Headers-1.3.228" ]] || tar -xzf "$WORK/sources/vulkan.tar.gz" -C "$WORK/sources"
[[ -d "$WORK/sources/JSON-4.10" ]] || tar -xzf "$WORK/sources/json.tar.gz" -C "$WORK/sources"

if [[ ! -f "$WORK/build/sdl.done" ]]; then
  cmake -S "$WORK/sources/SDL" -B "$WORK/build/sdl" -G 'Unix Makefiles' \
    "-DCMAKE_MAKE_PROGRAM=$(command -v "$BUILD_MAKE")" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT" "-DCMAKE_INSTALL_PREFIX=$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL_VULKAN=ON -DSDL_METAL=ON -DSDL2_DISABLE_INSTALL=OFF -DSDL2_DISABLE_SDL2MAIN=OFF
  cmake --build "$WORK/build/sdl" --parallel "$JOBS"
  cmake --install "$WORK/build/sdl"
  touch "$WORK/build/sdl.done"
fi

if [[ ! -f "$WORK/build/ffmpeg.done" ]]; then
  mkdir -p "$WORK/build/ffmpeg"
  (
    cd "$WORK/build/ffmpeg"
    "$WORK/sources/FFmpeg/configure" --prefix="$PREFIX" --target-os=darwin --arch=aarch64 \
      --enable-cross-compile --cc="$CC" --objcc="$CC" --ar="$AR" --ranlib="$RANLIB" --sysroot="$SDK" \
      --extra-cflags="$FLAGS" --extra-objcflags="$FLAGS" --extra-ldflags="$FLAGS" --enable-pic \
      --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
      --disable-autodetect --disable-network --disable-everything \
      --disable-avdevice --disable-avfilter --disable-postproc \
      --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale \
      --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,wmav2,wmapro \
      --enable-demuxer=bink --enable-protocol=file
    "$BUILD_MAKE" -j"$JOBS"
    "$BUILD_MAKE" install
  )
  touch "$WORK/build/ffmpeg.done"
fi

if [[ ! -f "$WORK/build/vkd3d.done" ]]; then
  mkdir -p "$WORK/build/vkd3d"
  (
    cd "$WORK/build/vkd3d"
    export PERL5LIB="$WORK/sources/JSON-4.10/lib${PERL5LIB:+:$PERL5LIB}"
    export CPPFLAGS="-target arm64-apple-ios${DEPLOYMENT} -isysroot $SDK -I$WORK/sources/Vulkan-Headers-1.3.228/include -I$WORK/sources/SPIRV-Headers-6efe5ae6b7c6ad29da3f2cc3f164b9c3d72b78b1/include"
    export CC AR RANLIB
    export OBJC="$CC" OBJCFLAGS="$FLAGS"
    export CFLAGS="$FLAGS" LDFLAGS="$FLAGS" SONAME_LIBVULKAN=libMoltenVK.dylib
    # vkd3d 2.0 configure.ac initializes am__fastdepOBJC only inside its Metal
    # probe. --without-metal skips both branches and breaks configure itself.
    # Keep the real SDK probe enabled; only libvkd3d-shader is built below.
    WIDL=no "$WORK/sources/vkd3d-2.0/configure" --host=aarch64-apple-darwin --prefix="$PREFIX" \
      --enable-static --disable-shared --disable-doxygen-doc --disable-demos --disable-tests \
      --without-xcb --without-ncurses --without-opengl --without-spirv-tools
    # A direct library target bypasses Automake's all -> BUILT_SOURCES ordering.
    "$BUILD_MAKE" -j"$JOBS" include/private/vkd3d_version.h include/private/spirv_grammar.h \
      libs/vkd3d-shader/hlsl.tab.h libs/vkd3d-shader/preproc.tab.h
    "$BUILD_MAKE" -j"$JOBS" libvkd3d-shader.la
    cp .libs/libvkd3d-shader.a "$PREFIX/lib/"
    mkdir -p "$PREFIX/include/vkd3d"
    cp "$WORK/sources/vkd3d-2.0/include/vkd3d_shader.h" "$WORK/sources/vkd3d-2.0/include/vkd3d_types.h" "$PREFIX/include/vkd3d/"
  )
  touch "$WORK/build/vkd3d.done"
fi

if [[ ! -f "$WORK/build/moltenvk.done" ]]; then
  CROSS="$WORK/sources/SPIRV-Cross-ios-samplerfix"
  if [[ ! -d "$CROSS/.git" ]]; then
    git init "$CROSS"
    git -C "$CROSS" remote add origin https://github.com/KhronosGroup/SPIRV-Cross.git
    git -C "$CROSS" fetch --depth=1 origin 0a88b2d5c08708d45692b7096a0a84e7bfae366c
    git -C "$CROSS" checkout --detach FETCH_HEAD
  fi
  [[ "$(git -C "$CROSS" rev-parse HEAD)" == 0a88b2d5c08708d45692b7096a0a84e7bfae366c ]]
  # This named build-only checkout is intentionally patched; vendor stays intact.
  python3 "$ROOT/ios/dependencies/patch_spirv_cross.py" "$CROSS/spirv_msl.cpp" --in-place
  MVK="$WORK/sources/MoltenVK-ios-cachefix"
  if [[ ! -d "$MVK/.git" ]]; then
    git clone --no-hardlinks "$WORK/sources/MoltenVK" "$MVK"
  fi
  [[ "$(git -C "$MVK" rev-parse HEAD)" == 458870543b9bcf6b0edd6f90aaa776707b310d96 ]]
  # Preserve the pristine source checkout. Admit only this exact, idempotent
  # upstream backport in a separate named build stage.
  python3 - "$MVK" <<'PYMVK'
from pathlib import Path
import subprocess, sys
root=Path(sys.argv[1])
changed=subprocess.check_output(['git','-C',str(root),'diff','--name-only','HEAD'],text=True).splitlines()
if set(changed)-{'MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp',
                'MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h',
                'MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm',
                'MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h',
                'MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm',
                'MoltenVK/MoltenVK/GPUObjects/MVKSync.mm'}:
    raise SystemExit('Unreviewed changes in MoltenVK build stage')
PYMVK
  python3 "$ROOT/ios/dependencies/patch_moltenvk.py" \
    "$MVK/MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp" --in-place
  python3 "$ROOT/ios/dependencies/patch_mvk_hot_cache.py" "$MVK/MoltenVK/MoltenVK/GPUObjects"
  python3 "$ROOT/ios/dependencies/patch_mvk_library.py" "$MVK/MoltenVK/MoltenVK/GPUObjects" \
    --pristine "$WORK/sources/MoltenVK/MoltenVK/MoltenVK/GPUObjects"
  # The admission gate failed real Metal event-dependency validation. Keep
  # original native submission methods and reject any cached gated variant.
  python3 "$ROOT/ios/dependencies/verify_lifecycle_baseline.py" "$MVK/MoltenVK/MoltenVK"
  python3 "$ROOT/ios/dependencies/patch_mvk_compile_timing.py" "$MVK/MoltenVK/MoltenVK/GPUObjects" \
    --header "$ROOT/ios/runtime-port/performance_metrics.h"
  (
    cd "$MVK"
    ./fetchDependencies --ios --keep-cache --spirv-cross-root "$CROSS"
    # Upstream's make wrapper converts command-line variables to C macros;
    # use xcodebuild directly so signing/SDK/deployment are real build settings.
    # Only the app-linked static archive is needed. The upstream packaging
    # scheme also links an unused dynamic framework. The static archive keeps
    # the application's strong performance-observation hooks.
    xcodebuild build -project MoltenVK/MoltenVK.xcodeproj -target MoltenVK-iOS-static \
      -configuration Release -sdk iphoneos -jobs "$JOBS" \
      CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
      CONFIGURATION_BUILD_DIR="$WORK/build/moltenvk-static" \
      IPHONEOS_DEPLOYMENT_TARGET="$DEPLOYMENT" \
      'GCC_PREPROCESSOR_DEFINITIONS=$(inherited) MVK_HIDE_VULKAN_SYMBOLS=0'
    cp "$WORK/build/moltenvk-static/libMoltenVK.a" "$PREFIX/lib/"
    cp -pRL MoltenVK/include/. "$PREFIX/include/"
    # Keep complete symbol evidence. Only performance observations are added;
    # the rejected lifecycle gate must not be linked into the dependency.
    xcrun nm -u "$PREFIX/lib/libMoltenVK.a" > "$RUN_LOG/moltenvk-undefined.txt"
    if grep -q '_skyrim_gpu_' "$RUN_LOG/moltenvk-undefined.txt"; then
      echo 'Rejected lifecycle gate still referenced by MoltenVK.' >&2; exit 1
    fi
    for symbol in skyrim_perf_now_ns skyrim_perf_interval skyrim_perf_end; do
      grep -q "_$symbol" "$RUN_LOG/moltenvk-undefined.txt"
    done
  )
  touch "$WORK/build/moltenvk.done"
fi

python3 "$ROOT/ios/dependencies/verify_lifecycle_baseline.py" "$WORK/sources/MoltenVK-ios-cachefix/MoltenVK/MoltenVK"
for name in SDL2 SDL2main avformat avcodec avutil swresample swscale vkd3d-shader MoltenVK; do
  [[ -s "$PREFIX/lib/lib$name.a" ]] || { echo "Missing static archive: $name" >&2; exit 1; }
  xcrun lipo "$PREFIX/lib/lib$name.a" -verify_arch arm64
done
printf 'Prepared iPhoneOS dependencies: %s\n' "$PREFIX"
