"""Build and exercise Classic Lab's actual sample UI in a new iPhone simulator."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import plistlib
import runpy
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PORT = ROOT / "ios/runtime-port"
sys.path.insert(0, str(ROOT / "experiments/classic-plugin"))
try:
    from run import stage
    from run_serialization import checked_file, expected_bytes
finally:
    sys.path.pop(0)

BUNDLE = "org.skyrimios.tests.classic-probe-ui"


def invoke(*args, timeout=120):
    subprocess.run([str(arg) for arg in args], check=True, timeout=timeout)


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True, timeout=60).strip()


def verify(documents, input_bytes):
    state = json.loads((documents / "ui-check.json").read_bytes())
    if state.get("outcome") != "passed":
        raise ValueError(f"Simulator UI did not complete: {state}")
    if checked_file(documents / "windows.skse", 16) != input_bytes:
        raise ValueError("the selected test input changed")
    result = checked_file(documents / "translated.skse", 21)
    report = json.loads((documents / "classic-lab-result.json").read_bytes())
    expected = {"schema": 1, "platform": "ios_simulator", "bundle_id": BUNDLE, "build": "1",
                "input_value": 16, "first_save_after_revert": 5, "output_value": 21,
                "records_read": 1, "callback_mask": 7, "input_sha256": hashlib.sha256(input_bytes).hexdigest(),
                "output_sha256": hashlib.sha256(result).hexdigest()}
    for key, value in expected.items():
        if type(report.get(key)) is not type(value) or report[key] != value:
            raise ValueError(f"unexpected simulator result field: {key}")
    if uuid.UUID(report["run_id"]).version != 4 or not report["os_version"]:
        raise ValueError("missing run or simulator OS identity")
    completed = datetime.datetime.fromisoformat(report["completed_at"].replace("Z", "+00:00"))
    if completed.tzinfo is None or set(report) != set(expected) | {"run_id", "completed_at", "os_version"}:
        raise ValueError("unexpected report structure")


def build(work, skydream, sample):
    headers = work / "public-headers"
    headers.mkdir()
    stage(skydream, headers)
    sdk = output("xcrun", "--sdk", "iphonesimulator", "--show-sdk-path")
    arch = platform.machine()
    tools = work / "build"
    invoke("cmake", "-S", HERE, "-B", tools, "-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={sdk}",
           f"-DCMAKE_OSX_ARCHITECTURES={arch}", "-DCMAKE_OSX_DEPLOYMENT_TARGET=18.0",
           "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY", f"-DRUNTIME_HEADERS={headers/'runtime'}")
    invoke("cmake", "--build", tools, "--parallel", "2")
    lib, = tools.rglob("libclassic_probe_ui_core.a")
    app = work / "ClassicProbeUITest.app"
    app.mkdir()
    name = "ClassicProbeUITest"
    info = {"CFBundleIdentifier": BUNDLE, "CFBundleName": name, "CFBundleExecutable": name,
            "CFBundleInfoDictionaryVersion": "6.0", "CFBundlePackageType": "APPL", "CFBundleVersion": "1",
            "CFBundleShortVersionString": "1.0", "CFBundleSupportedPlatforms": ["iPhoneSimulator"],
            "MinimumOSVersion": "18.0", "UIDeviceFamily": [1], "LSRequiresIPhoneOS": True,
            "LSSupportsOpeningDocumentsInPlace": True, "UILaunchScreen": {},
            "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False, "UISceneConfigurations": {
                "UIWindowSceneSessionRoleApplication": [{"UISceneConfigurationName": "Test", "UISceneDelegateClassName": "ProbeUIScene"}]}}}
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    (app / "windows.skse").write_bytes(sample)
    invoke("xcrun", "--sdk", "iphonesimulator", "clang", "-target", f"{arch}-apple-ios18.0-simulator",
           "-isysroot", sdk, "-fobjc-arc", "-fmodules", "-Wall", "-Wextra", "-Werror", "-I", PORT,
           HERE / "host.m", PORT / "classic_probe_ui.m", lib, "-framework", "UIKit", "-framework", "Foundation",
           "-framework", "UniformTypeIdentifiers", "-o", app / name)
    invoke("codesign", "--force", "--sign", "-", app)
    return app


def run(skydream, destination, windows_sample):
    if sys.platform != "darwin":
        raise SystemExit("Xcode and an iOS 18+ Simulator on macOS are required")
    destination.mkdir(parents=True, exist_ok=False)
    sample = checked_file(windows_sample, 16) if windows_sample else expected_bytes(16)
    with tempfile.TemporaryDirectory(prefix="classic-ui-") as work:
        app = build(Path(work), skydream, sample)
        create_simulator = runpy.run_path(str(ROOT / "tests/mod-simulator/prepare.py"))["choose_device"]
        simulator = str(uuid.UUID(create_simulator()))
        try:
            simctl = ["xcrun", "simctl"]
            invoke(*simctl, "boot", simulator)
            invoke(*simctl, "bootstatus", simulator, "-b", timeout=360)
            invoke(*simctl, "install", simulator, app)
            invoke(*simctl, "launch", simulator, BUNDLE)
            documents = Path(output(*simctl, "get_app_container", simulator, BUNDLE, "data")) / "Documents"
            end = time.monotonic() + 120
            while not (documents / "ui-check.json").exists():
                if time.monotonic() >= end:
                    invoke(*simctl, "io", simulator, "screenshot", destination / "last-screen.png")
                    raise TimeoutError("Classic Lab UI test did not release a result")
                time.sleep(0.25)
            invoke(*simctl, "io", simulator, "screenshot", destination / "last-screen.png")
            verify(documents, sample)
            for name in ("classic-lab-result.json", "translated.skse"):
                shutil.copyfile(documents / name, destination / name)
        finally:
            subprocess.run(["xcrun", "simctl", "shutdown", simulator], timeout=60, check=False)
            invoke("xcrun", "simctl", "delete", simulator)
    print("Verified the actual UIKit controller and AOT sample on iOS Simulator: Windows-format 16, reset save 5, output 21.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skydream", required=True, type=Path, help="pinned public Skydream git checkout")
    parser.add_argument("--output", type=Path, required=True, help="new directory for simulator evidence")
    parser.add_argument("--windows-sample", type=Path, help="optional exact 83-byte output of the authored Windows DLL")
    args = parser.parse_args()
    run(args.skydream.resolve(), args.output.resolve(), args.windows_sample)
