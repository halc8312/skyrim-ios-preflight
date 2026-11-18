#!/usr/bin/env python3
"""Copy reviewed vanilla game inputs to a fresh private Skyrim folder.

No original, save, profile, mod, signing file, or device is modified. The sibling
owned-data-manifest.json is published only after source stability and complete
destination readback checks. A failed attempt remains incomplete for inspection.
DLC copying is opt-in and does not enable plugins or change a load order.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE_SHA256 = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605"
CANDIDATE_BYTES = 17_462_064
MAX_FILE_BYTES = 16 * 1024 ** 3
MAX_TOTAL_BYTES = 32 * 1024 ** 3
MANIFEST = "owned-data-manifest.json"
# Exact vanilla filenames present in the verified English 1.9.32 input. These
# are inputs, not an assertion that an arbitrary same-named BSA/ESM is unmodified.
REQUIRED_FILES = (
    "Data/Skyrim.esm", "Data/Update.esm", "Data/Update.bsa",
    *(f"Data/Skyrim - {name}.bsa" for name in (
        "Animations", "Interface", "Meshes", "Misc", "Shaders", "Sounds",
        "Textures", "Voices", "VoicesExtra")),
    *(f"Data/Strings/{name}_English.{suffix}" for name in ("Skyrim", "Update")
      for suffix in ("DLSTRINGS", "ILSTRINGS", "STRINGS")),
    "Data/Interface/Translate_ENGLISH.txt", "Data/Video/BGS_Logo.bik",
    "Skyrim_default.ini", "VeryHigh.ini", "high.ini", "low.ini", "medium.ini",
)
DLC_NAMES = ("Dawnguard", "HearthFires", "Dragonborn")


class PreparationError(ValueError):
    pass


def _absolute(path: Path) -> Path:
    path = Path(path).expanduser()
    if ".." in path.parts:
        raise PreparationError("Parent traversal is not accepted; use a direct path")
    return path.absolute()


def _is_link(info: os.stat_result) -> bool:
    # Windows junctions and other reparse points must not bypass the boundary.
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & 0x400)


def _real(path: Path, *, directory: bool) -> os.stat_result:
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        info = current.lstat()
        if _is_link(info):
            raise PreparationError("Symbolic links and reparse points are not accepted")
        if current != path and not stat.S_ISDIR(info.st_mode):
            raise PreparationError("A path parent is not a directory")
    info = path.lstat()
    wanted = stat.S_ISDIR if directory else stat.S_ISREG
    if _is_link(info) or not wanted(info.st_mode):
        raise PreparationError("Expected a real directory" if directory else "Expected a regular file")
    return info


def _fingerprint(info: os.stat_result) -> tuple[int, ...]:
    # Windows stat infers executable permission bits from the filename whereas
    # fstat does not; its historical ctime is also not POSIX change time. Compare
    # file type and the stable handle identity/size/mtime on both platforms, and
    # add true metadata change time where the OS supplies that semantics.
    return (info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode), info.st_size,
            info.st_mtime_ns, info.st_ctime_ns if os.name != "nt" else 0)


def _lookup(root: Path, relative: str, *, optional: bool = False) -> Path | None:
    rel = PurePosixPath(relative)
    if rel.is_absolute() or ".." in rel.parts or "\\" in relative or ":" in relative:
        raise PreparationError("Invalid fixed input filename")
    current = root
    for index, part in enumerate(rel.parts):
        _real(current, directory=True)
        entries: dict[str, Path] = {}
        for item in current.iterdir():
            key = item.name.casefold()
            if key in entries:
                raise PreparationError("Ambiguous case-insensitive source names")
            entries[key] = item
        found = entries.get(part.casefold())
        if found is None:
            if optional:
                return None
            raise PreparationError("Missing required vanilla input: " + relative)
        _real(found, directory=index < len(rel.parts) - 1)
        current = found
    if not current.is_relative_to(root):
        raise PreparationError("Source escapes its game directory")
    return current


def _open_read(path: Path, expected: tuple[int, ...]):
    _real(path, directory=False)
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    flags |= getattr(os, "O_BINARY", 0)
    fd = os.open(path, flags)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or _fingerprint(info) != expected:
            raise PreparationError("Input changed before reading")
        return os.fdopen(fd, "rb")
    except BaseException:
        os.close(fd)
        raise


def _read_hash(path: Path, expected: tuple[int, ...], max_bytes: int) -> str:
    digest = hashlib.sha256()
    size = 0
    with _open_read(path, expected) as source:
        while chunk := source.read(1024 * 1024):
            size += len(chunk)
            if size > max_bytes:
                raise PreparationError("File grew beyond its size bound")
            digest.update(chunk)
        if size != expected[3] or _fingerprint(os.fstat(source.fileno())) != expected:
            raise PreparationError("Input changed while reading")
    if _fingerprint(_real(path, directory=False)) != expected:
        raise PreparationError("Input changed after reading")
    return digest.hexdigest()


def _mkdirs(path: Path, boundary: Path) -> None:
    if not path.is_relative_to(boundary):
        raise PreparationError("Directory escapes the private boundary")
    _real(boundary, directory=True)
    current = boundary
    for part in path.relative_to(boundary).parts:
        current /= part
        try:
            current.mkdir()
        except FileExistsError:
            pass
        _real(current, directory=True)


def _copy_source(source: Path, destination: Path, expected: tuple[int, ...]) -> str:
    digest = hashlib.sha256()
    size = 0
    with _open_read(source, expected) as original, destination.open("xb") as copied:
        while chunk := original.read(1024 * 1024):
            size += len(chunk)
            if size > expected[3]:
                raise PreparationError("Input grew during copying")
            copied.write(chunk)
            digest.update(chunk)
        copied.flush()
        os.fsync(copied.fileno())
        if size != expected[3] or _fingerprint(os.fstat(original.fileno())) != expected:
            raise PreparationError("Input changed during copying")
    return digest.hexdigest()


def prepare_data(game_dir: Path, candidate: Path, output: Path, *,
                 root: Path = ROOT, include_dlc: bool = False,
                 required_files: tuple[str, ...] = REQUIRED_FILES,
                 candidate_sha256: str = CANDIDATE_SHA256,
                 candidate_bytes: int = CANDIDATE_BYTES,
                 max_file_bytes: int = MAX_FILE_BYTES,
                 max_total_bytes: int = MAX_TOTAL_BYTES) -> dict:
    """Library entry permits synthetic fixtures; CLI never exposes identity overrides."""
    game, candidate, output, root = map(_absolute, (game_dir, candidate, output, root))
    _real(root, directory=True)
    private = root / "private"
    if not output.is_relative_to(private) or output.parent == private or output.name != "Skyrim":
        raise PreparationError("Output must be a fresh private/<run>/Skyrim directory")
    _real(game, directory=True)
    if output.is_relative_to(game):
        raise PreparationError("Output must not be inside the original game directory")
    # Inspect existing parents before any creation; do not follow links while
    # resolving either the destination or the completion-marker location.
    existing = output.parent
    while not os.path.lexists(existing):
        existing = existing.parent
    _real(existing, directory=True)
    if os.path.lexists(output) or os.path.lexists(output.parent / MANIFEST):
        raise PreparationError("Output or completion manifest already exists")

    plan: list[tuple[str, Path, tuple[int, ...]]] = []
    seen: set[str] = set()
    for name in required_files:
        if name.casefold() in seen or name.casefold() == "tesv_unpacked.exe":
            raise PreparationError("Duplicate fixed input filename")
        seen.add(name.casefold())
        source = _lookup(game, name)
        plan.append((name, source, _fingerprint(_real(source, directory=False))))
    dlc = []
    if include_dlc:
        for name in DLC_NAMES:
            pair = [(f"Data/{name}.{ext}", _lookup(game, f"Data/{name}.{ext}", optional=True))
                    for ext in ("esm", "bsa")]
            if any(p is not None for _, p in pair) and not all(p is not None for _, p in pair):
                raise PreparationError("Incomplete vanilla DLC pair: " + name)
            if pair[0][1] is not None:
                dlc.append(name)
                for relative, source in pair:
                    if relative.casefold() in seen:
                        raise PreparationError("Duplicate DLC input")
                    seen.add(relative.casefold())
                    plan.append((relative, source, _fingerprint(_real(source, directory=False))))
    fingerprint = _fingerprint(_real(candidate, directory=False))
    if fingerprint[3] != candidate_bytes:
        raise PreparationError("Candidate size does not match the reviewed English 1.9.32 input")
    plan.append(("TESV_unpacked.exe", candidate, fingerprint))
    if any(not 0 < item[2][3] <= max_file_bytes for item in plan):
        raise PreparationError("Input file is empty or exceeds the size limit")
    total = sum(item[2][3] for item in plan)
    if total > max_total_bytes:
        raise PreparationError("Input package exceeds the total size limit")
    if _read_hash(candidate, fingerprint, max_file_bytes) != candidate_sha256:
        raise PreparationError("Candidate SHA256 does not match the reviewed English 1.9.32 input")

    _mkdirs(private, root)
    _mkdirs(output.parent, private)
    output.mkdir()  # Exclusive; an existing directory is never reused.
    records = []
    for relative, source, original in plan:
        destination = output.joinpath(*PurePosixPath(relative).parts)
        _mkdirs(destination.parent, output)
        digest = _copy_source(source, destination, original)
        if relative == "TESV_unpacked.exe" and digest != candidate_sha256:
            raise PreparationError("Candidate changed after identity verification")
        records.append({"path": relative, "bytes": original[3], "sha256": digest})
    # Read every copy back and check all originals again before completion.
    for (relative, source, original), record in zip(plan, records):
        if _fingerprint(_real(source, directory=False)) != original:
            raise PreparationError("An original changed before package completion")
        destination = output.joinpath(*PurePosixPath(relative).parts)
        copied = _fingerprint(_real(destination, directory=False))
        if copied[3] != original[3] or _read_hash(destination, copied, max_file_bytes) != record["sha256"]:
            raise PreparationError("Destination readback differs from its original")
    result = {"schema": 1, "complete": True, "candidate_sha256": candidate_sha256,
              "files": records, "total_bytes": total, "dlc_copied": dlc,
              "dlc_activation_configured": False, "game_executed": False,
              "vanilla_asset_identity_verified": False,
              "notes": ["Use an unmodified owned English Skyrim 2011 1.9.32 installation.",
                        "Only the executable has a pinned known identity; asset hashes prove copy integrity.",
                        "DLC copying does not create plugins.txt or configure a load order."]}
    pending = output.parent / (".owned-data-" + uuid.uuid4().hex + ".pending")
    with pending.open("xb") as stream:
        stream.write((json.dumps(result, indent=2) + "\n").encode("utf-8"))
        stream.flush()
        os.fsync(stream.fileno())
    # Hard-link publication is atomic and refuses to replace a concurrent result.
    # If unsupported, fail explicitly and retain the pending evidence.
    os.link(pending, output.parent / MANIFEST)
    try:
        pending.unlink()
    except OSError:
        # Publication has already succeeded; an extra link must not turn a valid
        # completed package into a false "no completion published" CLI report.
        print("Completed package is intact; temporary manifest cleanup failed.", file=sys.stderr)
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True,
                        help="Unmodified owned English Skyrim 2011 installation")
    parser.add_argument("--candidate", type=Path, required=True,
                        help="Independently prepared, reviewed English 1.9.32 executable")
    parser.add_argument("--output", type=Path, required=True,
                        help="Fresh path inside this checkout: private/<run>/Skyrim")
    parser.add_argument("--include-dlc", action="store_true",
                        help="Copy present complete vanilla DLC ESM/BSA pairs; does not enable them")
    args = parser.parse_args(argv)
    try:
        result = prepare_data(args.game_dir, args.candidate, args.output, include_dlc=args.include_dlc)
    except (OSError, PreparationError) as error:
        parser.exit(1, f"Preparation failed: {error}\nNo successful completion was published by this attempt.\n")
    print(json.dumps({"complete": True, "files": len(result["files"]),
                      "bytes": result["total_bytes"],
                      "manifest": str(_absolute(args.output).parent / MANIFEST)}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
