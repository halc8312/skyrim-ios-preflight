#!/bin/bash
# The simulator and this bundle contain only authored public fixtures.
set -euo pipefail
if [[ "$(uname -s)" != Darwin || $# != 3 ]]; then
    echo "Usage on a Mac with an iOS 18+ simulator: $0 OUTPUT SDL_PREFIX SIMULATOR_ARCHIVE_PREFIX" >&2
    exit 2
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREP="$ROOT/tests/mod-simulator/prepare.py"
mkdir -p "$1"
RUN="$(cd "$1" && pwd)"
if [[ -e "$RUN/work" || -e "$RUN/ModScreens.xcresult" ]]; then
    echo "A previous simulator result exists in this directory" >&2
    exit 1
fi
EVIDENCE="$RUN/evidence"
mkdir -p "$EVIDENCE"
exec > >(tee "$EVIDENCE/simulator.log") 2>&1
unset SDKROOT IPHONEOS_DEPLOYMENT_TARGET
xcodebuild -version
xcrun --sdk iphonesimulator --show-sdk-version

python3 "$PREP" project "$RUN/work" "$2" "$3"
plutil -lint "$RUN/work/HostInfo.plist" "$RUN/work/ModScreens.xcodeproj/project.pbxproj"
UDID="$(python3 "$PREP" device)"
[[ "$UDID" =~ ^[0-9A-Fa-f-]{36}$ ]] || { echo "Invalid dedicated simulator identifier" >&2; exit 1; }
trap 'xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true' EXIT
xcrun simctl boot "$UDID"
xcrun simctl bootstatus "$UDID" -b
# Disabling parallel testing keeps installation, file verification and the UI
# test on the same freshly-created simulator. Never fall back to a real phone.
result="$RUN/ModScreens.xcresult"
status=0
xcodebuild -project "$RUN/work/ModScreens.xcodeproj" -scheme ModScreens \
    -destination "platform=iOS Simulator,id=$UDID" -destination-timeout 90 \
    -derivedDataPath "$RUN/DerivedData" -resultBundlePath "$result" \
    -parallel-testing-enabled NO -test-timeouts-enabled YES \
    -default-test-execution-time-allowance 600 -maximum-test-execution-time-allowance 600 \
    test || status=$?
echo "xcodebuild test exited with status $status."
if [[ -d "$result" ]]; then
    xcrun xcresulttool get test-results summary --path "$result" > "$EVIDENCE/summary.json"
    xcrun xcresulttool export attachments --path "$result" --output-path "$RUN/attachments"
    python3 - "$RUN/attachments" "$EVIDENCE/screenshots" "$status" <<'PY'
import json
import re
import shutil
import sys
from pathlib import Path

source, target = map(Path, sys.argv[1:3])
target.mkdir()
manifest = json.loads((source/"manifest.json").read_text())
entries = []
def walk(value):
    if isinstance(value, dict):
        if "exportedFileName" in value:
            entries.append(value)
        for v in value.values(): walk(v)
    elif isinstance(value, list):
        for v in value: walk(v)
walk(manifest)
required = {"settings","empty-hub","save-check","zip-imported","folder-imported","fomod-options",
            "profile-renamed-selected","nexus-browser","export-share-sheet","conflict-summary",
            "conflict-files","separate-profile-saves","imported-profile","picker-AuthoredZip.zip",
            "picker-AuthoredFolder","picker-AuthoredInstaller.zip","picker-Trial Two.skyrimios-modlist.json"}
captured = set()
# Retain the last visible screen when a system picker or a table action fails.
labels = required | {"failure-screen", "row-unavailable"}
for entry in entries:
    path = source / Path(entry["exportedFileName"]).name
    if not path.is_file() or path.suffix.lower() not in {".png",".jpeg",".jpg"}:
        continue
    label = str(entry.get("suggestedHumanReadableName",""))
    # Xcode inserts the attachment number and UUID before the name's last
    # extension. A screenshot named for a ZIP still has that ZIP suffix here.
    name = re.sub(r"_\d+_[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}(?=\.[^.]+$)",
                  "", label, flags=re.IGNORECASE)
    name = re.sub(r"\.(?:png|jpe?g)$", "", name, flags=re.IGNORECASE)
    if name in labels:
        shutil.copyfile(path, target/(name+path.suffix.lower()))
        captured.add(name)
if sys.argv[3] == "0" and not required.issubset(captured):
    raise SystemExit(f"Missing test screenshots: {sorted(required-captured)}")
print(f"Retained {len(captured & required)} required simulator screenshots.")
PY
fi
[[ "$status" -eq 0 ]] || exit "$status"
container="$(xcrun simctl get_app_container "$UDID" org.skyrimios.tests.modscreens data)"
python3 "$PREP" verify "$container" "$RUN/work/Fixtures" "$EVIDENCE/summary.json"
echo "MOD_SIMULATOR: real UIKit settings/screens and authored storage checks passed."
