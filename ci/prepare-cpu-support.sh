#!/usr/bin/env bash
# Copies only public headers and hand-authored CPU fixtures, never game inputs.
set -euo pipefail
test "$#" -eq 3
source_dir="$1"
simde_dir="$2"
probe_dir="$3"
harness_root="$(cd "$(dirname "$0")/.." && pwd)"
test -f "$source_dir/cpp/ppc_context.h"
test -f "$simde_dir/x86/avx.h"
test -d "$harness_root/ios-probe/cpu-fixture"
mkdir -p "$probe_dir/Support"
cp "$source_dir/cpp/ppc_context.h" "$probe_dir/Support/ppc_context.h"
mkdir -p "$probe_dir/Support/simde"
tar -C "$simde_dir" --exclude='.git' -cf - . | tar -C "$probe_dir/Support/simde" -xf -
# Git metadata is not needed by the compiler; copying headers alone also keeps
# the source-only fixture independent of the public checkout's nested .git.
cp -R "$harness_root/ios-probe/cpu-fixture" "$probe_dir/cpu-fixture"
