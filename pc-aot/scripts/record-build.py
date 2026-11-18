#!/usr/bin/env python3
"""Private CI evidence and strict probe validation. Python standard library only."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zipfile

EXPECTED_XCODE = "Xcode 26.6\nBuild version 17F113"
APP_NAME = "SkyrimProbe"


def command(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def version(value: int) -> str:
    return f"{value >> 16}.{(value >> 8) & 255}.{value & 255}"


def macho(data: bytes, *, dsym: bool = False) -> dict:
    """Accept one thin little-endian ARM64 Mach-O; reject simulator/universal."""
    if len(data) < 32:
        raise ValueError("truncated Mach-O")
    magic, cpu, subtype, kind, count, size, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C or subtype & 0xFFFFFF != 0:
        raise ValueError("expected thin ARM64 (not x86, arm64e, or a fat binary)")
    if kind != (10 if dsym else 2):
        raise ValueError("unexpected Mach-O file type")
    end = 32 + size
    if end > len(data):
        raise ValueError("truncated Mach-O load commands")
    result = {"architecture": "arm64", "signed_load_command": False}
    offset = 32
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError("truncated load command")
        cmd, length = struct.unpack_from("<2I", data, offset)
        if length < 8 or offset + length > end:
            raise ValueError("invalid load command length")
        if cmd == 0x32:
            if length < 24 or "platform" in result:
                raise ValueError("invalid/duplicate build-version command")
            platform, minimum, sdk, _ = struct.unpack_from("<4I", data, offset + 8)
            result.update(platform=platform, minimum_os=version(minimum), sdk=version(sdk))
        elif cmd == 0x1B:
            if length != 24 or "uuid" in result:
                raise ValueError("invalid/duplicate UUID command")
            result["uuid"] = str(uuid.UUID(bytes=data[offset + 8:offset + 24])).upper()
        elif cmd == 0x1D:
            if length != 16:
                raise ValueError("invalid code signature command")
            start, length_bytes = struct.unpack_from("<2I", data, offset + 8)
            if not length_bytes or start + length_bytes > len(data):
                raise ValueError("invalid code signature range")
            result["signed_load_command"] = True
        offset += length
    if offset != end or "uuid" not in result:
        raise ValueError("invalid command bounds or missing executable UUID")
    if not dsym and (result.get("platform") != 2 or result.get("minimum_os") != "18.0.0"):
        raise ValueError("expected iPhoneOS platform 2 and deployment target 18.0")
    return result


def validate_profile(profile: dict, bundle: str, team: str, udid: str) -> dict:
    ent = profile.get("Entitlements", {})
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]+", bundle):
        raise ValueError("invalid explicit bundle identifier")
    if profile.get("TeamIdentifier") != [team] or not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise ValueError("profile team mismatch")
    if ent.get("application-identifier") != f"{team}.{bundle}":
        raise ValueError("profile must match exact app identifier; wildcard rejected")
    if ent.get("com.apple.developer.team-identifier") != team:
        raise ValueError("profile entitlement team mismatch")
    if ent.get("get-task-allow") is not False or profile.get("ProvisionsAllDevices", False):
        raise ValueError("expected ad hoc distribution profile")
    if not udid or udid not in profile.get("ProvisionedDevices", []):
        raise ValueError("requested registered device is absent from profile")
    expiration = profile.get("ExpirationDate")
    if not isinstance(expiration, dt.datetime) or expiration.replace(tzinfo=dt.timezone.utc) <= dt.datetime.now(dt.timezone.utc):
        raise ValueError("profile expired or expiration missing")
    if not profile.get("DeveloperCertificates"):
        raise ValueError("profile contains no distribution certificate")
    profile_uuid = str(uuid.UUID(profile["UUID"])).upper()
    return {"uuid": profile_uuid, "expiration_utc": expiration.isoformat(),
            "registered_device_checked": True, "profile_type": "ad_hoc"}


def required(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise ValueError(f"required existing signing configuration missing: {name}")
    return value


def load_profile(path: Path) -> dict:
    result = subprocess.run(["security", "cms", "-D", "-i", str(path)], check=True, capture_output=True)
    return plistlib.loads(result.stdout)


def save_manifest(run: Path, manifest: dict) -> None:
    (run / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def begin(run: Path, mode: str, root: Path) -> None:
    commit = command("git", "-C", str(root), "rev-parse", "HEAD")
    manifest = {"schema": 1, "purpose": "diagnostic-probe-only", "mode": mode,
                "status": "started", "git_commit": commit,
                "git_tree": command("git", "-C", str(root), "rev-parse", "HEAD^{tree}"),
                "ci_provider": "codemagic" if os.environ.get("CM_BUILD_ID") else ("github-actions" if os.environ.get("GITHUB_RUN_ID") else "local"),
                "ci_build_id": os.environ.get("CM_BUILD_ID") or os.environ.get("GITHUB_RUN_ID"),
                "ci_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
                "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "jobs": 2, "device_gameplay_tested": False, "installable": False,
                "bundle_identifier": os.environ.get("SKYRIM_BUNDLE_ID", "org.skyrimios.probe"),
                "expected_xcode": EXPECTED_XCODE, "machine": command("uname", "-m"),
                "macos": command("sw_vers", "-productVersion")}
    save_manifest(run, manifest)
    if command("git", "-C", str(root), "status", "--porcelain"):
        raise ValueError("build requires clean committed source to identify exact CI input")
    if os.environ.get("CM_COMMIT") and os.environ["CM_COMMIT"] != commit:
        raise ValueError("Codemagic commit and checked-out Git commit differ")
    if os.environ.get("GITHUB_SHA") and os.environ["GITHUB_SHA"] != commit:
        raise ValueError("GitHub Actions commit and checked-out Git commit differ")
    manifest.update(xcode=command("xcodebuild", "-version"),
                    sdk_version=command("xcrun", "--sdk", "iphoneos", "--show-sdk-version"),
                    sdk_build=command("xcrun", "--sdk", "iphoneos", "--show-sdk-build-version"),
                    sdk_path=command("xcrun", "--sdk", "iphoneos", "--show-sdk-path"),
                    compiler=command("xcrun", "--sdk", "iphoneos", "clang", "--version"))
    save_manifest(run, manifest)
    if manifest["xcode"] != EXPECTED_XCODE or manifest["machine"] != "arm64":
        raise ValueError("pinned Xcode build or ARM64 build host mismatch")
    if "iPhoneOS" not in manifest["sdk_path"] or int(manifest["sdk_version"].split(".")[0]) < 26:
        raise ValueError("expected iPhoneOS 26 or newer SDK")


def prepare_profile(run: Path) -> None:
    bundle, team, udid = (required(name) for name in ("SKYRIM_BUNDLE_ID", "SKYRIM_TEAM_ID", "SKYRIM_DEVICE_UDID"))
    identity = required("SKYRIM_SIGNING_IDENTITY").upper()
    if not re.fullmatch(r"[A-F0-9]{40}", identity):
        raise ValueError("SKYRIM_SIGNING_IDENTITY must be existing distribution certificate SHA-1")
    profile = load_profile(Path(required("SKYRIM_PROFILE_PATH")))
    info = validate_profile(profile, bundle, team, udid)
    if identity not in {hashlib.sha1(c).hexdigest().upper() for c in profile["DeveloperCertificates"]}:
        raise ValueError("selected signing certificate not in profile")
    config = {"PRODUCT_BUNDLE_IDENTIFIER": bundle, "DEVELOPMENT_TEAM": team,
              "CODE_SIGN_STYLE": "Manual", "CODE_SIGN_IDENTITY": identity,
              "PROVISIONING_PROFILE_SPECIFIER": info["uuid"]}
    (run / "signing.xcconfig").write_text("".join(f"{key} = {value}\n" for key, value in config.items()), encoding="utf-8")
    export = {"method": "release-testing", "destination": "export", "signingStyle": "manual",
              "teamID": team, "signingCertificate": identity,
              "provisioningProfiles": {bundle: info["uuid"]}, "stripSwiftSymbols": False,
              "manageAppVersionAndBuildNumber": False, "thinning": "<none>"}
    (run / "ExportOptions.plist").write_bytes(plistlib.dumps(export))
    manifest = json.loads((run / "manifest.json").read_text())
    manifest["profile"] = info
    manifest["profile_sha256"] = sha256(Path(required("SKYRIM_PROFILE_PATH")))
    save_manifest(run, manifest)


def verify_signature(app: Path, run: Path) -> dict:
    command("codesign", "--verify", "--deep", "--strict", str(app))
    ent = plistlib.loads(subprocess.run(["codesign", "-d", "--entitlements", ":-", str(app)],
                                       check=True, capture_output=True).stdout)
    bundle, team, udid = (required(name) for name in ("SKYRIM_BUNDLE_ID", "SKYRIM_TEAM_ID", "SKYRIM_DEVICE_UDID"))
    if (ent.get("application-identifier") != f"{team}.{bundle}" or
            ent.get("com.apple.developer.team-identifier") != team or ent.get("get-task-allow", False)):
        raise ValueError("actual code-signing entitlements mismatch")
    profile = load_profile(app / "embedded.mobileprovision")
    info = validate_profile(profile, bundle, team, udid)
    prefix = run / ("certificate-" + uuid.uuid4().hex + "-")
    command("codesign", "-d", "--extract-certificates", str(prefix), str(app))
    actual = hashlib.sha1(Path(str(prefix) + "0").read_bytes()).hexdigest().upper()
    if actual != required("SKYRIM_SIGNING_IDENTITY").upper():
        raise ValueError("actual signing certificate differs from selected certificate")
    if actual not in {hashlib.sha1(c).hexdigest().upper() for c in profile["DeveloperCertificates"]}:
        raise ValueError("actual signing certificate absent from embedded profile")
    expected_profile = json.loads((run / "manifest.json").read_text())["profile"]
    if info["uuid"] != expected_profile["uuid"]:
        raise ValueError("embedded profile UUID differs from selected profile")
    return info


def check_app_members(app: Path, signed: bool) -> None:
    allowed = {"Info.plist", "PkgInfo", APP_NAME, "default.metallib"}
    if signed:
        allowed |= {"embedded.mobileprovision", "_CodeSignature/CodeResources"}
    for path in app.rglob("*"):
        rel = path.relative_to(app).as_posix()
        if path.is_symlink() or (path.is_file() and rel not in allowed):
            raise ValueError("unexpected probe bundle member: " + rel)


def check_dsym_members(dsym: Path) -> None:
    allowed = {"Contents/Info.plist", "Contents/Resources/DWARF/SkyrimProbe",
               "Contents/Resources/Relocations/aarch64/SkyrimProbe.yml",
               "Contents/Resources/Relocations/arm64/SkyrimProbe.yml"}
    for path in dsym.rglob("*"):
        rel = path.relative_to(dsym).as_posix()
        if path.is_symlink() or (path.is_file() and rel not in allowed):
            raise ValueError("unexpected dSYM member: " + rel)


def check_archive_members(archive: Path) -> None:
    for path in archive.rglob("*"):
        rel = path.relative_to(archive).as_posix()
        if path.is_symlink() or (path.is_file() and rel != "Info.plist" and
                not rel.startswith(("Products/Applications/SkyrimProbe.app/",
                                    "dSYMs/SkyrimProbe.app.dSYM/"))):
            raise ValueError("unexpected archive member: " + rel)


def verify_app(app: Path, run: Path, signed: bool, expected_uuid: str | None = None) -> dict:
    check_app_members(app, signed)
    info = plistlib.loads((app / "Info.plist").read_bytes())
    bundle = os.environ.get("SKYRIM_BUNDLE_ID", "org.skyrimios.probe")
    if (info.get("CFBundleExecutable") != APP_NAME or info.get("CFBundleIdentifier") != bundle or
            info.get("CFBundleSupportedPlatforms") != ["iPhoneOS"] or
            info.get("DTPlatformName") != "iphoneos" or info.get("MinimumOSVersion") != "18.0"):
        raise ValueError("app bundle, platform, or deployment target mismatch")
    result = macho((app / APP_NAME).read_bytes())
    if result["signed_load_command"] != signed:
        raise ValueError("unexpected executable code-signature state")
    manifest = json.loads((run / "manifest.json").read_text())
    sdk = tuple(int(x) for x in manifest["sdk_version"].split("."))
    sdk = sdk + (0,) * (3 - len(sdk))
    if result["sdk"] != ".".join(map(str, sdk)):
        raise ValueError("Mach-O SDK differs from selected iPhoneOS SDK")
    if expected_uuid and result["uuid"] != expected_uuid:
        raise ValueError("archive/export executable UUID mismatch")
    result["sha256"] = sha256(app / APP_NAME)
    if signed:
        result["profile"] = verify_signature(app, run)
    return result


def zip_tree(source: Path, destination: Path) -> None:
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob("*")):
            if path.is_symlink():
                raise ValueError("symlinks are not supported in probe artifacts")
            if path.is_file():
                archive.write(path, (Path(source.name) / path.relative_to(source)).as_posix())


def verify(run: Path, mode: str) -> None:
    manifest = json.loads((run / "manifest.json").read_text())
    signed = mode == "ad-hoc"
    archive = run / "SkyrimProbe.xcarchive"
    app = (archive / "Products/Applications/SkyrimProbe.app" if signed else
           run / "DerivedData/Build/Products/Release-iphoneos/SkyrimProbe.app")
    dsym = (archive / "dSYMs/SkyrimProbe.app.dSYM" if signed else
            run / "DerivedData/Build/Products/Release-iphoneos/SkyrimProbe.app.dSYM")
    manifest["executable"] = verify_app(app, run, signed)
    check_dsym_members(dsym)
    symbols = macho((dsym / "Contents/Resources/DWARF/SkyrimProbe").read_bytes(), dsym=True)
    if symbols["uuid"] != manifest["executable"]["uuid"]:
        raise ValueError("executable and dSYM UUID mismatch")
    manifest["dsym"] = {"uuid": symbols["uuid"], "sha256": sha256(dsym / "Contents/Resources/DWARF/SkyrimProbe")}
    artifacts = run / "artifacts"
    if signed:
        check_archive_members(archive)
        ipas = list((run / "export").glob("*.ipa"))
        if len(ipas) != 1:
            raise ValueError("expected exactly one exported IPA")
        unpack = run / "export-verified"
        unpack.mkdir()
        with zipfile.ZipFile(ipas[0]) as zipped:
            for item in zipped.infolist():
                parts = Path(item.filename).parts
                if item.filename.startswith("/") or ".." in parts or "\\" in item.filename:
                    raise ValueError("unsafe IPA path")
                if not item.filename.startswith("Payload/"):
                    raise ValueError("unexpected IPA root member")
                if (item.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError("IPA symlink")
            zipped.extractall(unpack)
        if sorted(p.name for p in (unpack / "Payload").iterdir()) != ["SkyrimProbe.app"]:
            raise ValueError("unexpected IPA payload")
        manifest["exported_executable"] = verify_app(unpack / "Payload/SkyrimProbe.app", run, True,
                                                       manifest["executable"]["uuid"])
        shutil.copy2(ipas[0], artifacts / "SkyrimProbe-ad-hoc.ipa")
        zip_tree(archive, artifacts / "SkyrimProbe.xcarchive.zip")
    else:
        zip_tree(app, artifacts / "SkyrimProbe-UNSIGNED-NOT-INSTALLABLE.app.zip")
    zip_tree(dsym, artifacts / "SkyrimProbe.app.dSYM.zip")
    manifest.update(status="verified", installable=signed,
                    installation_scope="registered-device-only; device install not tested" if signed else "not installable")
    save_manifest(run, manifest)


def finish(run: Path, code: int) -> None:
    path = run / "manifest.json"
    manifest = json.loads(path.read_text()) if path.exists() else {"schema": 1, "status": "failed-before-initialization"}
    manifest["exit_code"] = code
    if code:
        manifest["status"] = "failed"
        manifest["installable"] = False
    manifest["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    artifacts = run / "artifacts"
    for log in run.glob("*.log"):
        shutil.copy2(log, artifacts / log.name)
    manifest["artifacts"] = [{"path": p.name, "bytes": p.stat().st_size, "sha256": sha256(p)}
                              for p in sorted(artifacts.iterdir()) if p.is_file() and p.name != "manifest.json"]
    save_manifest(run, manifest)
    shutil.copy2(path, artifacts / "manifest.json")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["begin", "profile", "verify", "finish"])
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--mode", choices=["unsigned", "ad-hoc"], default="unsigned")
    parser.add_argument("--exit-code", type=int, default=0)
    args = parser.parse_args()
    try:
        if args.action == "begin":
            begin(args.run, args.mode, Path(__file__).resolve().parents[1])
        elif args.action == "profile":
            prepare_profile(args.run)
        elif args.action == "verify":
            verify(args.run, args.mode)
        else:
            finish(args.run, args.exit_code)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, plistlib.InvalidFileException, zipfile.BadZipFile) as error:
        print(f"Build evidence/validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
