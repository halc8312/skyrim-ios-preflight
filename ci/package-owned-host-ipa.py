"""Package only the self-authored device test app after an unsigned-image audit."""
import hashlib
import json
from pathlib import Path
import plistlib
import stat
import struct
import sys
import zipfile

EXPECTED_IMAGES = {
    "OwnedGameHost": 2,
    "Frameworks/PrivateGuest.framework/PrivateGuest": 6,
    "Frameworks/WgpuNative.framework/WgpuNative": 6,
}


def image_metadata(data, kind):
    if len(data) < 32:
        raise ValueError("truncated Mach-O")
    magic, cpu, subtype, filetype, count, commands, _, reserved = struct.unpack_from("<8I", data)
    if (magic, cpu, subtype, filetype, reserved) != (0xFEEDFACF, 0x100000C, 0, kind, 0):
        raise ValueError("expected thin plain ARM64 image")
    if not 1 <= count <= 4096 or not 8 <= commands <= 4_194_304 or 32 + commands > len(data):
        raise ValueError("invalid Mach-O command bounds")
    offset, platforms = 32, []
    for _ in range(count):
        if offset + 8 > 32 + commands:
            raise ValueError("truncated load command")
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or size % 8 or offset + size > 32 + commands:
            raise ValueError("invalid load command")
        if command in (0x1D, 0x2B):
            raise ValueError("device unsigned contract forbids signature/DRS commands even when empty")
        if command == 0x32:
            if size < 24:
                raise ValueError("truncated build version")
            platforms.append(struct.unpack_from("<III", data, offset + 8))
        offset += size
    if offset != 32 + commands or platforms != [(2, 0x1A0000, 0x1A0500)]:
        raise ValueError("device platform/minimum/SDK differs")
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                platform=2, minimum_os="26.0", sdk="26.5", code_signature_commands=0)


def package(app, ipa, metadata):
    app, ipa, metadata = map(Path, (app, ipa, metadata))
    if app.name != "OwnedGameHost.app" or app.is_symlink() or not app.is_dir():
        raise ValueError("expected built public app bundle")
    if ipa.exists() or metadata.exists():
        raise ValueError("output must be new")
    info = plistlib.loads((app / "Info.plist").read_bytes())
    if (info.get("CFBundleIdentifier"), info.get("CFBundleExecutable")) != (
            "com.halc8312.ownedgamehost", "OwnedGameHost"):
        raise ValueError("unexpected app identity")
    files, images, names, total = [], {}, set(), 0
    for path in sorted(app.rglob("*")):
        relative = path.relative_to(app)
        if (path.is_symlink() or "_CodeSignature" in relative.parts
                or path.name.lower().endswith(".mobileprovision")):
            raise ValueError("symlink/signing artifact forbidden in unsigned IPA")
        if not path.is_file():
            continue
        name = relative.as_posix()
        if name.casefold() in names:
            raise ValueError("case-alias IPA path")
        names.add(name.casefold())
        data = path.read_bytes()
        total += len(data)
        if total > 256_000_000 or len(files) >= 2000:
            raise ValueError("public app size/count limit")
        if name in EXPECTED_IMAGES:
            images[name] = image_metadata(data, EXPECTED_IMAGES[name])
        elif data[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                           b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf"):
            raise ValueError("unexpected additional executable image")
        files.append((name, data, path.stat().st_mode & 0o777))
    if set(images) != set(EXPECTED_IMAGES):
        raise ValueError("missing expected executable image")
    with ipa.open("xb") as stream:
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=False) as archive:
            for name, data, mode in files:
                entry = zipfile.ZipInfo("Payload/OwnedGameHost.app/" + name, (2026, 1, 1, 0, 0, 0))
                entry.create_system = 3
                entry.external_attr = (stat.S_IFREG | mode) << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.extra = b""
                archive.writestr(entry, data)
    result = dict(public_fixture_only=True, unsigned_device_ipa=True, images=images,
                  ipa_bytes=ipa.stat().st_size, ipa_sha256=hashlib.sha256(ipa.read_bytes()).hexdigest(),
                  app_bundle_identifier=info["CFBundleIdentifier"], bundle_files=len(files),
                  provisioning_profile_present=False, signature_commands_present=False,
                  game_executed=False, physical_iphone_verified=False, ready_to_play=False,
                  signature_authority_authenticated=False)
    with metadata.open("x") as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps(result))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: package-owned-host-ipa.py PUBLIC_APP NEW_IPA NEW_METADATA_JSON")
    package(*sys.argv[1:])
