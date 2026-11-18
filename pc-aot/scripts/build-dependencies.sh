#!/bin/bash
# Preserve the renderer recipe and its build-15 shader cache identity.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bash "$root/ios/dependencies/build-dependencies.sh"
bash "$root/ios/dependencies/build-mod-archive-dependencies.sh" iphoneos
