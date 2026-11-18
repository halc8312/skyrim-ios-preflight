"""Read-only prerequisites for building a personally owned Skyrim copy.

This does not install tools, fetch sources, modify the game, start CI, or sign.
Run from the Python 3.12 virtual environment used for discovery/generation.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE_SHA256 = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605"
SOURCE_REVISION = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"


def command(argv):
    return subprocess.check_output(argv, stderr=subprocess.STDOUT, text=True,
                                   timeout=30).strip()


def inspect(generation_only=False, candidate=None):
    checks = []

    def add(name, good, detail):
        checks.append((name, bool(good), detail))

    add("Python", sys.version_info >= (3, 12), platform.python_version() + "; 3.12+ required")
    for name, expected in (("capstone", "5.0.9"), ("pefile", "2024.8.26")):
        try:
            actual = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            actual = "not installed in this interpreter"
        add(name, actual == expected, actual + "; required " + expected)
    for tool in ("git", "sh"):
        add(tool, shutil.which(tool), "available" if shutil.which(tool) else "not on PATH")
    source = ROOT / "vendor/skydream"
    try:
        head = command(["git", "-C", str(source), "rev-parse", "HEAD"])
        dirty = command(["git", "-c", "core.autocrlf=true", "-C", str(source),
                         "status", "--porcelain", "--untracked-files=all"])
        add("Pinned translator", head == SOURCE_REVISION and not dirty,
            "matching clean checkout" if head == SOURCE_REVISION and not dirty
            else "wrong revision or modified checkout; preserve it and use a fresh clone")
    except (OSError, subprocess.SubprocessError):
        add("Pinned translator", False, "fetch the exact revision using the build guide")
    if candidate is not None:
        try:
            with candidate.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            good = actual == CANDIDATE_SHA256
            add("Owned input", good, "reviewed English 1.9.32 candidate" if good
                else "unrecognized input; do not bypass the hash or use SE/AE/Japanese 1.9.31")
        except OSError:
            add("Owned input", False, "candidate file could not be read")
    else:
        add("Owned input", True, "not supplied; input compatibility has NOT been checked")
    if generation_only:
        add("Generation host", sys.platform != "win32",
            "POSIX required; use WSL for Windows discovery and generation")
    else:
        add("Apple build host", sys.platform == "darwin",
            "macOS with full Xcode required for the documented complete route")
        for tool in ("xcrun", "xcodebuild", "cmake", "perl", "flex", "bison", "gmake", "curl", "tar", "codesign", "security"):
            add(tool, shutil.which(tool), "available" if shutil.which(tool) else "not on PATH")
        if shutil.which("bison"):
            try:
                line = command(["bison", "--version"]).splitlines()[0]
                add("Bison version", bool(re.search(r"\b3\.\d+", line)), line + "; 3.x required")
            except (OSError, subprocess.SubprocessError):
                add("Bison version", False, "could not query version")
        if shutil.which("gmake"):
            try:
                line = command(["gmake", "--version"]).splitlines()[0]
                match = re.search(r"GNU Make (\d+)\.(\d+)", line)
                good = match and tuple(map(int, match.groups())) >= (4, 3)
                add("GNU Make version", good, line + "; 4.3+ required")
            except (OSError, subprocess.SubprocessError):
                add("GNU Make version", False, "could not query version")
        if sys.platform == "darwin" and shutil.which("xcrun"):
            try:
                sdk = command(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"])
                add("iPhoneOS SDK", int(sdk.split(".")[0]) >= 18,
                    sdk + "; existing host evidence used Xcode 26.6 / SDK 26.5")
            except (OSError, ValueError, subprocess.SubprocessError):
                add("iPhoneOS SDK", False, "select full Xcode and complete its first launch setup")
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, help="separately prepared owned TESV_unpacked.exe")
    parser.add_argument("--generation-only", action="store_true", help="check only POSIX discovery/generation prerequisites")
    args = parser.parse_args()
    checks = inspect(args.generation_only, args.candidate)
    for name, good, detail in checks:
        print(f"[{'OK' if good else 'MISSING'}] {name}: {detail}")
    print("No files changed. Signing authorization, game execution, and your iPhone are not validated by this check.")
    return 0 if all(good for _, good, _ in checks) else 2


if __name__ == "__main__":
    raise SystemExit(main())
