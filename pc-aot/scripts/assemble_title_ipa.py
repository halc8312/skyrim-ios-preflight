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
# The optional MOD hook interface and the reviewed seam headers its tables use.
MOD_HOOK_HEADERS = {
    "PcGuestModHooksABI.h": "ios/private-abi",
    "lift_menu_seam.h": "experiments/fuz-voice",
    "lift_world_seam.h": "experiments/fuz-voice",
    "lift_voice_seam.h": "experiments/fuz-voice",
    "voice_fallback.h": "experiments/fuz-voice",
}
MOD_HOOK_GROUPS_DEFINED = 63 | 128  # PC_GUEST_MOD_HOOKS_DEFINED in version 1


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


def classic_capable(record, key="classic_skse_experiment"):
    # The older Classic guest uses this name. It distinguishes the callback ABI,
    # independently of which product bundles the framework.
    value = record.get(key, False)
    if type(value) is not bool:
        raise ValueError("Invalid Classic capability marker")
    return value


def mod_hook_groups(record, key="mod_hook_groups"):
    value = record.get(key, 0)
    if (type(value) is not int or value & ~MOD_HOOK_GROUPS_DEFINED or value < 0
            or (value & 2 and not value & 1)):
        raise ValueError("Invalid MOD hook groups marker")
    return value


def host_mod_hooks_abi(record, key="mod_hooks_abi"):
    value = record.get(key, 0)
    if type(value) is not int or value not in (0, 1):
        raise ValueError("Invalid MOD hook ABI marker")
    return value


def same_text(first, second):
    return first.read_bytes().replace(b"\r\n", b"\n") == second.read_bytes().replace(b"\r\n", b"\n")


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
    classic = classic_capable(guest)
    if classic != classic_capable(result) or classic != classic_capable(contract):
        raise ValueError("Host/guest Classic ABI mismatch")
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
        extension = abi.with_name("PcGuestSkseABI.h")
        if (contract.get("classic_abi_header_sha256") != sha(extension)
                or (directory / extension.name).read_bytes().replace(b"\r\n", b"\n")
                != extension.read_bytes().replace(b"\r\n", b"\n")):
            raise ValueError("Host/guest Classic binding header mismatch")
    groups = mod_hook_groups(guest)
    if groups != mod_hook_groups(contract):
        raise ValueError("Guest MOD hook groups differ from its compile contract")
    if groups:
        if not classic or host_mod_hooks_abi(result) != 1:
            raise ValueError("Guest MOD hooks require a Classic host with the MOD hook ABI")
        recorded = contract.get("mod_hooks_header_sha256")
        if not isinstance(recorded, dict) or set(recorded) != set(MOD_HOOK_HEADERS):
            raise ValueError("Host/guest MOD hook header mismatch")
        for name, folder in MOD_HOOK_HEADERS.items():
            source = ROOT / folder / name
            if recorded[name] != sha(source) or not same_text(directory / name, source):
                raise ValueError("Host/guest MOD hook header mismatch: " + name)
    else:
        host_mod_hooks_abi(result)
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
    classic = classic_capable(guest)
    framework_plist = (framework / "Info.plist").read_bytes()
    if classic != classic_capable(plistlib.loads(framework_plist), "PcGuestClassicSkseExperiment"):
        raise ValueError("Guest framework Classic ABI mismatch")
    if classic and not a.host_artifacts:
        raise ValueError("Classic assembly requires --host-artifacts with all four ABI headers")
    groups = mod_hook_groups(guest)
    if groups != mod_hook_groups(plistlib.loads(framework_plist), "PcGuestModHooks"):
        raise ValueError("Guest framework MOD hook groups mismatch")
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
    if classic != classic_capable(info, "PcGuestClassicSkseExperiment"):
        raise ValueError("Host/guest Classic ABI mismatch")
    if groups and host_mod_hooks_abi(info, "PcGuestModHooksABI") != 1:
        raise ValueError("Host Info.plist lacks the MOD hook ABI")
    expected_build = plistlib.loads((ROOT / "ios/runtime-port/Info.plist.in").read_bytes())["CFBundleVersion"]
    if (info.get("CFBundleIdentifier") != "org.skyrimios.pc2011"
            or info.get("CFBundleExecutable") != "SkyrimIOS"
            or str(info.get("CFBundleVersion")) != expected_build):
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
    records["Frameworks/SkyrimGuest.framework/SkyrimGuest"] = (binary.read_bytes(), 0o755)
    records["Frameworks/SkyrimGuest.framework/Info.plist"] = (framework_plist, 0o644)
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
