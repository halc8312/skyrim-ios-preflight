#!/usr/bin/env python3
"""Sign a locally assembled SkyrimIOS IPA with the owner's macOS keychain.

No certificate or private key is exported. The provisioning profile is decoded
by Apple's security tool; that decode is not an independent Apple-chain audit.
codesign verification and device authorization checks do not prove installation
or gameplay. All staged inputs, logs and failures remain under private/.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APP = "Payload/SkyrimIOS.app"
FRAMEWORK = "Frameworks/SkyrimGuest.framework"
MAX_ARCHIVE = 768 << 20
MAX_MEMBER = 512 << 20
MAX_PROFILE = 4 << 20
MAX_ENTRIES = 4096
MEMORY_KEYS = ("com.apple.developer.kernel.extended-virtual-addressing",
               "com.apple.developer.kernel.increased-memory-limit")
DEBUG_MEMORY = "com.apple.developer.kernel.increased-debugging-memory-limit"


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def explicit_bundle_id(value):
    return isinstance(value, str) and bool(re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9-]*(?:\.[A-Za-z0-9][A-Za-z0-9-]*)+", value))


def validate_profile(profile, identity, bundle_id, now=None):
    """Validate decoded authorization, never assert a CMS trust-chain result."""
    require(isinstance(profile, dict), "Profile must be a property-list dictionary")
    require(isinstance(identity, str) and re.fullmatch(r"[0-9A-Fa-f]{40}", identity),
            "Use the full 40-character SHA-1 keychain signing identity")
    require(explicit_bundle_id(bundle_id), "An explicit bundle identifier is required")
    ent = profile.get("Entitlements")
    require(isinstance(ent, dict), "Profile entitlements are missing")
    teams = profile.get("TeamIdentifier")
    require(isinstance(teams, list) and len(teams) == 1 and
            isinstance(teams[0], str) and re.fullmatch(r"[A-Z0-9]{10}", teams[0]),
            "Profile must identify one Apple developer team")
    prefixes = profile.get("ApplicationIdentifierPrefix")
    require(isinstance(prefixes, list) and len(prefixes) == 1 and
            isinstance(prefixes[0], str) and re.fullmatch(r"[A-Z0-9]{10}", prefixes[0]),
            "Profile must identify one application identifier prefix")
    app_id = prefixes[0] + "." + bundle_id
    require(ent.get("application-identifier") == app_id,
            "Profile App ID does not exactly match the requested bundle identifier")
    require(ent.get("com.apple.developer.team-identifier") == teams[0],
            "Profile team authorization is inconsistent")
    platforms = profile.get("Platform")
    require(isinstance(platforms, list) and "iOS" in platforms,
            "An iOS provisioning profile is required")
    require(profile.get("ProvisionsAllDevices", False) is False,
            "This helper supports registered-device development or Ad Hoc profiles only")
    devices = profile.get("ProvisionedDevices")
    require(isinstance(devices, list) and devices and all(
        isinstance(d, str) and re.fullmatch(r"[A-Za-z0-9-]{8,128}", d) for d in devices),
        "The profile must contain registered test devices")
    require(len(set(devices)) == len(devices), "Profile has duplicate registered devices")
    expires = profile.get("ExpirationDate")
    require(isinstance(expires, datetime), "Profile expiration is missing")
    if expires.tzinfo is None:
        expires = expires.replace(tzinfo=timezone.utc)
    now = now or datetime.now(timezone.utc)
    if now.tzinfo is None:
        now = now.replace(tzinfo=timezone.utc)
    require(expires > now, "Provisioning profile has expired")
    created = profile.get("CreationDate")
    if created is not None:
        require(isinstance(created, datetime), "Profile creation time is invalid")
        if created.tzinfo is None:
            created = created.replace(tzinfo=timezone.utc)
        require(created <= now, "Provisioning profile is not yet valid")
    certificates = profile.get("DeveloperCertificates")
    require(isinstance(certificates, list) and certificates and all(
        isinstance(c, bytes) and c for c in certificates), "Profile signing certificates are missing")
    require(identity.upper() in {hashlib.sha1(c).hexdigest().upper() for c in certificates},
            "The selected signing certificate is not authorized by this profile")
    require(type(ent.get("get-task-allow")) is bool, "Profile signing mode is missing")
    for key in MEMORY_KEYS:
        require(ent.get(key) is True, "Profile must genuinely authorize " + key)
    result = {"application-identifier": app_id,
              "com.apple.developer.team-identifier": teams[0],
              "get-task-allow": ent["get-task-allow"],
              **{key: True for key in MEMORY_KEYS}}
    # The debugging allowance is development-profile dependent. Never invent it.
    if ent.get(DEBUG_MEMORY) is True:
        result[DEBUG_MEMORY] = True
    return result


def keychain_identity_available(output, identity):
    found = re.findall(rb"(?m)^\s*\d+\)\s+([0-9a-fA-F]{40})\s+\"", output)
    return identity.upper().encode("ascii") in {s.upper() for s in found}


def unsigned_members(archive):
    """Check every name before extraction, including case-insensitive collisions."""
    members = archive.infolist()
    require(0 < len(members) <= MAX_ENTRIES, "Unexpected archive entry count")
    seen, files = set(), {}
    total = 0
    for member in members:
        name = member.filename
        require(member.orig_filename == name, "Archive filename was normalized or truncated")
        directory = name.endswith("/")
        bare = name[:-1] if directory else name
        parts = bare.split("/")
        require(all(p not in ("", ".", "..") for p in parts) and
                not name.startswith("/") and "\\" not in name and ":" not in name and
                not any(ord(c) < 32 or ord(c) == 127 for c in name),
                "Archive contains a noncanonical path")
        require((directory and bare in ("Payload", APP)) or bare.startswith(APP + "/"),
                "Archive contains content outside SkyrimIOS.app")
        folded = bare.casefold()
        require(folded not in seen, "Archive contains duplicate or case-colliding paths")
        seen.add(folded)
        mode = member.external_attr >> 16
        expected = stat.S_IFDIR if directory else stat.S_IFREG
        require(stat.S_IFMT(mode) in (0, expected), "Archive contains a link or special file")
        require(not member.flag_bits & 1, "Encrypted ZIP members are unsupported")
        require("_codesignature" not in [p.casefold() for p in parts] and
                not name.lower().endswith(".mobileprovision"),
                "Input must not contain a signing profile or bundle signatures")
        relative = bare[len(APP) + 1:] if bare.startswith(APP + "/") else ""
        if relative.startswith("Frameworks/"):
            require(relative == "Frameworks/SkyrimGuest.framework" or
                    relative in (FRAMEWORK + "/SkyrimGuest", FRAMEWORK + "/Info.plist"),
                    "Unexpected nested framework content")
        elif relative and relative != "Frameworks":
            require(not any(p.lower().endswith((".app", ".appex", ".xpc", ".framework", ".dylib"))
                            for p in PurePosixPath(relative).parts), "Unexpected nested executable bundle")
        if directory:
            continue
        total += member.file_size
        require(member.file_size <= MAX_MEMBER and total <= MAX_ARCHIVE,
                "Archive exceeds the uncompressed size limit")
        files[bare] = member
    file_names = {name.casefold() for name in files}
    for name in seen:
        parent = PurePosixPath(name).parent
        while str(parent) != ".":
            require(str(parent) not in file_names, "Archive has a file/directory path collision")
            parent = parent.parent
    required = (APP + "/Info.plist", APP + "/SkyrimIOS",
                APP + "/" + FRAMEWORK + "/Info.plist", APP + "/" + FRAMEWORK + "/SkyrimGuest")
    require(all(name in files for name in required), "Assembled app or private guest is missing")
    return files


def validate_app_info(archive, files):
    for relative, executable in (("Info.plist", "SkyrimIOS"),
                                 (FRAMEWORK + "/Info.plist", "SkyrimGuest")):
        member = files[APP + "/" + relative]
        require(member.file_size <= 1 << 20, "Bundle property list exceeds its bound")
        info = plistlib.loads(archive.read(member))
        require(isinstance(info, dict) and info.get("CFBundleExecutable") == executable,
                "Unexpected bundle executable")
        if executable == "SkyrimIOS":
            require("PcDiagnostics" not in info,
                    "Friend builds must not contain another person's diagnostic pairing")
            require(isinstance(info.get("PcGuestExpectedInputSHA256"), str) and
                    re.fullmatch(r"[0-9a-f]{64}", info["PcGuestExpectedInputSHA256"]),
                    "Owned-input binding is missing")
            require(info.get("MinimumOSVersion") == "18.0" and
                    info.get("CFBundleSupportedPlatforms") == ["iPhoneOS"],
                    "Expected the iOS 18+ device host")
        binary_name = APP + "/" + ("" if executable == "SkyrimIOS" else FRAMEWORK + "/") + executable
        with archive.open(files[binary_name]) as stream:
            header = stream.read(32)
        require(len(header) == 32, "Truncated executable")
        magic, cpu, _, file_type = struct.unpack("<IIII", header[:16])
        require(magic == 0xFEEDFACF and cpu == 0x0100000C and
                file_type == (2 if executable == "SkyrimIOS" else 6),
                "Expected thin ARM64 Mach-O app and guest executables")


def snapshot_file(source, destination, limit):
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    descriptor = os.open(source, flags)
    with os.fdopen(descriptor, "rb") as incoming:
        before = os.fstat(incoming.fileno())
        require(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= limit,
                "Input must be a regular file within the size limit")
        copied = 0
        with destination.open("xb") as output:
            while block := incoming.read(1 << 20):
                copied += len(block)
                require(copied <= limit, "Input changed beyond its size limit")
                output.write(block)
        after = os.fstat(incoming.fileno())
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) and copied == before.st_size,
                "Input changed while being copied")


def run_tool(command, log):
    try:
        result = subprocess.run([str(c) for c in command], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=180, check=False)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b"") + b"\n--- timed out; stderr ---\n" + (error.stderr or b""))
        raise RuntimeError(Path(command[0]).name + " timed out; inspect the private signing log") from error
    log.write_bytes(result.stdout + b"\n--- stderr ---\n" + result.stderr)
    if result.returncode:
        raise RuntimeError(Path(command[0]).name + " failed; inspect the private signing log")
    return result.stdout


def private_output(path, root=ROOT):
    root = root.resolve()
    private = root / "private"
    require(not private.is_symlink(), "private/ must not be a symbolic link")
    path = path.expanduser().absolute()
    require(not os.path.lexists(path), "Output already exists; choose a new IPA path")
    resolved = path.resolve()
    require(resolved.is_relative_to(private) and resolved.suffix.lower() == ".ipa",
            "Output must be a new .ipa inside this checkout's private/ directory")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    return resolved


def sign_package(unsigned, profile, identity, bundle_id, output, *, root=ROOT, runner=run_tool):
    output = private_output(Path(output), root)
    work = Path(tempfile.mkdtemp(prefix="sign-title-", dir=output.parent))
    logs = work / "logs"
    logs.mkdir()
    # Keep this marker even on a failed command; it contains no credentials.
    report = {"schema": 1, "signed": False, "published": False, "codesign_verified": False,
              "profile_apple_chain_independently_verified": False,
              "installed": False, "game_executed": False}
    report_path = work / "result.json"
    try:
        require(isinstance(identity, str) and re.fullmatch(r"[0-9A-Fa-f]{40}", identity),
                "Use the full 40-character SHA-1 keychain signing identity")
        identity = identity.upper()
        snapshot_file(unsigned, work / "unsigned.ipa", MAX_ARCHIVE)
        snapshot_file(profile, work / "profile.mobileprovision", MAX_PROFILE)
        report.update(input_sha256=sha(work / "unsigned.ipa"), profile_sha256=sha(work / "profile.mobileprovision"))
        decoded = runner(["/usr/bin/security", "cms", "-D", "-i", work / "profile.mobileprovision"],
                         logs / "profile-decode.log")
        require(len(decoded) <= MAX_PROFILE, "Decoded profile exceeds its bound")
        authorization = plistlib.loads(decoded)
        entitlements = validate_profile(authorization, identity, bundle_id)
        identities = runner(["/usr/bin/security", "find-identity", "-v", "-p", "codesigning"],
                            logs / "keychain-identities.log")
        require(keychain_identity_available(identities, identity),
                "The matching valid signing identity/private key is not available in this keychain")
        stage = work / "stage"
        stage.mkdir()
        with zipfile.ZipFile(work / "unsigned.ipa") as archive:
            files = unsigned_members(archive)
            validate_app_info(archive, files)
            for name, member in files.items():
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as incoming, target.open("xb") as outgoing:
                    while block := incoming.read(1 << 20):
                        outgoing.write(block)
                target.chmod(0o755 if target.name in ("SkyrimIOS", "SkyrimGuest") else 0o644)
        app = stage / APP
        info_path = app / "Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        info["CFBundleIdentifier"] = bundle_id
        info_path.write_bytes(plistlib.dumps(info))
        (app / "embedded.mobileprovision").write_bytes((work / "profile.mobileprovision").read_bytes())
        entitlements_path = work / "entitlements.plist"
        entitlements_path.write_bytes(plistlib.dumps(entitlements))
        framework = app / FRAMEWORK
        runner(["/usr/bin/codesign", "--force", "--sign", identity, "--timestamp=none",
                "--generate-entitlement-der", framework], logs / "sign-framework.log")
        runner(["/usr/bin/codesign", "--force", "--sign", identity, "--timestamp=none",
                "--generate-entitlement-der", "--entitlements", entitlements_path, app],
               logs / "sign-app.log")
        runner(["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", framework],
               logs / "verify-framework.log")
        runner(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", app],
               logs / "verify-app.log")
        signed_entitlements = runner(["/usr/bin/codesign", "-d", "--entitlements", ":-", app],
                                     logs / "signed-entitlements.log")
        require(plistlib.loads(signed_entitlements) == entitlements,
                "Signed app entitlements differ from the reviewed profile subset")
        for target, name in ((framework, "framework"), (app, "app")):
            prefix = work / (name + "-signer-")
            runner(["/usr/bin/codesign", "-d", "--extract-certificates", prefix, target],
                   logs / (name + "-signer.log"))
            certificate = Path(str(prefix) + "0")
            require(certificate.is_file() and hashlib.sha1(certificate.read_bytes()).hexdigest().upper() == identity,
                    "Signed executable uses an unexpected certificate")
        require(sha(app / "embedded.mobileprovision") == report["profile_sha256"],
                "Embedded provisioning profile changed")
        # Fail if a tool unexpectedly replaced any unchanged resource or injected pairing.
        require("PcDiagnostics" not in plistlib.loads(info_path.read_bytes()), "Unexpected diagnostic pairing")
        with zipfile.ZipFile(work / "unsigned.ipa") as archive:
            for name in files:
                if name in (APP + "/Info.plist", APP + "/SkyrimIOS", APP + "/" + FRAMEWORK + "/SkyrimGuest"):
                    continue
                with archive.open(files[name]) as stream:
                    expected = hashlib.file_digest(stream, "sha256").hexdigest()
                require(sha(stage / name) == expected, "Signing modified an application resource")
        signed = work / "signed.ipa"
        with signed.open("xb") as destination:
            with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                for path in sorted(stage.rglob("*")):
                    require(not path.is_symlink(), "Signing stage contains a symbolic link")
                    if path.is_file():
                        archive.write(path, path.relative_to(stage).as_posix())
            destination.flush()
            os.fsync(destination.fileno())
        with zipfile.ZipFile(signed) as archive:
            require(archive.testzip() is None, "Signed IPA ZIP integrity check failed")
        report.update(signed=True, codesign_verified=True, sha256=sha(signed),
                      bytes=signed.stat().st_size, registered_devices=len(authorization["ProvisionedDevices"]),
                      development_profile=entitlements["get-task-allow"], diagnostic_pairing=False)
        # Same directory/filesystem: publish atomically without replacing any existing output.
        os.link(signed, output)
        report["published"] = True
        report["output"] = str(output)
        return {**report, "private_report": str(report_path)}
    except BaseException:
        report["failure_evidence"] = True
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unsigned-ipa", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--identity", required=True, help="Full SHA-1 from security find-identity -v -p codesigning")
    parser.add_argument("--bundle-id", required=True, help="Explicit App ID registered in your own developer account")
    parser.add_argument("--output", type=Path, required=True, help="New file under this checkout's ignored private/")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Signing requires macOS and an existing Apple signing identity in its keychain")
    os.umask(0o077)
    try:
        result = sign_package(args.unsigned_ipa, args.profile, args.identity, args.bundle_id, args.output)
    except (OSError, ValueError, RuntimeError, plistlib.InvalidFileException, zipfile.BadZipFile,
            subprocess.SubprocessError) as error:
        parser.exit(1, "Signing failed: " + str(error) + "\nPrivate signing evidence was retained beside the requested output.\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
