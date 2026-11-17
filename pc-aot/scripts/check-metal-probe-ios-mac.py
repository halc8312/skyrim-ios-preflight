#!/usr/bin/env python3
"""Manual unsigned phone build and simulator UI check. No device or game proof."""
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = "org.skyrimios.metalprobe"


def fresh_unsupported_record(path, prior_paths, source_revision):
    """Only new output from this exact app may satisfy the simulator run."""
    if path in prior_paths or path.is_symlink():
        return None
    try:
        record = json.loads(path.read_text())
    except (ValueError, OSError):
        return None
    if not isinstance(record, dict) or record.get("finished") is not True:
        return None
    comparison = record.get("comparison", {})
    if not isinstance(comparison, dict) or not isinstance(comparison.get("host_build"), dict):
        return None
    if comparison["host_build"].get("source_revision") != source_revision:
        return None
    if record.get("status") != "unsupported" or comparison.get("status") != "unsupported":
        raise RuntimeError("Simulator must persist an explicit unsupported result")
    for key in ("physical_iphone_verified", "gpu_execution_verified", "metal4_library_verified"):
        if comparison.get(key) is not False:
            raise RuntimeError("Simulator incorrectly claimed or omitted device/GPU evidence")
    if comparison.get("samples") != []:
        raise RuntimeError("Simulator ran unexpected comparison samples")
    return record


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise SystemExit("Requires an Apple ARM64 host; no substitute device evidence")
    parent = ROOT / "local/metal-probe-ios-checks"
    if parent.is_symlink():
        raise SystemExit("Refusing linked evidence parent")
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    spec = importlib.util.spec_from_file_location("probe_command_control", ROOT / "experiments/metal-compile/offline.py")
    control = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(control)
    deadline = time.monotonic() + 780
    report = {"schema": 1, "status": "failed", "scope": "unsigned iPhone build and simulator UI only",
              "physical_iphone_verified": False, "metal4_runtime_verified": False,
              "gameplay_improvement_verified": False, "commands": []}
    native_groups = (("phone-runtime", "local/tests/phone-runtime-*"), ("store", "local/metal-probe-store/run-*"))
    prior_native = {group: set(ROOT.glob(pattern)) for group, pattern in native_groups}

    def call(label, command, limit=60):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("Overall preview budget exhausted")
        limit = min(limit, remaining)
        outer_deadline = time.monotonic() + limit
        if label in ("build-device", "build-simulator"):
            if limit <= 15:
                raise RuntimeError("Insufficient cleanup budget for child build")
            # Set the shorter child deadline AFTER launch. Passing it in the
            # supervisor's own env would shorten that supervisor as well.
            command = [sys.executable, "-c",
                       "import os,sys;os.environ['SKYRIM_METAL_TEST_DEADLINE']=sys.argv[1];os.execv(sys.argv[2],sys.argv[2:])",
                       str(outer_deadline - 10), *command]
        event, stdout, stderr = control.run_command(command, label, out, report, os.environ.copy(), limit, deadline=outer_deadline)
        if event.get("returncode") != 0 or event.get("timed_out"):
            raise RuntimeError(label + " failed; inspect retained stdout/stderr")
        return stdout.strip()

    def build(simulator):
        base = ROOT / "local/metal-probe-ios"
        before = set(base.glob("run-*")) if base.exists() else set()
        args = [sys.executable, str(ROOT / "scripts/build-metal-probe-ios.py")]
        if simulator:
            args.append("--simulator")
        try:
            call("build-simulator" if simulator else "build-device", args, 420)
        finally:
            fresh = set(base.glob("run-*")) - before
            for run in fresh:
                for artifact in (run / "artifacts").glob("*"):
                    if artifact.is_file():
                        label = ("simulator-" if simulator else "device-") if artifact.suffix != ".ipa" else ""
                        shutil.copy2(artifact, out / (label + artifact.name))
        if len(fresh) != 1:
            raise RuntimeError("Expected one fresh build result")
        run = fresh.pop()
        result = json.loads((run / "build-result.json").read_text())
        expected = "unsigned_simulator_build_passed" if simulator else "unsigned_iphone_build_passed"
        if result["status"] != expected:
            raise RuntimeError("Unexpected build status")
        report["simulator_build" if simulator else "device_build"] = result
        return run / result["app_relative_path"]

    try:
        call("preflight", [sys.executable, str(ROOT / "scripts/check-public-tree.py")])
        for suite in ("test_metal_probe_ios_build.py", "test_metal_probe_ui_driver.py", "test_metal_phone_runtime.py", "test_metal_probe_store.py", "test_metal_compile_runtime.py"):
            call(suite.removesuffix(".py"), [sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tests"), "-p", suite, "-v"], 180)
        build(False)
        simulator_app = build(True)
        devices = json.loads(call("simulator-devices", ["xcrun", "simctl", "list", "devices", "available", "--json"]))["devices"]
        choices = [(runtime, device) for runtime, group in devices.items() if "iOS-26" in runtime
                   for device in group if device.get("isAvailable") and device["name"].startswith("iPhone")]
        if not choices:
            raise RuntimeError("No installed iOS 26 iPhone simulator; no runtime download requested")
        choices.sort(key=lambda item: (item[1]["name"] != "iPhone 17 Pro Max", item[0], item[1]["name"]))
        runtime, device = choices[0]
        udid = device["udid"]
        report["preview_device"] = {"runtime": runtime, "name": device["name"]}
        if device["state"] != "Booted":
            call("simulator-boot", ["xcrun", "simctl", "boot", udid])
        call("simulator-ready", ["xcrun", "simctl", "bootstatus", udid, "-b"], 480)
        # Local ad-hoc signing is only for this copied simulator preview bundle.
        preview = out / "preview" / "MetalProbe.app"
        shutil.copytree(simulator_app, preview)
        call("simulator-sign", ["codesign", "--force", "--sign", "-", "--timestamp=none", str(preview)])
        call("simulator-install", ["xcrun", "simctl", "install", udid, str(preview)])
        call("appearance", ["xcrun", "simctl", "ui", udid, "appearance", "light"])
        call("launch-ready", ["xcrun", "simctl", "launch", udid, BUNDLE])
        time.sleep(3)
        call("screen-ready", ["xcrun", "simctl", "io", udid, "screenshot", str(out / "ready.png")])
        call("terminate-ready", ["xcrun", "simctl", "terminate", udid, BUNDLE])
        container = Path(call("app-container", ["xcrun", "simctl", "get_app_container", udid, BUNDLE, "data"]))
        records = container / "Library/Application Support/MetalProbe"
        prior_paths = set(records.glob("*/*.json"))
        call("launch-smoke", ["xcrun", "simctl", "launch", udid, BUNDLE, "--probe-ui-smoke"])
        wait_until = min(deadline, time.monotonic() + 30)
        record = None
        while time.monotonic() < wait_until:
            for path in records.glob("*/*.json"):
                candidate = fresh_unsupported_record(path, prior_paths, report["simulator_build"]["source_revision"])
                if candidate:
                    record = candidate
                    shutil.copy2(path, out / "simulator-comparison.json")
                    break
            if record:
                break
            time.sleep(0.2)
        if not record:
            raise RuntimeError("No fresh unsupported record from this exact source revision")
        time.sleep(1)
        call("screen-result", ["xcrun", "simctl", "io", udid, "screenshot", str(out / "result.png")])
        call("terminate-result", ["xcrun", "simctl", "terminate", udid, BUNDLE])
        call("launch-restore", ["xcrun", "simctl", "launch", udid, BUNDLE])
        time.sleep(2)
        call("screen-restored", ["xcrun", "simctl", "io", udid, "screenshot", str(out / "restored.png")])
        call("terminate-restored", ["xcrun", "simctl", "terminate", udid, BUNDLE])
        call("launch-share", ["xcrun", "simctl", "launch", udid, BUNDLE, "--probe-ui-share"])
        time.sleep(3)
        call("screen-share", ["xcrun", "simctl", "io", udid, "screenshot", str(out / "share.png")])
        report["simulator_unsupported_record_verified"] = True
        report["screenshots_captured"] = ["ready.png", "result.png", "restored.png", "share.png"]
        report["visual_review_pending"] = True
        report["status"] = "passed"
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        # Preserve native test logs even when an earlier test fails.
        for group, pattern in native_groups:
            for i, run in enumerate(sorted(set(ROOT.glob(pattern)) - prior_native[group])):
                dest = out / "native-evidence" / f"{group}-{i}"
                for path in run.rglob("*"):
                    if not path.is_symlink() and path.is_file() and path.suffix in (".json", ".log", ".txt"):
                        target = dest / path.relative_to(run)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, target)
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        with zipfile.ZipFile(out / "metal-probe-ios-checks.zip", "x", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(out.rglob("*")):
                if path.is_file() and "preview" not in path.relative_to(out).parts and path.name != "metal-probe-ios-checks.zip":
                    archive.write(path, path.relative_to(out))
        print("Metal iPhone probe evidence: " + str(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
