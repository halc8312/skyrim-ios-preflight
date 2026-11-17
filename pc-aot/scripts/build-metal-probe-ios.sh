#!/bin/bash
# Separate authored UIKit probe. No game files, profile, signing or installation.
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/build-metal-probe-ios.py" "$@"
