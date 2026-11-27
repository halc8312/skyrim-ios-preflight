#!/bin/bash
# The simulator and this bundle contain only authored public fixtures.
set -euo pipefail
if [[ "$(uname -s)" != Darwin || $# -lt 3 || $# -gt 4 ]]; then
    echo "Usage on a Mac with an iOS 18+ simulator: $0 OUTPUT SDL_PREFIX SIMULATOR_ARCHIVE_PREFIX [screens|nexus-live|dll-probe]" >&2
    exit 2
fi
mode="${4:-screens}"
allowance=900
case "$mode" in
    screens)
        method=testActualSettingsModScreens
        # The full flow reached the generated-output import at 25 minutes on CI.
        # Allow that final profile check to finish. Individual waits stay bounded.
        allowance=2100
        ;;
    nexus-live) method=testLiveNexusListing ;;
    dll-probe) method=testInstalledModDllDiagnostics; export SKYRIM_IOS_SKSE_DLL_PROBE=1 ;;
    *) echo "Unknown simulator test mode: $mode" >&2; exit 2 ;;
esac
tests=("-only-testing:ModScreensUITests/ModScreensTests/$method")
if [[ "$mode" == screens ]]; then
    tests+=("-only-testing:ModScreensUITests/ModScreensTests/testNativeMenuControls")
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
    "${tests[@]}" \
    -default-test-execution-time-allowance "$allowance" -maximum-test-execution-time-allowance "$allowance" \
    test || status=$?
echo "xcodebuild test exited with status $status."
if [[ -d "$result" ]]; then
    xcrun xcresulttool get test-results summary --path "$result" > "$EVIDENCE/summary.json"
    xcrun xcresulttool export attachments --path "$result" --output-path "$RUN/attachments"
    python3 - "$RUN/attachments" "$EVIDENCE/screenshots" "$status" "$mode" <<'PY'
import json
import os
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
required = {"settings","mod-hub-japanese","empty-hub","save-check","zip-imported","folder-imported","fomod-options","fomod-full-text",
            "profile-renamed-selected","nexus-browser","export-share-sheet","conflict-summary",
            "conflict-files","separate-profile-saves","imported-profile","picker-AuthoredZip.zip",
            "picker-AuthoredFolder","picker-AuthoredInstaller.zip","picker-Trial Two.skyrimios-modlist.json"}
live = sys.argv[4] == "nexus-live"
if live:
    required = {"nexus-live-listing"}
elif sys.argv[4] == "dll-probe":
    required = {"installed-dll-diagnosis"}
elif os.environ.get("SKYRIM_IOS_SKSE_DLL_PROBE") == "1":
    required.add("installed-dll-diagnosis")
if sys.argv[4] == "screens" and os.environ.get("SKYRIM_IOS_CLASSIC_SKSE") == "1":
    required.add("additional-save-check")
if sys.argv[4] == "screens":
    required.update({"settings-language-arabic", "native-menu-compact"})
captured = set()
# Retain the last visible screen when a system picker or a table action fails.
labels = required | {"failure-screen", "row-unavailable"}
if live:
    labels.update("nexus-live-" + result for result in
                  ("challenge", "site-error", "interstitial", "network-error", "navigation-error", "other-url", "unconfirmed"))
for entry in entries:
    path = source / Path(entry["exportedFileName"]).name
    if not path.is_file():
        continue
    label = str(entry.get("suggestedHumanReadableName",""))
    # Xcode inserts the attachment number and UUID before the name's last
    # extension. A screenshot named for a ZIP still has that ZIP suffix here.
    name = re.sub(r"_\d+_[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}(?=\.[^.]+$|$)",
                  "", label, flags=re.IGNORECASE)
    if live and re.fullmatch(r"nexus-live-result(?:\.txt)?", name, re.IGNORECASE):
        shutil.copyfile(path, target.parent/"nexus-live-result.txt")
        continue
    if path.suffix.lower() not in {".png",".jpeg",".jpg"}:
        continue
    name = re.sub(r"\.(?:png|jpe?g)$", "", name, flags=re.IGNORECASE)
    if name in labels:
        shutil.copyfile(path, target/(name+path.suffix.lower()))
        captured.add(name)
if sys.argv[3] == "0" and not required.issubset(captured):
    raise SystemExit(f"Missing test screenshots: {sorted(required-captured)}")
if live and sys.argv[3] == "0":
    summary = json.loads((target.parent/"summary.json").read_text())
    report = (target.parent/"nexus-live-result.txt").read_text(encoding="utf-8-sig")
    if (summary.get("result") != "Passed" or summary.get("passedTests") != 1 or
        summary.get("failedTests") != 0 or not report.startswith("result=listing\n")):
        raise SystemExit("Live Nexus test needs one passing test and an observed listing.")
print(f"Retained {len(captured & required)} required simulator screenshots.")
PY
fi
if (( status != 0 )); then
    echo "MOD simulator exited with status $status."
    if [[ -s "$EVIDENCE/summary.json" ]]; then
        cat "$EVIDENCE/summary.json"
    fi
    exit "$status"
fi
if [[ "$mode" == nexus-live ]]; then
    test -s "$EVIDENCE/nexus-live-result.txt"
    echo "MOD_SIMULATOR: live Nexus Skyrim listing visible in the shipping browser."
    exit 0
fi
container="$(xcrun simctl get_app_container "$UDID" org.skyrimios.tests.modscreens data)"
if [[ "$mode" == dll-probe ]]; then
    python3 "$PREP" verify-dll "$container" "$RUN/work/Fixtures" "$EVIDENCE/summary.json"
    echo "MOD_SIMULATOR: the shipping MOD details opened the isolated DLL diagnostic."
    exit 0
fi
python3 "$PREP" verify "$container" "$RUN/work/Fixtures" "$EVIDENCE/summary.json"
python3 - "$container/Documents/mod-menu-requests.txt" <<'PY'
from pathlib import Path
import sys
events = Path(sys.argv[1]).read_text().splitlines()
if events[:6] != ["metrics 0", "lifecycle 0", "lifecycle 1", "metrics 1", "metrics 0", "lifecycle 0"]:
    raise SystemExit("Unexpected pause/resume requests while opening the MOD hub, returning to Settings, and reopening it")
print("MOD_SIMULATOR: the MOD hub requested pause, resume, and pause through the existing lifecycle.")
PY
echo "MOD_SIMULATOR: real UIKit settings/screens and authored storage checks passed."
