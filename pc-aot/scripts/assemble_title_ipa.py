"""Assemble the public iPhoneOS host and locally translated guest without signing."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import plistlib
import stat
import zipfile

APP = "Payload/SkyrimIOS.app/"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host-zip", type=Path, required=True)
    p.add_argument("--host-sha256", required=True)
    p.add_argument("--guest-result", type=Path, required=True)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--input-sha256", required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    if sha(a.host_zip) != a.host_sha256 or sha(a.input) != a.input_sha256:
        raise ValueError("Input SHA256 mismatch")
    guest = json.loads(a.guest_result.read_text())
    framework = Path(guest["framework"])
    binary = framework / "SkyrimGuest"
    if not guest.get("strict_link_succeeded") or guest.get("signed") or sha(binary) != guest["sha256"]:
        raise ValueError("Guest build result does not match the unsigned framework")
    a.output.parent.mkdir(parents=True, exist_ok=True)
    records = {}
    total = 0
    with zipfile.ZipFile(a.host_zip) as source:
        for member in source.infolist():
            if member.filename.startswith("__MACOSX/"):
                continue  # ditto's Finder metadata, never application content
            path = PurePosixPath(member.filename)
            if (not member.filename.startswith("SkyrimIOS.app/") or ".." in path.parts
                    or "\\" in member.filename or member.filename.startswith("/")):
                raise ValueError("Unexpected public-host archive path")
            if member.is_dir():
                continue
            mode = member.external_attr >> 16
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                raise ValueError("Host contains a link or special file")
            relative = member.filename[len("SkyrimIOS.app/"):]
            if relative in records or relative.startswith("Frameworks/") or "_CodeSignature" in path.parts or relative.endswith(".mobileprovision"):
                raise ValueError("Unexpected existing framework, signature, or duplicate")
            total += member.file_size
            if member.file_size > 512 << 20 or total > 768 << 20:
                raise ValueError("Public host exceeds expected size")
            records[relative] = (source.read(member), mode & 0o777 or 0o644)
    info = plistlib.loads(records["Info.plist"][0])
    if (info.get("CFBundleIdentifier") != "org.skyrimios.pc2011"
            or info.get("CFBundleExecutable") != "SkyrimIOS"
            or str(info.get("CFBundleVersion")) != "2"):
        raise ValueError("Public-host identity mismatch")
    info["PcGuestExpectedInputSHA256"] = a.input_sha256
    info["LSSupportsGameMode"] = True
    info["MinimumOSVersion"] = "18.0"
    info["CFBundleSupportedPlatforms"] = ["iPhoneOS"]
    info["UIDeviceFamily"] = [1, 2]
    records["Info.plist"] = (plistlib.dumps(info), 0o644)
    for name in ("SkyrimGuest", "Info.plist"):
        records["Frameworks/SkyrimGuest.framework/" + name] = (
            (framework / name).read_bytes(), 0o755 if name == "SkyrimGuest" else 0o644)
    with a.output.open("xb") as dest:
        with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as target:
            for relative, (raw, mode) in sorted(records.items()):
                entry = zipfile.ZipInfo(APP + relative)
                entry.create_system = 3
                entry.external_attr = (stat.S_IFREG | mode) << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                target.writestr(entry, raw)
    with zipfile.ZipFile(a.output) as check:
        if check.testzip() is not None:
            raise ValueError("Assembled ZIP failed CRC validation")
    print(json.dumps({"unsigned_ipa": str(a.output), "sha256": sha(a.output),
                     "guest_sha256": guest["sha256"], "files": len(records),
                     "signed": False, "game_executed": False}))


if __name__ == "__main__":
    main()
