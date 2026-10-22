#!/usr/bin/env bash
# Manual public-only retry: preserve the prior full run's successful gates.
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 0
harness="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/skyrim-resident-preflight.XXXXXX")"
export HOMEBREW_NO_AUTO_UPDATE=1
export RUSTUP_TOOLCHAIN=nightly-2026-10-11
mkdir -p "$harness/out"

# Reuse of run 16's earlier 309/24/15/signing evidence requires this same SDK.
# Refuse a changed latest image rather than silently claiming equivalence.
xcode_version="$(xcodebuild -version)"
printf '%s\n' "$xcode_version"
test "$xcode_version" = $'Xcode 26.6\nBuild version 17F113'
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
rustc --version
simde_commit=a532a12ca9bbdc5e6547eb602e6256b71a5377d4
simde="$work/simde"
git init -q "$simde"
git -C "$simde" remote add origin https://github.com/simd-everywhere/simde-no-tests.git
git -C "$simde" fetch --depth 1 origin "$simde_commit"
git -C "$simde" checkout -q --detach FETCH_HEAD
test "$(git -C "$simde" rev-parse HEAD)" = "$simde_commit"
python3 "$harness/ci/run-bounded-command.py" 900 \
  bash "$harness/ci/run-ios-resident-tests.sh" "$work" "$simde"
