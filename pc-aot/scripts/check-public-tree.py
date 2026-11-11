#!/usr/bin/env python3
"""Fail-closed source preflight, not a license or secret-leak proof.

Checks tracked files AND nonignored untracked files. Ignored private inputs are
not opened. Force-added private files are rejected. No network or dependencies.
"""
from pathlib import Path, PurePosixPath
import argparse
import re
import subprocess
import sys

PRIVATE_PARTS = {"local", "private", "generated", "vendor", "build", "artifacts",
                 "data", "saves", "mods", "signing", "deriveddata", "__pycache__"}
SOURCE_SUFFIXES = {".md", ".txt", ".json", ".yaml", ".yml", ".py", ".sh", ".ps1",
                   ".c", ".h", ".cc", ".cpp", ".hpp", ".m", ".mm", ".metal",
                   ".plist", ".pbxproj", ".xcscheme", ".xcworkspacedata", ".xcconfig",
                   ".cmake", ".in", ".example", ".toml", ".csv", ".tsv"}
ROOT_NAMES = {".gitignore", ".gitattributes", "LICENSE", "NOTICE", "CMakeLists.txt"}
FORBIDDEN_SUFFIXES = {".exe", ".dll", ".bsa", ".ba2", ".esm", ".esp", ".esl",
                      ".ess", ".skse", ".nif", ".dds", ".hkx", ".pex", ".fuz",
                      ".wav", ".mp3", ".xwm", ".p12", ".p8", ".pem", ".key",
                      ".mobileprovision", ".ipa", ".zip", ".7z", ".tar", ".gz",
                      ".o", ".obj", ".a", ".dylib", ".so", ".bin"}
GENERATED_PATTERNS = (
    re.compile(r"\b(?:loc|sub|dword|byte|word|qword)_[0-9a-fA-F]{6,}\b"),
    re.compile(r"(?im)^\s*(?://|/\*|#)\s*(?:automatically )?generated (?:from|by) (?:tesv|skyrim|ghidra|ida|retdec|reccmp)"),
)


def check_file(relative: str, path: Path) -> list[str]:
    rel = PurePosixPath(relative)
    errors = []
    if any(part.lower() in PRIVATE_PARTS for part in rel.parts):
        errors.append("private/generated directory")
    if path.is_symlink():
        return errors + ["symlink"]
    if not path.is_file():
        return errors + ["not a regular file (including submodule)"]
    if rel.suffix.lower() in FORBIDDEN_SUFFIXES:
        errors.append("binary, game data, signing material, or archive extension")
    if rel.name not in ROOT_NAMES and rel.suffix.lower() not in SOURCE_SUFFIXES:
        errors.append("not an approved source-text type")
    if rel.name.startswith(".env") and rel.name != ".env.example":
        errors.append("environment secrets")
    if path.stat().st_size > 2 * 1024 * 1024:
        return errors + ["oversized source file; manual review required"]
    data = path.read_bytes()
    if data.startswith((b"MZ", b"\x7fELF", b"BSA\x00", b"BTDX", b"TES4", b"PK\x03\x04",
                        b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf")) or b"\x00" in data:
        return errors + ["binary payload or binary magic"]
    try:
        source = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return errors + ["not UTF-8 source text"]
    if re.search(r"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----", source):
        errors.append("private-key marker")
    if rel.suffix.lower() in {".c", ".h", ".cc", ".cpp", ".hpp", ".m", ".mm"}:
        if any(pattern.search(source) for pattern in GENERATED_PATTERNS):
            errors.append("possible generated/decompiled game source")
    return errors


def scan(root: Path) -> list[str]:
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached",
                             "--others", "--exclude-standard"], check=True, capture_output=True)
    errors = []
    for entry in sorted(set(result.stdout.decode("utf-8").split("\x00")) - {""}):
        for problem in check_file(entry, root / entry):
            errors.append(f"{entry}: {problem}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = scan(args.root.resolve())
    if errors:
        print("Source preflight rejected:\n" + "\n".join(errors), file=sys.stderr)
        return 1
    print("Source preflight passed (tracked + nonignored files; ignored private inputs not read).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
