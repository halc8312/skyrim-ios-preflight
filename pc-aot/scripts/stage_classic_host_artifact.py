"""Select the unsigned public Classic Lab host and headers needed for local assembly."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import stat
import tempfile
import zipfile

SCRIPTS = Path(__file__).resolve().parent
ABI = SCRIPTS.parent / "ios/private-abi"
HOST = "SkyrimIOS-public-host.zip"
ABI_HEADERS = ("PcGuestABI.h", "PcGuestSkseABI.h", "x86.h", "rt_table.h")
APP_FILES = {"SkyrimIOS.app/SkyrimIOS", "SkyrimIOS.app/Info.plist", "SkyrimIOS.app/ModArchiveLicenses.txt"}


def validate_app(path, linked_executable):
    with zipfile.ZipFile(path) as archive:
        files = set()
        for entry in archive.infolist():
            name = entry.filename
            if entry.is_dir() and name == "SkyrimIOS.app/":
                continue
            kind = stat.S_IFMT(entry.external_attr >> 16)
            if name not in APP_FILES or name in files or kind not in (0, stat.S_IFREG):
                raise ValueError("Unexpected or duplicated public host archive member")
            files.add(name)
        if files != APP_FILES or not archive.read("SkyrimIOS.app/ModArchiveLicenses.txt").strip():
            raise ValueError("Incomplete public application or license notice")
        info = plistlib.loads(archive.read("SkyrimIOS.app/Info.plist"))
        if (info.get("CFBundleIdentifier") != "org.skyrimios.pc2011.classic-lab"
                or info.get("CFBundleDisplayName") != "SkyrimIOS Classic Lab"
                or info.get("CFBundleExecutable") != "SkyrimIOS"
                or info.get("PcGuestClassicSkseExperiment") is not True
                or "PcGuestExpectedInputSHA256" in info):
            raise ValueError("Unexpected application identity or privately assembled configuration")
        macho = runpy.run_path(str(SCRIPTS / "record-build.py"))["macho"]
        executable = archive.read("SkyrimIOS.app/SkyrimIOS")
        if macho(executable)["signed_load_command"]:
            raise ValueError("Expected an unsigned iPhoneOS ARM64 host")
        if executable != linked_executable.read_bytes():
            raise ValueError("Archived executable differs from the linked app that passed the symbol checks")


def stage(source, destination, commit):
    if not re.fullmatch(r"[a-f0-9]{40}", commit):
        raise ValueError("Expected the complete source commit")
    names = (HOST, "result.json", *ABI_HEADERS)
    if any((source / name).is_symlink() or not (source / name).is_file() for name in names):
        raise ValueError("Incomplete public host artifacts")
    with (source / HOST).open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    expected = {
        "schema": 1, "source_commit": commit, "artifact_sha256": digest, "target": "arm64-apple-ios",
        "public_host_sdk_build": True, "private_guest_included": False, "game_executed": False,
        "physical_iphone_verified": False, "signed_for_installation": False, "classic_skse_experiment": True,
    }
    result = json.loads((source / "result.json").read_text(encoding="utf-8"))
    if result != expected or any(type(result[k]) is not type(v) for k, v in expected.items()):
        raise ValueError("Build result differs from the requested unsigned Classic source and archive")
    validate_app(source / HOST, source.parent / "build/SkyrimIOS.app/SkyrimIOS")
    for name in ABI_HEADERS:
        original = (ABI if name.startswith("PcGuest") else source.parent / "overlay/runtime") / name
        if (source / name).read_bytes() != original.read_bytes():
            raise ValueError(f"Public ABI header differs from the build input: {name}")
    # Publish the complete set together. A failed copy can be retried at this location.
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(destination)
    with tempfile.TemporaryDirectory(dir=destination.parent) as folder:
        transfer = Path(folder) / "transfer"
        transfer.mkdir()
        for name in names:
            shutil.copyfile(source / name, transfer / name)
        transfer.rename(destination)
    print(f"Unsigned Classic Lab host {commit[:12]}: {len(names)} files, ZIP SHA-256 {digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    try:
        stage(args.artifacts, args.destination, args.commit)
    except (OSError, ValueError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
        parser.exit(1, f"Public host artifact check failed: {error}\n")
