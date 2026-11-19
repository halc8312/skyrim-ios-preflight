"""Assemble the public iPhoneOS host and locally translated guest without signing."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import plistlib
import re
import stat
import zipfile

APP = "Payload/SkyrimIOS.app/"
ROOT = Path(__file__).resolve().parents[1]


def load_diagnostics_pairing(path):
    """Private final-assembly input; never part of public host/CI output."""
    if path.stat().st_size > 4096:
        raise ValueError("Diagnostic pairing exceeds size bound")
    pairing = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(pairing, dict) or set(pairing) != {"endpoint", "token"}:
        raise ValueError("Invalid diagnostic pairing schema")
    hostname = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    if (not isinstance(pairing["endpoint"], str)
            or not re.fullmatch(r"https://" + hostname + r"\." + hostname
                + r"\.ts\.net/skyrim-diagnostics/v1/reports", pairing["endpoint"])
            or not isinstance(pairing["token"], str)
            or not re.fullmatch(r"[A-Za-z0-9_-]{43,128}", pairing["token"])):
        raise ValueError("Diagnostic pairing requires the private HTTPS receiver and a dedicated token")
    return pairing


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def classic_variant(value):
    """Older ordinary manifests have no marker. Only actual booleans are valid."""
    if type(value) is not bool:
        raise ValueError("Invalid Classic experiment marker")
    return value


def host_artifacts(directory, guest_result, input_sha256):
    """Bind the easy self-build route to the actual host/guest ABI evidence."""
    result = json.loads((directory / "result.json").read_text())
    archive = directory / "SkyrimIOS-public-host.zip"
    if (result.get("public_host_sdk_build") is not True
            or result.get("private_guest_included") is not False
            or result.get("target") != "arm64-apple-ios"
            or result.get("artifact_sha256") != sha(archive)):
        raise ValueError("Host artifacts are not a verified public iPhoneOS build")
    guest = json.loads(guest_result.read_text())
    if guest.get("candidate_sha256") != input_sha256:
        raise ValueError("Guest result is not bound to this owned input; use a fresh self-build stage")
    build = guest_result.resolve().parent
    stage = build.parent
    contract = json.loads((build / "compile-contract.json").read_text())
    classic = classic_variant(guest.get("classic_skse_experiment", False))
    if (classic != classic_variant(result.get("classic_skse_experiment", False))
            or classic != classic_variant(contract.get("classic_skse_experiment", False))):
        raise ValueError("Host/guest Classic experiment mismatch")
    runtime = stage / "source/runtime"
    for name in ("x86.h", "rt_table.h"):
        source = runtime / name
        if contract.get("headers", {}).get(name) != sha(source):
            raise ValueError("Guest ABI source changed after compilation: " + name)
        original = source.read_bytes().replace(b"\r\n", b"\n")
        host = (directory / name).read_bytes().replace(b"\r\n", b"\n")
        if name == "rt_table.h":
            # The public late-binding adapter changes this declaration only.
            original = original.replace(b"extern const FnModule g_modules[];",
                b"extern FnModule g_modules[]; /* assigned once before any guest thread starts */")
        if host != original:
            raise ValueError("Host/guest ABI header mismatch: " + name)
    abi = ROOT / "ios/private-abi/PcGuestABI.h"
    if (contract.get("abi_header_sha256") != sha(abi)
            or (directory / "PcGuestABI.h").read_bytes().replace(b"\r\n", b"\n")
            != abi.read_bytes().replace(b"\r\n", b"\n")):
        raise ValueError("Host/guest binding header mismatch")
    if classic:
        classic_abi = abi.with_name("PcGuestSkseABI.h")
        if (contract.get("classic_abi_header_sha256") != sha(classic_abi)
                or (directory / classic_abi.name).read_bytes().replace(b"\r\n", b"\n")
                != classic_abi.read_bytes().replace(b"\r\n", b"\n")):
            raise ValueError("Host/guest Classic binding header mismatch")
    return archive, result["artifact_sha256"]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    hosts = p.add_mutually_exclusive_group(required=True)
    hosts.add_argument("--host-zip", type=Path)
    hosts.add_argument("--host-artifacts", type=Path,
                       help="Successful host artifacts directory; also verifies shared ABI headers")
    p.add_argument("--host-sha256", help="Required with --host-zip; read from that build's result.json")
    p.add_argument("--guest-result", type=Path, required=True)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--input-sha256", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--diagnostics-pairing", type=Path,
                   help="Private endpoint/token JSON added only during local assembly")
    a = p.parse_args()
    if a.host_artifacts:
        if a.host_sha256:
            p.error("--host-sha256 is only used with --host-zip")
        a.host_zip, a.host_sha256 = host_artifacts(a.host_artifacts, a.guest_result, a.input_sha256)
    elif not a.host_sha256:
        p.error("--host-zip requires --host-sha256")
    if sha(a.host_zip) != a.host_sha256 or sha(a.input) != a.input_sha256:
        raise ValueError("Input SHA256 mismatch")
    guest = json.loads(a.guest_result.read_text())
    framework = Path(guest["framework"])
    binary = framework / "SkyrimGuest"
    if not guest.get("strict_link_succeeded") or guest.get("signed") or sha(binary) != guest["sha256"]:
        raise ValueError("Guest build result does not match the unsigned framework")
    guest_classic = classic_variant(guest.get("classic_skse_experiment", False))
    guest_info = plistlib.loads((framework / "Info.plist").read_bytes())
    if classic_variant(guest_info.get("PcGuestClassicSkseExperiment", False)) != guest_classic:
        raise ValueError("Guest framework Classic experiment mismatch")
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
    if classic_variant(info.get("PcGuestClassicSkseExperiment", False)) != guest_classic:
        raise ValueError("Host/guest Classic experiment mismatch")
    expected_id = "org.skyrimios.pc2011.classic-lab" if guest_classic else "org.skyrimios.pc2011"
    expected_builds = {"16", "17"} if guest_classic else {"16"}
    if (info.get("CFBundleIdentifier") != expected_id
            or info.get("CFBundleExecutable") != "SkyrimIOS"
            or str(info.get("CFBundleVersion")) not in expected_builds):
        raise ValueError("Public-host identity mismatch")
    info["PcGuestExpectedInputSHA256"] = a.input_sha256
    info["LSSupportsGameMode"] = True
    info["MinimumOSVersion"] = "18.0"
    info["CFBundleSupportedPlatforms"] = ["iPhoneOS"]
    info["UIDeviceFamily"] = [1, 2]
    if "PcDiagnostics" in info:
        raise ValueError("Public host must not contain a diagnostic pairing")
    if a.diagnostics_pairing:
        info["PcDiagnostics"] = load_diagnostics_pairing(a.diagnostics_pairing)
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
