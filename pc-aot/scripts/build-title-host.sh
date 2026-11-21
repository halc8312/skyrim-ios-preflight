#!/bin/bash
# SDK-built public host only. The owned game and generated code remain local.
set -euo pipefail
umask 077
[[ "$(uname -s)" == Darwin ]] || { echo 'Apple iPhoneOS SDK required' >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
CLASSIC_SKSE="${SKYRIM_IOS_CLASSIC_SKSE-0}"
DLL_PROBE="${SKYRIM_IOS_SKSE_DLL_PROBE-0}"
SETTINGS_PROBE="${SKYRIM_IOS_CLASSIC_SETTINGS_PROBE-0}"
case "$CLASSIC_SKSE" in
  0|1) ;;
  *) echo 'SKYRIM_IOS_CLASSIC_SKSE must be 0 or 1' >&2; exit 2 ;;
esac
case "$DLL_PROBE" in
  0|1) ;;
  *) echo 'SKYRIM_IOS_SKSE_DLL_PROBE must be 0 or 1' >&2; exit 2 ;;
esac
case "$SETTINGS_PROBE" in
  0|1) ;;
  *) echo 'SKYRIM_IOS_CLASSIC_SETTINGS_PROBE must be 0 or 1' >&2; exit 2 ;;
esac
python3 scripts/check-public-tree.py
mkdir -p local/title-host
RUN="$(mktemp -d "$ROOT/local/title-host/run-XXXXXX")"
mkdir -p "$RUN/artifacts"
exec > >(tee "$RUN/artifacts/build.log") 2>&1
echo "Public host build: $RUN"
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-version
MOD_DEPS_ROOT="${SKYRIM_IOS_DEPS_ROOT:-$ROOT/local/ios-dependencies-build15}"
bash scripts/fetch-mod-codecs.sh
env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT bash ios/dependencies/build-mod-archive-dependencies.sh macosx

# Copy only the upstream public tree. Its patches are checked before use.
python3 - "$ROOT" "$RUN" <<'PY'
from pathlib import Path
import hashlib, json, subprocess, sys, tarfile
root, run = map(Path, sys.argv[1:])
lock = json.loads((root/'source-lock.json').read_text())['sources'][0]
checkout = root/'vendor/skydream'
checkout.parent.mkdir(exist_ok=True)
if not checkout.exists():
    subprocess.run(['git','init',str(checkout)], check=True)
    subprocess.run(['git','-C',str(checkout),'remote','add','origin',lock['url']], check=True)
    subprocess.run(['git','-C',str(checkout),'fetch','--depth=1','origin',lock['revision']], check=True)
    subprocess.run(['git','-C',str(checkout),'checkout','--detach',lock['revision']], check=True)
head=subprocess.check_output(['git','-C',str(checkout),'rev-parse','HEAD'],text=True).strip()
if head != lock['revision']: raise SystemExit('Wrong upstream source revision')
source=run/'source'
source.mkdir()
archive=run/'source.tar'
subprocess.run(['git','-C',str(checkout),'archive','--format=tar','-o',str(archive),head],check=True)
with tarfile.open(archive) as stream: stream.extractall(source, filter='data')
subprocess.run(['git','init','-q',str(source)],check=True)
for item in lock['patches']:
    patch=root/item['path']
    if hashlib.sha256(patch.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Changed public patch: '+item['path'])
    subprocess.run(['git','-C',str(source),'apply','--check',str(patch)],check=True)
    subprocess.run(['git','-C',str(source),'apply',str(patch)],check=True)
PY

# These public fixtures execute the changed helpers/texture bridge on this ARM Mac.
# They do not contain the game or imply physical iPhone gameplay/performance.
(
  # The workflow exports an iPhone deployment target for the app. Native test
  # executables must instead link against macOS and run on the build machine.
  unset IPHONEOS_DEPLOYMENT_TARGET
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
  export SKYRIM_MOD_ARCHIVE_TEST_PREFIX="$MOD_DEPS_ROOT/mod-archive/macosx/prefix"
  # Run independent suites even after a failure, but never build/package a host
  # unless every required suite passed. Preserve all diagnostics in one Mac run.
  HOST_CHECK_STATUS=0
  run_validation() { "$@" || HOST_CHECK_STATUS=1; }
  run_validation python3 -m unittest discover -s tests -p 'test_build_title_guest.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_title_assembly.py' -v
  if [[ "$CLASSIC_SKSE" == 1 ]]; then
    run_validation env SKYRIM_SKYDREAM_SOURCE="$ROOT/vendor/skydream" SKYDREAM_SOURCE_DIR="$ROOT/vendor/skydream" \
      python3 -m unittest discover -s tests -p 'test_classic_*.py' -v
  fi
  if [[ "$DLL_PROBE" == 1 ]]; then
    run_validation env CC=clang PYTHONPATH="$ROOT/experiments/skse-plugin-vm" python3 -B -m unittest -v \
      test_classic_pex test_x86_sse_conversion \
      test_skse_plugin_vm.PluginInterpreterTests.test_memory_permissions_abi_and_budget
  fi
  run_validation python3 -m unittest discover -s tests -p 'test_title_signing.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_owned_data.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_ordered_stores.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_shadow_ranges.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_performance_metrics.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_performance_stalls.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_lock_metrics.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_lock_timing_analysis.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_d3d9_lock_timing.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_runtime_lock_timing.py' -v
  run_validation env PYTHONPATH=tests SKYRIM_TEST_SANITIZERS=thread python3 -m unittest \
    test_lock_metrics \
    test_d3d9_lock_timing.D3D9LockNativeTests.test_actual_lock_recursion_contention_and_hot_clock_budget \
    test_runtime_lock_timing -v
  run_validation python3 -m unittest discover -s tests -p 'test_save_write_probe.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_skse_copy_pair.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_autosave_preferences.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_game_language.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mod_plugins.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mod_requirements.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mod_features.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mod_archive.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mod_optimize.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mod_libarchive.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_diagnostic_upload.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_private_diagnostics.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_lifecycle_controller.py' -v
  run_validation env PYTHONPATH=tests SKYRIM_TEST_THREAD_SANITIZER=1 python3 -m unittest \
    test_lifecycle_controller.LifecycleTests.test_actual_controller_preserves_producers_and_resume_races -v
  run_validation python3 -m unittest discover -s tests -p 'test_present_timing.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_frame_timing_analysis.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_shader_lead_diagnostics.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_asset_timing.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_audio_submix.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_audio_backpressure.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_waiter_membership.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_xwma_buffer.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_xwma_voice_profile.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_constant_scratch.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_dds_layout.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_directory_index.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_additional_work.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_watchdog_layout.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_pipeline_cache.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_pipeline_cache_namespace.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_pipeline_locking.py' -v
  # Graphics coordination comparison: exercise both routes on Apple. The app
  # keeps the original route until its startup-only comparison option is ON.
  run_validation python3 -m unittest discover -s tests -p 'test_graphics_state_flights.py' -v
  run_validation env PYTHONPATH=tests SKYRIM_TEST_SANITIZERS=1 python3 -m unittest \
    test_graphics_state_flights.GraphicsStateFlightNativeTests.test_actual_state_lookup_publication_admission_and_failures -v
  run_validation env PYTHONPATH=tests SKYRIM_TEST_SANITIZERS=thread python3 -m unittest \
    test_graphics_state_flights.GraphicsStateFlightNativeTests.test_actual_state_lookup_publication_admission_and_failures -v
  run_validation python3 -m unittest discover -s tests -p 'test_prefetch_gate.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_state_cache_work.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_pending_work.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_resource_wait.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_device*.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_descriptor_capacity.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_mobile*.py' -v
  run_validation python3 -m unittest discover -s tests -p 'test_metal_toolchain.py' -v
  exit "$HOST_CHECK_STATUS"
)

bash scripts/build-dependencies.sh
# Keep this required gate, but collect independent app compiler failures in the
# same paid-minute run instead of stopping before its build can start.
METAL_CHECK_STATUS=0
bash ios/dependencies/check-metal-alias.sh "$RUN/metal-alias" || METAL_CHECK_STATUS=$?
# The fixture exercises actual coordination methods with authored Metal PSOs
# and GPU readback. It is not a full DXVK/MoltenVK or iPhone gameplay test.
if ! env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT \
  SKYRIM_GRAPHICS_METAL_OUTPUT="$RUN/graphics-state-metal" python3 -m unittest \
  discover -s tests -p 'test_graphics_state_flights_metal.py' -v \
  2>&1 | tee "$RUN/artifacts/graphics-state-metal.log"; then
  METAL_CHECK_STATUS=1
fi
python3 - "$RUN/graphics-state-metal" "$RUN/artifacts/graphics-state-metal-checks.zip" <<'PYGRAPHICS'
from pathlib import Path
import sys, zipfile
source, output = map(Path, sys.argv[1:])
with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(source.rglob('*')):
        if path.is_file() and not path.is_symlink() and path.suffix in ('.mm', '.c', '.log', '.json'):
            archive.write(path, path.relative_to(source).as_posix())
PYGRAPHICS
# Preserve the actual toolchain/compiler diagnostics on success and failure.
# Only authored fixtures and text logs are included, not CMake object files.
python3 - "$RUN/metal-alias" "$RUN/artifacts/metal-alias-checks.zip" <<'PYMETAL'
from pathlib import Path
import sys, zipfile
source, output = map(Path, sys.argv[1:])
with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(source.rglob('*')):
        if path.is_file() and not path.is_symlink() and 'build' not in path.relative_to(source).parts:
            archive.write(path, path.relative_to(source).as_posix())
PYMETAL
if (( METAL_CHECK_STATUS != 0 )); then
  echo "Metal checks failed ($METAL_CHECK_STATUS); building host only to collect independent errors." >&2
fi
# The runtime adapter verifies its own upstream anchors. Apply it to pristine
# upstream, then apply the CPU ABI patches to the staged runtime only.
python3 ios/runtime-port/prepare_runtime.py --source "$ROOT/vendor/skydream" --output "$RUN/overlay/runtime"
git -C "$RUN/overlay" init -q
for patch in patches/skydream/0001-auxiliary-carry.patch patches/skydream/0005-call-flags.patch patches/skydream/0006-inline-ordered-stores.patch; do
  git -C "$RUN/overlay" apply --include='runtime/*' "$ROOT/$patch"
done
python3 ios/private-abi/prepare_private_abi.py "$RUN/overlay"

cmake -S ios/runtime-port -B "$RUN/build" -G 'Unix Makefiles' \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 -DCMAKE_BUILD_TYPE=Release \
  -DSKYRIM_IOS_BUILD_APP=ON -DSKYRIM_IOS_BUNDLE_IDENTIFIER=org.skyrimios.pc2011 \
  "-DSKYRIM_IOS_CLASSIC_SKSE=$CLASSIC_SKSE" \
  "-DSKYRIM_IOS_SKSE_DLL_PROBE=$DLL_PROBE" \
  "-DSKYRIM_IOS_CLASSIC_SETTINGS_PROBE=$SETTINGS_PROBE" \
  "-DSKYDREAM_SOURCE_DIR=$RUN/source" \
  "-DSKYRIM_IOS_RUNTIME_OVERLAY_DIR=$RUN/overlay/runtime" \
  "-DSKYRIM_IOS_BINDING_SOURCES=$ROOT/ios/private-abi/pc_guest_host.c" \
  "-DSKYRIM_MOD_ARCHIVE_PREFIX=$RUN/without-mod-archive-libraries" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
# Link the actual iPhone app with the optional readers absent, then reuse the
# build directory for the distributable app with its readers and notices.
cmake --build "$RUN/build" --parallel 2 -- -k
xcrun nm "$RUN/build/SkyrimIOS.app/SkyrimIOS" > "$RUN/mod-fallback-symbols.txt"
if grep -q ' _skyrim_archive_list$' "$RUN/mod-fallback-symbols.txt"; then
  echo 'Unexpected optional archive reader in the fallback app.' >&2; exit 1
fi
echo 'MOD_ARCHIVE: iPhoneOS fallback app linked without libarchive or liblzma.'
cmake -S ios/runtime-port -B "$RUN/build" "-DSKYRIM_MOD_ARCHIVE_PREFIX=$MOD_DEPS_ROOT/prefix"
cmake --build "$RUN/build" --parallel 2 -- -k
xcrun nm "$RUN/build/SkyrimIOS.app/SkyrimIOS" > "$RUN/mod-full-symbols.txt"
grep -q ' _skyrim_archive_list$' "$RUN/mod-full-symbols.txt"
test -s "$RUN/build/SkyrimIOS.app/ModArchiveLicenses.txt"
MOD_SIMULATOR_STATUS=0
mod_simulator_run="$RUN/mod-simulator"
check_mod_simulator() {
  env -u IPHONEOS_DEPLOYMENT_TARGET -u SDKROOT bash scripts/check-mod-simulator.sh \
    "$1" "$MOD_DEPS_ROOT/prefix" "$MOD_DEPS_ROOT/mod-archive/iphonesimulator/prefix" "${2:-screens}"
}
if bash ios/dependencies/build-mod-archive-dependencies.sh iphonesimulator; then
  check_mod_simulator "$mod_simulator_run" || MOD_SIMULATOR_STATUS=$?
  if (( MOD_SIMULATOR_STATUS != 0 )); then
    echo "MOD simulator attempt exited with status $MOD_SIMULATOR_STATUS. Retrying the full check on a fresh simulator."
    if [[ -d "$mod_simulator_run/evidence" ]]; then
      ditto -c -k --keepParent "$mod_simulator_run/evidence" "$RUN/artifacts/mod-simulator-first-attempt.zip"
    fi
    mod_simulator_run="$RUN/mod-simulator-retry"
    MOD_SIMULATOR_STATUS=0
    check_mod_simulator "$mod_simulator_run" || MOD_SIMULATOR_STATUS=$?
  fi
else
  MOD_SIMULATOR_STATUS=1
fi
if [[ -d "$mod_simulator_run/evidence" ]]; then
  ditto -c -k --keepParent "$mod_simulator_run/evidence" "$RUN/artifacts/mod-simulator-checks.zip"
else
  echo "MOD simulator evidence directory is missing." >&2
  MOD_SIMULATOR_STATUS=1
fi
if (( MOD_SIMULATOR_STATUS != 0 )); then
  echo "Required MOD simulator checks failed; no host package will be published." >&2
  exit "$MOD_SIMULATOR_STATUS"
fi
if [[ "$DLL_PROBE" == 1 ]]; then
  dll_run="$RUN/mod-dll-simulator"
  dll_status=0
  check_mod_simulator "$dll_run" dll-probe || dll_status=$?
  if [[ -d "$dll_run/evidence" ]]; then
    ditto -c -k --keepParent "$dll_run/evidence" "$RUN/artifacts/mod-simulator-dll-checks.zip"
  fi
  (( dll_status == 0 )) || exit "$dll_status"
fi
if (( METAL_CHECK_STATUS != 0 )); then
  echo 'Required Metal checks failed; no host package or success result will be published.' >&2
  exit "$METAL_CHECK_STATUS"
fi
APP="$RUN/build/SkyrimIOS.app"
test -s "$APP/SkyrimIOS"
xcrun nm "$APP/SkyrimIOS" > "$RUN/app-symbols.txt"
if [[ "$DLL_PROBE" == 1 ]]; then
  grep -Eq ' [Tt] _skyrim_mod_dll_probe_controller$' "$RUN/app-symbols.txt"
elif grep -Eq ' [Tt] _skyrim_mod_dll_probe_controller$' "$RUN/app-symbols.txt"; then
  echo 'The DLL diagnostic was linked without being selected.' >&2; exit 1
fi
if [[ "$CLASSIC_SKSE" == 1 ]]; then
  for symbol in _pc_guest_bind_classic_skse _pc_guest_select_classic_skse _skse_register_natives _skse_system_run; do
    grep -Eq " [Tt] ${symbol}$" "$RUN/app-symbols.txt" || { echo "Missing Classic runtime symbol $symbol" >&2; exit 1; }
  done
elif grep -Eq ' [Tt] (_pc_guest_bind_classic_skse|_skse_register_natives)$' "$RUN/app-symbols.txt"; then
  echo 'A Classic runtime entry point is present in a host built without the feature.' >&2; exit 1
fi
if grep -Eq ' [Tt] (_skyrim_classic_probe_controller|_skyrim_classic_fixture_run|_skyrim_serial_i386_.*)$' "$RUN/app-symbols.txt"; then
  echo 'Unexpected lab fixture in the standard application.' >&2; exit 1
fi
if grep -q '_skyrim_gpu_' "$RUN/app-symbols.txt"; then
  echo 'Rejected lifecycle gate remains in the app; refusing package.' >&2; exit 1
fi
xcrun lipo "$APP/SkyrimIOS" -verify_arch arm64
xcrun vtool -show-build "$APP/SkyrimIOS" | tee "$RUN/artifacts/platform.txt"
grep -q 'platform IOS' "$RUN/artifacts/platform.txt"
xcrun otool -L "$APP/SkyrimIOS" > "$RUN/artifacts/linked-libraries.txt"
cp "$RUN/overlay/runtime/x86.h" "$RUN/artifacts/x86.h"
cp "$RUN/overlay/runtime/rt_table.h" "$RUN/artifacts/rt_table.h"
cp ios/private-abi/PcGuestABI.h "$RUN/artifacts/"
if [[ "$CLASSIC_SKSE" == 1 ]]; then
  cp ios/private-abi/PcGuestSkseABI.h "$RUN/artifacts/"
fi
ditto -c -k --keepParent "$APP" "$RUN/artifacts/SkyrimIOS-public-host.zip"
python3 - "$RUN/artifacts" "$APP/Info.plist" "$CLASSIC_SKSE" "$DLL_PROBE" <<'PY'
from pathlib import Path
import hashlib, json, os, plistlib, sys
out=Path(sys.argv[1]); app=out/'SkyrimIOS-public-host.zip'
info = plistlib.loads(Path(sys.argv[2]).read_bytes())
classic = sys.argv[3] == '1'
if info.get('CFBundleIdentifier') != 'org.skyrimios.pc2011' or info.get('CFBundleDisplayName') != 'SkyrimIOS':
    raise SystemExit('Unexpected standard iPhone product identity')
if ((classic and info.get('PcGuestClassicSkseExperiment') is not True)
        or (not classic and 'PcGuestClassicSkseExperiment' in info)):
    raise SystemExit('The app does not match the selected Classic guest ABI')
result = {
 'schema':1,'source_commit':os.environ.get('GITHUB_SHA') or os.environ.get('CM_COMMIT'),
 'artifact_sha256':hashlib.sha256(app.read_bytes()).hexdigest(),
 'target':'arm64-apple-ios','public_host_sdk_build':True,
 'private_guest_included':False,'game_executed':False,'dll_diagnostic':sys.argv[4] == '1',
 'physical_iphone_verified':False,'signed_for_installation':False
}
if classic:
    result['classic_skse_experiment'] = True
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
PY
echo 'Public host built; local private-framework assembly and signing still required.'
