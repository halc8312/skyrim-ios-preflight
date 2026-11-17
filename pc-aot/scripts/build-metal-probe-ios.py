#!/usr/bin/env python3
"""Build an unsigned authored Metal probe; SDK success is not device evidence."""
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import struct
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APP_NAME = "MetalProbe"
DEFAULT_BUNDLE = "org.skyrimios.metalprobe"
# The second name is the pre-existing, separate diagnostic app, never SkyrimIOS.
ALLOWED_BUNDLES = {DEFAULT_BUNDLE, "org.skyrimios.probe"}
INPUTS = (
    "experiments/metal-compile/runtime.mm",
    "experiments/metal-compile/runtime_host.h",
    "experiments/metal-compile/fixture.metal",
    "experiments/metal-compile/ios/main.mm",
    "experiments/metal-compile/ios/ProbeRunStore.h",
    "experiments/metal-compile/ios/ProbeRunStore.mm",
    "experiments/metal-compile/ios/Info.plist",
    "experiments/metal-compile/offline.py",
    "scripts/build-metal-probe-ios.sh",
    "scripts/build-metal-probe-ios.py",
)
APP_FILES = {APP_NAME, "Info.plist", "fixture.metal", "BuildInfo.json"}
FALSE_EVIDENCE = (
    "signed_for_device", "installed", "physical_iphone_verified",
    "gpu_execution_verified", "metal4_runtime_verified", "gameplay_improvement_verified",
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_bundle(value):
    if value not in ALLOWED_BUNDLES:
        raise ValueError("bundle must be one of the two isolated diagnostic app identifiers")
    return value


def fresh_run(root):
    # Reject links before resolving; output can never escape this ignored subtree.
    base = root / "local" / "metal-probe-ios"
    for path in (root / "local", base):
        if path.is_symlink():
            raise ValueError("build output parent must not be a symlink")
    base.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="run-", dir=base))


def validate_macho(data, simulator=False):
    if len(data) < 32:
        raise ValueError("truncated Mach-O")
    magic, cpu, subtype, kind, count, size, _, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, subtype & 0xFFFFFF, kind) != (0xFEEDFACF, 0x0100000C, 0, 2):
        raise ValueError("expected thin ARM64 executable, not arm64e or universal")
    end = 32 + size
    if end > len(data) or count > 4096:
        raise ValueError("invalid load-command bounds")
    offset, build, uuid = 32, None, None
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError("truncated load command")
        cmd, length = struct.unpack_from("<2I", data, offset)
        if length < 8 or length % 8 or offset + length > end:
            raise ValueError("invalid load command size")
        if cmd == 0x32:
            if length < 24 or build is not None:
                raise ValueError("missing/duplicate build-version command")
            build = struct.unpack_from("<4I", data, offset + 8)
            if length != 24 + build[3] * 8:
                raise ValueError("invalid build-version tools")
        elif cmd == 0x1B:
            if length != 24 or uuid is not None:
                raise ValueError("invalid/duplicate UUID")
            uuid = data[offset + 8:offset + 24].hex()
        elif cmd == 0x1D:
            raise ValueError("expected unsigned executable without embedded signature")
        elif cmd in (0x24, 0x25, 0x2F, 0x30):
            raise ValueError("unexpected legacy deployment-target command")
        offset += length
    if offset != end or build is None or uuid is None:
        raise ValueError("incomplete executable load commands")
    expected_platform = 7 if simulator else 2
    if build[0] != expected_platform or build[1] != 26 << 16 or build[2] < 26 << 16:
        raise ValueError("expected explicit iOS 26 target and matching device/simulator platform")
    return {"architecture": "arm64", "platform": build[0], "minimum_os": "26.0",
            "sdk_encoded": build[2], "uuid": uuid, "signed_load_command": False}


def make_plist(template, bundle, simulator):
    validate_bundle(bundle)
    value = plistlib.loads(template)
    value["CFBundleIdentifier"] = bundle
    value["CFBundleSupportedPlatforms"] = ["iPhoneSimulator" if simulator else "iPhoneOS"]
    return plistlib.dumps(value, sort_keys=True)


def validate_app(app, bundle, simulator, fixture_hash):
    if app.is_symlink() or not app.is_dir():
        raise ValueError("app must be an owned directory")
    found = set()
    for path in app.rglob("*"):
        if path.is_symlink() or not path.is_file():
            raise ValueError("unexpected directory or symlink in authored app")
        found.add(path.relative_to(app).as_posix())
    if found != APP_FILES:
        raise ValueError("app file set differs from authored four-file bundle")
    value = plistlib.loads((app / "Info.plist").read_bytes())
    required = {"CFBundleIdentifier": validate_bundle(bundle), "CFBundleExecutable": APP_NAME,
                "CFBundlePackageType": "APPL", "MinimumOSVersion": "26.0",
                "CFBundleSupportedPlatforms": ["iPhoneSimulator" if simulator else "iPhoneOS"],
                "UIDeviceFamily": [1],
                "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False}}
    for key, expected in required.items():
        if value.get(key) != expected:
            raise ValueError("app metadata mismatch: " + key)
    if sha256(app / "fixture.metal") != fixture_hash:
        raise ValueError("authored source resource changed")
    build_info = json.loads((app / "BuildInfo.json").read_text(encoding="utf-8"))
    if build_info.get("bundle_id") != bundle or build_info.get("simulator") is not simulator:
        raise ValueError("build provenance mismatch")
    if any(build_info.get(key) is not False for key in FALSE_EVIDENCE):
        raise ValueError("build metadata must not claim signing/device/runtime success")
    return validate_macho((app / APP_NAME).read_bytes(), simulator)


def package_app(app, destination, simulator):
    if {path.name for path in app.iterdir()} != APP_FILES:
        raise ValueError("app must contain exactly the authored bundle files")
    with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(app.iterdir()):
            if path.is_symlink() or not path.is_file() or path.name not in APP_FILES:
                raise ValueError("unexpected app member")
            prefix = APP_NAME + ".app" if simulator else "Payload/" + APP_NAME + ".app"
            member = zipfile.ZipInfo.from_file(path, prefix + "/" + path.name)
            member.create_system = 3
            member.external_attr = (0o100755 if path.name == APP_NAME else 0o100644) << 16
            member.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(member, path.read_bytes())


def archive_evidence(run):
    destination = run / "artifacts" / "metal-probe-build-evidence.zip"
    # Only this run's owned records and source snapshot. No IPA nesting or private inputs.
    with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(run.rglob("*")):
            relative = path.relative_to(run)
            if path.is_symlink():
                raise ValueError("symlink in build evidence")
            if not path.is_file() or relative.parts[0] in {"artifacts", "Payload", "objects"}:
                continue
            if path.suffix not in {".json", ".txt", ".log", ".mm", ".h", ".metal", ".plist", ".py", ".sh"}:
                raise ValueError("unexpected evidence file")
            archive.write(path, relative.as_posix())


def execute(root, simulator=False, bundle=DEFAULT_BUNDLE, command=None):
    validate_bundle(bundle)
    if platform.system() != "Darwin":
        raise ValueError("requires macOS with iPhone SDK; no host or simulator fallback")
    run = fresh_run(root)
    (run / "artifacts").mkdir()
    report = {"schema": 1, "scope": "authored UIKit probe build only", "status": "failed",
              "bundle_id": bundle, "simulator": simulator, "commands": [], "inputs": {},
              "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              **{key: False for key in FALSE_EVIDENCE}}
    deadline = time.monotonic() + 600
    environment = {key: os.environ[key] for key in
                   ("PATH", "HOME", "TMPDIR", "DEVELOPER_DIR", "LANG", "LC_ALL", "SKYRIM_METAL_TEST_DEADLINE")
                   if key in os.environ}

    def call(args, label, timeout=30):
        event, stdout, _ = command(args, label, run, report, environment, timeout, deadline=deadline)
        if event.get("timed_out") or event.get("returncode") != 0:
            raise ValueError(label + " failed; original logs preserved")
        return stdout.strip()

    try:
        if command is None:
            spec = importlib.util.spec_from_file_location("metal_probe_build_control", root / "experiments/metal-compile/offline.py")
            control = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(control)
            command = control.run_command
        snapshot = run / "inputs"
        for name in INPUTS:
            source = root / name
            linked = source.is_symlink()
            for parent in source.parents:
                if parent == root:
                    break
                linked = linked or parent.is_symlink()
            if linked or not source.is_file():
                raise ValueError("missing or linked authored source: " + name)
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            report["inputs"][name] = sha256(target)
        report["source_revision"] = call(["git", "-C", str(root), "rev-parse", "HEAD"], "source-revision")
        report["xcode"] = call(["xcodebuild", "-version"], "xcode-version")
        sdk = "iphonesimulator" if simulator else "iphoneos"
        sdk_path = call(["xcrun", "--sdk", sdk, "--show-sdk-path"], "sdk-path")
        report["sdk_version"] = call(["xcrun", "--sdk", sdk, "--show-sdk-version"], "sdk-version")
        if not Path(sdk_path).is_dir() or int(report["sdk_version"].split(".")[0]) < 26:
            raise ValueError("iOS 26 or newer SDK required")
        # Preserve the clang++ launcher name; resolving it to clang changes link behavior.
        compiler = call(["xcrun", "--sdk", sdk, "--find", "clang++"], "compiler-path")
        report["compiler_version"] = call([compiler, "--version"], "compiler-version")
        objects = run / "objects"
        objects.mkdir()
        base = snapshot / "experiments/metal-compile"
        triple = "arm64-apple-ios26.0" + ("-simulator" if simulator else "")
        common = [compiler, "-target", triple, "-isysroot", sdk_path, "-std=c++17", "-fobjc-arc", "-fblocks",
                  "-O2", "-gline-tables-only", "-Wall", "-Wextra", "-Werror", "-I", str(base)]
        for name, source in (("runtime", base / "runtime.mm"), ("host", base / "ios/main.mm"),
                             ("store", base / "ios/ProbeRunStore.mm")):
            call(common + ["-DSKYRIM_METAL_RUNTIME_NO_MAIN", "-c", str(source), "-o", str(objects / (name + ".o"))],
                 "compile-" + name, 180)
        app = run / "Payload" / (APP_NAME + ".app")
        app.mkdir(parents=True)
        call(common + [str(objects / (name + ".o")) for name in ("runtime", "host", "store")] + ["-framework", "Foundation",
                       "-framework", "Metal", "-framework", "UIKit", "-Wl,-no_adhoc_codesign",
                       "-o", str(app / APP_NAME)], "link-app", 120)
        (app / APP_NAME).chmod(0o755)
        (app / "Info.plist").write_bytes(make_plist((base / "ios/Info.plist").read_bytes(), bundle, simulator))
        shutil.copyfile(base / "fixture.metal", app / "fixture.metal")
        build_info = {key: value for key, value in report.items() if key not in {"commands", "status"}}
        (app / "BuildInfo.json").write_text(json.dumps(build_info, indent=2) + "\n", encoding="utf-8")
        report["macho"] = validate_app(app, bundle, simulator, report["inputs"]["experiments/metal-compile/fixture.metal"])
        for name, digest in report["inputs"].items():
            if sha256(snapshot / name) != digest:
                raise ValueError("source snapshot changed during build")
        artifact = run / "artifacts" / ("MetalProbe-simulator.app.zip" if simulator else "MetalProbe-unsigned.ipa")
        package_app(app, artifact, simulator)
        report["artifact"] = {"name": artifact.name, "sha256": sha256(artifact), "bytes": artifact.stat().st_size}
        report["app_relative_path"] = str(app.relative_to(run).as_posix())
        report["status"] = "unsigned_simulator_build_passed" if simulator else "unsigned_iphone_build_passed"
    except BaseException as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        report["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        (run / "build-result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        archive_evidence(run)
        print("Metal probe build evidence: " + str(run))
    return run


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", action="store_true", help="explicit UI preview build; never a physical-device proof")
    parser.add_argument("--bundle-id", choices=sorted(ALLOWED_BUNDLES), default=DEFAULT_BUNDLE)
    args = parser.parse_args(argv)
    try:
        execute(ROOT, args.simulator, args.bundle_id)
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
