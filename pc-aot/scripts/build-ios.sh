#!/bin/bash
set -euo pipefail
umask 077

MODE="${1:-unsigned}"
case "$MODE" in unsigned|ad-hoc) ;; *) echo 'Usage: build-ios.sh unsigned|ad-hoc' >&2; exit 2 ;; esac
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'Requires macOS with the pinned Xcode and iPhoneOS SDK; no simulator fallback.' >&2
  exit 2
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
python3 scripts/check-public-tree.py
# Each invocation has a new directory. Never delete/overwrite earlier failures.
mkdir -p "$ROOT/local/ci"
RUN="$(mktemp -d "$ROOT/local/ci/$(date -u +%Y%m%dT%H%M%SZ)-${MODE}-XXXXXX")"
mkdir "$RUN/artifacts"
finish() {
  result=$?
  trap - EXIT
  python3 scripts/record-build.py finish --run "$RUN" --exit-code "$result" || result=1
  echo "Private build evidence: $RUN"
  exit "$result"
}
trap finish EXIT
python3 scripts/record-build.py begin --run "$RUN" --mode "$MODE" 2>&1 | tee "$RUN/preflight.log"
BUNDLE="${SKYRIM_BUNDLE_ID:-org.skyrimios.probe}"
COMMON=(-project ios/SkyrimProbe.xcodeproj -scheme SkyrimProbe -configuration Release
  -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath "$RUN/DerivedData"
  -jobs 2 -disableAutomaticPackageResolution ARCHS=arm64 ONLY_ACTIVE_ARCH=NO
  SUPPORTED_PLATFORMS=iphoneos SUPPORTS_MACCATALYST=NO IPHONEOS_DEPLOYMENT_TARGET=18.0
  "PRODUCT_BUNDLE_IDENTIFIER=$BUNDLE" DEBUG_INFORMATION_FORMAT=dwarf-with-dsym
  GCC_GENERATE_DEBUGGING_SYMBOLS=YES COMPILER_INDEX_STORE_ENABLE=NO)
if [[ "$MODE" == unsigned ]]; then
  xcodebuild "${COMMON[@]}" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= \
    build 2>&1 | tee "$RUN/build.log"
else
  python3 scripts/record-build.py profile --run "$RUN" 2>&1 | tee "$RUN/profile-validation.log"
  # Existing certificates are installed by Codemagic ios_signing. No provisioning
  # updates, new certificate creation, or App Store Connect upload is allowed here.
  xcodebuild "${COMMON[@]}" -xcconfig "$RUN/signing.xcconfig" \
    -archivePath "$RUN/SkyrimProbe.xcarchive" CODE_SIGNING_ALLOWED=YES archive 2>&1 | tee "$RUN/archive.log"
  xcodebuild -exportArchive -archivePath "$RUN/SkyrimProbe.xcarchive" \
    -exportOptionsPlist "$RUN/ExportOptions.plist" -exportPath "$RUN/export" \
    2>&1 | tee "$RUN/export.log"
fi
python3 scripts/record-build.py verify --run "$RUN" --mode "$MODE" 2>&1 | tee "$RUN/verification.log"
