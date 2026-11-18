#!/bin/bash
# Read-only MOD import. Each SDK gets its own static libraries and headers.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
base="${SKYRIM_IOS_DEPS_ROOT:-$root/local/ios-dependencies-build15}"
platform="${1:?expected iphoneos, iphonesimulator, macosx, or native}"
jobs="${JOBS:-2}"
deployment="${IPHONEOS_DEPLOYMENT_TARGET:-18.0}"
unset SDKROOT IPHONEOS_DEPLOYMENT_TARGET MACOSX_DEPLOYMENT_TARGET
host_triplet=
case "$platform" in
  iphoneos|iphonesimulator|macosx)
    sdk="$(xcrun --sdk "$platform" --show-sdk-path)"
    export SDKROOT="$sdk"
    export CC AR RANLIB
    CC="$(xcrun --sdk "$platform" --find clang)"
    AR="$(xcrun --sdk "$platform" --find ar)"
    RANLIB="$(xcrun --sdk "$platform" --find ranlib)"
    sdk_identity="$(xcrun --sdk "$platform" --show-sdk-build-version)"
    flags="-O2 -fPIC -isysroot $sdk"
    if [[ "$platform" != macosx ]]; then
      target="arm64-apple-ios$deployment"
      [[ "$platform" != iphonesimulator ]] || target="$target-simulator"
      flags="$flags -target $target"
      host_triplet=aarch64-apple-darwin
    fi
    ;;
  native)
    : "${CC:=cc}"
    export CC
    sdk_identity="$(uname -sm)"
    flags="-O2 -fPIC"
    ;;
  *) echo "Unknown archive-library platform: $platform" >&2; exit 2 ;;
esac
work="$base/mod-archive/$platform"
archives="$base/mod-archive/downloads"
prefix="$work/prefix"
[[ "$platform" != iphoneos ]] || prefix="$base/prefix"
mkdir -p "$archives" "$work" "$prefix/lib" "$prefix/include" "$prefix/share/skyrim-ios"
recipe="$(shasum -a 256 "$0")"
key="$(printf '%s\n' "${recipe%% *}" "$platform" "$sdk_identity" "$flags" "$("$CC" --version)" | shasum -a 256 | cut -d' ' -f1)"
stage="$work/$key"
mkdir -p "$stage"
fetch() {
  local url="$1" digest="$2" filename="${1##*/}" temp
  if [[ ! -f "$archives/$filename" ]]; then
    temp="$(mktemp "$archives/.download-XXXXXX")"
    curl --fail --location --retry 3 "$url" -o "$temp"
    [[ "$(shasum -a 256 "$temp" | cut -d' ' -f1)" == "$digest" ]]
    mv "$temp" "$archives/$filename"
  fi
  [[ "$(shasum -a 256 "$archives/$filename" | cut -d' ' -f1)" == "$digest" ]] || {
    echo "Wrong source digest: $filename" >&2; exit 1
  }
  if [[ ! -d "$stage/${filename%.tar.xz}" ]]; then
    # Extract completely before publishing the source tree used by retries.
    temp="$(mktemp -d "$stage/extract-XXXXXX")"
    tar -xJf "$archives/$filename" -C "$temp"
    mv "$temp/${filename%.tar.xz}" "$stage/"
    rmdir "$temp"
  fi
}
xz_version=5.8.4
archive_version=3.8.11
fetch "https://github.com/tukaani-project/xz/releases/download/v$xz_version/xz-$xz_version.tar.xz" \
  4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b
fetch "https://github.com/libarchive/libarchive/releases/download/v$archive_version/libarchive-$archive_version.tar.xz" \
  a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2
xz="$stage/xz-$xz_version"
archive="$stage/libarchive-$archive_version"
install="$stage/install"
toolchain=("--build=$("$xz/build-aux/config.guess")")
[[ -z "$host_triplet" ]] || toolchain+=("--host=$host_triplet")
if [[ ! -f "$stage/complete" ]]; then
  mkdir -p "$stage/xz-build" "$stage/archive-build"
  export CFLAGS="$flags" LDFLAGS="$flags" CPPFLAGS=
  # Do not import Homebrew/pkg-config paths for a different SDK.
  export PKG_CONFIG=false
  (
    cd "$stage/xz-build"
    "$xz/configure" "${toolchain[@]}" --prefix="$install" --enable-static --disable-shared \
      --disable-encoders --disable-threads --disable-microlzma --disable-lzip-decoder \
      --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
      --disable-doc --disable-scripts --disable-nls
    make -C src/liblzma -j"$jobs" install
  )
  (
    cd "$stage/archive-build"
    # Registrar only uses unfiltered input. Disable subprocess filter helpers
    # and unavailable process/disk APIs even if the SDK exports legacy symbols.
    export ac_cv_func_fork=no ac_cv_func_vfork=no ac_cv_func_posix_spawnp=no
    if [[ "$platform" == iphoneos || "$platform" == iphonesimulator ]]; then
      export ac_cv_func_mknod=no
    fi
    export CPPFLAGS="-I$install/include" LDFLAGS="$flags -L$install/lib"
    "$archive/configure" "${toolchain[@]}" --prefix="$install" \
      --enable-static --disable-shared --disable-bsdtar --disable-bsdcpio \
      --disable-bsdcat --disable-bsdunzip --disable-xattr --disable-acl \
      --disable-posix-regex-lib --with-lzma --without-bz2lib --without-libb2 \
      --without-lz4 --without-zstd --without-lzo2 --without-cng --without-mbedtls \
      --without-nettle --without-openssl --without-md --without-xml2 --without-xmllite \
      --without-bsdxml --without-expat
    grep -q '^#define HAVE_LIBLZMA 1' config.h
    grep -q '^#define HAVE_ZLIB_H 1' config.h
    make -j"$jobs" libarchive.la
  )
  touch "$stage/complete"
fi
cp "$install/lib/liblzma.a" "$stage/archive-build/.libs/libarchive.a" "$prefix/lib/"
cp "$archive/libarchive/archive.h" "$archive/libarchive/archive_entry.h" "$install/include/lzma.h" "$prefix/include/"
mkdir -p "$prefix/include/lzma"
cp "$install/include/lzma/"*.h "$prefix/include/lzma/"
if [[ "$platform" != native ]]; then
  arch=arm64
  [[ "$platform" != macosx ]] || arch="$(uname -m)"
  for lib in lzma archive; do xcrun lipo "$prefix/lib/lib$lib.a" -verify_arch "$arch"; done
fi

# Ship the upstream license texts and per-file author notices with the binary.
python3 - "$archive" "$xz" "$prefix/share/skyrim-ios/ModArchiveLicenses.txt" <<'PY'
from pathlib import Path
import re, sys
archive, xz, out = map(Path, sys.argv[1:])
texts = ["libarchive", (archive / "COPYING").read_text(), "Additional libarchive source notices"]
seen = set()
for path in sorted((archive / "libarchive").glob("*.[ch]")):
    source = path.read_text(encoding="utf-8", errors="replace")
    leading = re.match(r"(?:\s*/\*.*?\*/)+", source, re.S)
    if leading:
        for comment in re.findall(r"/\*.*?\*/", leading[0], re.S):
            if re.search(r"copyright|public domain|SPDX-License-Identifier", comment, re.I) and comment not in seen:
                seen.add(comment)
                texts.append(comment)
texts.extend(["XZ Utils liblzma — Copyright (C) The XZ Utils authors and contributors",
              (xz / "COPYING.0BSD").read_text()])
out.write_text("\n\n".join(texts) + "\n", encoding="utf-8")
PY
printf 'Prepared %s MOD archive libraries (%s, %s)\n' "$platform" "$xz_version" "$archive_version"
