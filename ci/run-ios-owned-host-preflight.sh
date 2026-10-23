#!/usr/bin/env bash
# Manual source-only host integration. No title input, credentials or billing.
set -euo pipefail
case "${SKYRT_OWNED_HOST_SCOPE:-}" in
  '') owned_host_stage_seconds=1500 ;;
  normal-play-evidence) owned_host_stage_seconds=450 ;;
  *) printf '%s\n' 'Unknown public owned-host scope refused.' >&2; exit 2 ;;
esac
readonly owned_host_stage_seconds
test "$(uname -s)" = Darwin
test "$#" -eq 0
harness="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/skyrim-owned-host-preflight.XXXXXX")"
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
export RUSTUP_TOOLCHAIN=nightly-2026-10-11
export IPHONEOS_DEPLOYMENT_TARGET=26.0
mkdir -p "$harness/out"
# Reuse earlier source-only SDK evidence only under the same Apple toolchain.
version="$(xcodebuild -version)"
printf '%s\n' "$version"
test "$version" = $'Xcode 26.6\nBuild version 17F113'
test "$(xcrun --sdk iphoneos --show-sdk-version)" = 26.5
test "$(xcrun --sdk iphonesimulator --show-sdk-version)" = 26.5
if ! command -v xcodegen >/dev/null 2>&1; then
  brew install xcodegen
fi
if ! command -v rustup >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location \
    https://sh.rustup.rs -o "$work/rustup-init.sh"
  sh "$work/rustup-init.sh" -y --profile minimal --default-toolchain "$RUSTUP_TOOLCHAIN" --no-modify-path
  export PATH="$HOME/.cargo/bin:$PATH"
fi
rustup toolchain install "$RUSTUP_TOOLCHAIN" --profile minimal
rustup target add --toolchain "$RUSTUP_TOOLCHAIN" aarch64-apple-ios aarch64-apple-ios-sim
rustc --version
simde_commit=a532a12ca9bbdc5e6547eb602e6256b71a5377d4
simde="$work/simde"
git init -q "$simde"
git -C "$simde" remote add origin https://github.com/simd-everywhere/simde-no-tests.git
git -C "$simde" fetch --depth 1 origin "$simde_commit"
git -C "$simde" checkout -q --detach FETCH_HEAD
test "$(git -C "$simde" rev-parse HEAD)" = "$simde_commit"
python3 "$harness/ci/run-bounded-command.py" "$owned_host_stage_seconds" \
  bash "$harness/ci/run-ios-owned-host-tests.sh" "$work" "$simde"
