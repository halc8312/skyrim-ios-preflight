#!/bin/bash
# SDK-built public host only. The owned game and generated code remain local.
set -euo pipefail
umask 077
[[ "$(uname -s)" == Darwin ]] || { echo 'Apple iPhoneOS SDK required' >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
python3 scripts/check-public-tree.py
mkdir -p local/title-host
RUN="$(mktemp -d "$ROOT/local/title-host/run-XXXXXX")"
mkdir -p "$RUN/artifacts"
exec > >(tee "$RUN/artifacts/build.log") 2>&1
echo "Public host build: $RUN"
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-version

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

bash ios/dependencies/build-dependencies.sh
# The runtime adapter verifies its own upstream anchors. Apply it to pristine
# upstream, then apply the CPU ABI patches to the staged runtime only.
python3 ios/runtime-port/prepare_runtime.py --source "$ROOT/vendor/skydream" --output "$RUN/overlay/runtime"
git -C "$RUN/overlay" init -q
for patch in patches/skydream/0001-auxiliary-carry.patch patches/skydream/0005-call-flags.patch; do
  git -C "$RUN/overlay" apply --include='runtime/*' "$ROOT/$patch"
done
python3 ios/private-abi/prepare_private_abi.py "$RUN/overlay"

cmake -S ios/runtime-port -B "$RUN/build" -G 'Unix Makefiles' \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 -DCMAKE_BUILD_TYPE=Release \
  -DSKYRIM_IOS_BUILD_APP=ON -DSKYRIM_IOS_BUNDLE_IDENTIFIER=org.skyrimios.pc2011 \
  "-DSKYDREAM_SOURCE_DIR=$RUN/source" \
  "-DSKYRIM_IOS_RUNTIME_OVERLAY_DIR=$RUN/overlay/runtime" \
  "-DSKYRIM_IOS_BINDING_SOURCES=$ROOT/ios/private-abi/pc_guest_host.c" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build "$RUN/build" --parallel 2
APP="$RUN/build/SkyrimIOS.app"
test -s "$APP/SkyrimIOS"
xcrun lipo -verify_arch arm64 "$APP/SkyrimIOS"
xcrun vtool -show-build "$APP/SkyrimIOS" | tee "$RUN/artifacts/platform.txt"
grep -q 'platform IOS' "$RUN/artifacts/platform.txt"
xcrun otool -L "$APP/SkyrimIOS" > "$RUN/artifacts/linked-libraries.txt"
cp "$RUN/overlay/runtime/x86.h" "$RUN/artifacts/x86.h"
cp "$RUN/overlay/runtime/rt_table.h" "$RUN/artifacts/rt_table.h"
cp ios/private-abi/PcGuestABI.h "$RUN/artifacts/"
ditto -c -k --keepParent "$APP" "$RUN/artifacts/SkyrimIOS-public-host.zip"
python3 - "$RUN/artifacts" <<'PY'
from pathlib import Path
import hashlib, json, os, sys
out=Path(sys.argv[1]); app=out/'SkyrimIOS-public-host.zip'
(out/'result.json').write_text(json.dumps({
 'schema':1,'source_commit':os.environ.get('GITHUB_SHA') or os.environ.get('CM_COMMIT'),
 'artifact_sha256':hashlib.sha256(app.read_bytes()).hexdigest(),
 'target':'arm64-apple-ios','public_host_sdk_build':True,
 'private_guest_included':False,'game_executed':False,
 'physical_iphone_verified':False,'signed_for_installation':False
},indent=2)+'\n')
PY
echo 'Public host built; local private-framework assembly and signing still required.'
