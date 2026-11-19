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
                   ".c", ".h", ".cc", ".cpp", ".hpp", ".m", ".mm", ".swift", ".metal",
                   ".plist", ".pbxproj", ".xcscheme", ".xcworkspacedata", ".xcconfig",
                   ".cmake", ".in", ".example", ".toml", ".csv", ".tsv", ".patch"}
ROOT_NAMES = {".gitignore", ".gitattributes", "LICENSE", "NOTICE", "CMakeLists.txt"}
AUTHORED_SHADER_SOURCES = {
    "experiments/d3d9-postprocess/two_tone.hlsl",
    "experiments/d3d9-postprocess/depth_view.hlsl",
    "experiments/d3d9-present-grade/warm_grade.glsl",
}
FORBIDDEN_SUFFIXES = {".exe", ".dll", ".bsa", ".ba2", ".esm", ".esp", ".esl",
                      ".ess", ".skse", ".nif", ".dds", ".hkx", ".pex", ".fuz",
                      ".wav", ".mp3", ".xwm", ".p12", ".p8", ".pem", ".key",
                      ".mobileprovision", ".ipa", ".zip", ".7z", ".tar", ".gz",
                      ".o", ".obj", ".a", ".dylib", ".so", ".bin"}
GENERATED_PATTERNS = (
    re.compile(r"\b(?:loc|sub|dword|byte|word|qword)_[0-9a-fA-F]{6,}\b"),
    re.compile(r"(?im)^\s*(?://|/\*|#)\s*(?:automatically )?generated (?:from|by) (?:tesv|skyrim|ghidra|ida|retdec|reccmp)"),
)


def check_patch(relative: PurePosixPath, source: str) -> list[str]:
    """Permit reviewed source diffs, not binary/game payloads hidden as patches."""
    errors = []
    if relative.parts[:2] != ("patches", "skydream"):
        errors.append("patch outside reviewed source patch directory")
    headers = re.findall(r"(?m)^diff --git a/(\S+) b/(\S+)$", source)
    if not headers or len(headers) != len(re.findall(r"(?m)^diff --git ", source)):
        errors.append("missing or unsupported patch headers")
    for before, after in headers:
        target = PurePosixPath(after)
        source_target = bool(target.parts) and target.parts[0] in {"runtime", "tools"} and target.suffix in {".c", ".h", ".cpp", ".py"}
        build_target = after == "desktop/CMakeLists.txt"
        if before != after or target.is_absolute() or ".." in target.parts or not (source_target or build_target):
            errors.append("patch target is not approved upstream source")
        if f"--- a/{before}\n+++ b/{after}\n" not in source:
            errors.append("patch file headers do not match target")
    if re.search(r"(?m)^(GIT binary patch|Binary files |literal [0-9]+|delta [0-9]+)", source):
        errors.append("binary patch payload")
    body = "\n".join(line[1:] if line[:1] in {"+", "-", " "} else line for line in source.splitlines())
    if any(pattern.search(body) for pattern in GENERATED_PATTERNS):
        errors.append("possible generated/decompiled game source in patch")
    return errors


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
    if (rel.name not in ROOT_NAMES and rel.suffix.lower() not in SOURCE_SUFFIXES
            and relative not in AUTHORED_SHADER_SOURCES):
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
    if rel.suffix.lower() == ".patch":
        errors.extend(check_patch(rel, source.replace("\r\n", "\n")))
    if rel.suffix.lower() in {".c", ".h", ".cc", ".cpp", ".hpp", ".m", ".mm", ".swift"}:
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
