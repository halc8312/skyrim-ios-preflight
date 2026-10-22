#!/usr/bin/env python3
"""Create a fresh, pinned WgpuNative overlay; never edit a Cargo cache or checkout."""
from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tomllib
import urllib.request

SOURCE_COMMIT = "6aed50955d934ac36049ba8d002034841633ae02"
HEADERS_COMMIT = "673658bc2bd70ec39fc55ebe6bb0173cf6d0a603"
LOCK_SHA256 = "9f780c3e7e792c19ae22e708cf899eb177b5696c9ab61215c6ae235adcecbdb0"
HAL_VERSION = "29.0.3"
HAL_SHA256 = "31f8e1a9e7a8512f276f7c62e018c7fa8d60954303fed2e5750114332049193f"
DISPATCH_SHA256 = "1e0e367e4e7da84520dedcac1901e4da967309406d1e51017ae1abfb97adbd38"
PATCH_SHA256 = "adbc9479400a26015a4c3905d34e118b458c171b46759e42cad537c3f8016628"
PATCH_FILES = {
    "Cargo.toml": (
        "c832b132eb40c89920282a2fb7947bb392e2a8a150371b8047232fd14829f6d6",
        "ab1a67866e48e4978ae9445e3340308b103e0fa30344dfe861214ce334a50a9a"),
    "src/metal/surface.rs": (
        "748c5349bb68319d433ed2d7c2c27055e632f27196acfdfa857b279d8ed7bae2",
        "81572dc24ba0e57eb687aee2e73d0d578afa6172cdd6804b167507c00b5c7c22"),
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def git(source: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(source), *args])


def checked_tree(source: Path, commit: str) -> bytes:
    require(git(source, "rev-parse", "HEAD").decode().strip() == commit,
            "unexpected public source commit")
    require(not git(source, "status", "--porcelain", "--untracked-files=normal").strip(),
            "public source checkout is modified")
    # git archive applies checkout conversions; pin LF independently of the
    # developer's global Windows autocrlf configuration.
    return git(source, "-c", "core.autocrlf=false", "-c", "core.eol=lf",
               "archive", "--format=tar", "HEAD")


def archive_files(data: bytes, prefix: str | None = None) -> dict[str, bytes]:
    require(len(data) <= 32_000_000, "public archive is too large")
    files: dict[str, bytes] = {}
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            require(not path.is_absolute() and "\\" not in member.name
                    and ":" not in member.name and ".." not in path.parts
                    and str(path) == member.name.rstrip("/"), "unsafe archive path")
            require(member.isdir() or member.isfile(), "archive links are forbidden")
            if prefix is not None:
                require(path.parts and path.parts[0] == prefix, "unexpected crate archive root")
                path = PurePosixPath(*path.parts[1:])
            if member.isdir():
                continue
            name = str(path)
            require(name not in ("", ".") and name not in files, "duplicate archive file")
            total += member.size
            require(0 <= member.size <= 10_000_000 and total <= 64_000_000,
                    "expanded public archive is too large")
            stream = archive.extractfile(member)
            require(stream is not None, "missing archive member data")
            files[name] = stream.read(member.size + 1)
            require(len(files[name]) == member.size, "archive member size changed")
    require(bool(files), "empty public archive")
    return files


def rewrite_lock(data: bytes) -> bytes:
    require(sha(data) == LOCK_SHA256, "unexpected WgpuNative lockfile bytes")
    before = tomllib.loads(data.decode("utf-8"))
    expected = copy.deepcopy(before)
    hal, = [package for package in expected["package"] if package["name"] == "wgpu-hal"]
    dispatch, = [package for package in expected["package"] if package["name"] == "dispatch2"]
    require(hal["version"] == HAL_VERSION and hal["checksum"] == HAL_SHA256,
            "unexpected locked wgpu-hal")
    require(dispatch["version"] == "0.3.1" and dispatch["checksum"] == DISPATCH_SHA256,
            "unexpected locked dispatch2")
    del hal["source"]
    del hal["checksum"]
    require("dispatch2" not in hal["dependencies"], "dispatch2 dependency already present")
    hal["dependencies"] = sorted([*hal["dependencies"], "dispatch2"])
    text = data.decode("utf-8")
    pattern = r'(?ms)^\[\[package\]\]\nname = "wgpu-hal"\n.*?(?=^\[\[package\]\]|\Z)'
    block, = re.findall(pattern, text)
    replacement = re.sub(r'(?m)^(source|checksum) = "[^"\n]+"\n', "", block)
    anchor = ' "cfg_aliases",\n'
    require(replacement.count(anchor) == 1, "wgpu-hal dependency anchor changed")
    replacement = replacement.replace(anchor, anchor + ' "dispatch2",\n')
    result = text.replace(block, replacement, 1).encode("utf-8")
    require(tomllib.loads(result.decode("utf-8")) == expected,
            "lockfile changed beyond the audited local HAL override and dispatch2 edge")
    return result


def write_tree(root: Path, files: dict[str, bytes]) -> None:
    root.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(data)


def prepare(source: Path, output: Path, crate: bytes, patch: Path) -> dict:
    source = source.resolve(strict=True)
    output = output.resolve()
    require(not output.exists() and not output.is_relative_to(source)
            and not source.is_relative_to(output), "overlay must be fresh and separate")
    require(sha(crate) == HAL_SHA256, "wgpu-hal crate checksum mismatch")
    patch_bytes = patch.read_bytes()
    require(sha(patch_bytes) == PATCH_SHA256, "unexpected main-queue patch bytes")
    source_files = archive_files(checked_tree(source, SOURCE_COMMIT))
    header_source = source / "ffi/webgpu-headers"
    header_files = archive_files(checked_tree(header_source, HEADERS_COMMIT))
    require(git(source, "ls-tree", "HEAD", "ffi/webgpu-headers").decode().split()[2]
            == HEADERS_COMMIT, "unexpected header gitlink")
    hal_files = archive_files(crate, "wgpu-hal-" + HAL_VERSION)
    for name, (before, _) in PATCH_FILES.items():
        require(sha(hal_files[name]) == before, "HAL patch preimage changed: " + name)
    original_lock = source_files["Cargo.lock"]
    source_files["Cargo.lock"] = rewrite_lock(original_lock)
    manifest = source_files["Cargo.toml"].decode("utf-8")
    model = tomllib.loads(manifest)
    require("patch" not in model, "public manifest already has patch overrides")
    manifest += '\n[patch.crates-io]\nwgpu-hal = { path = "../wgpu-hal-29.0.3" }\n'
    expected = copy.deepcopy(model)
    expected["patch"] = {"crates-io": {"wgpu-hal": {"path": "../wgpu-hal-29.0.3"}}}
    require(tomllib.loads(manifest) == expected, "unexpected manifest change")
    source_files["Cargo.toml"] = manifest.encode("utf-8")
    for name, data in header_files.items():
        target = "ffi/webgpu-headers/" + name
        require(target not in source_files, "header tree conflicts with source archive")
        source_files[target] = data
    output.mkdir(parents=False, exist_ok=False)
    build_source = output / "source"
    patched_hal = output / ("wgpu-hal-" + HAL_VERSION)
    write_tree(build_source, source_files)
    write_tree(patched_hal, hal_files)
    for flags in (["--check"], []):
        subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf",
                        "-C", str(patched_hal), "apply", "--whitespace=error-all",
                        *flags, str(patch.resolve())], check=True)
    actual_names = {path.relative_to(patched_hal).as_posix()
                    for path in patched_hal.rglob("*") if path.is_file()}
    require(actual_names == set(hal_files), "patch changed the HAL file inventory")
    for name, original in hal_files.items():
        actual = (patched_hal / name).read_bytes()
        expected_sha = PATCH_FILES[name][1] if name in PATCH_FILES else sha(original)
        require(sha(actual) == expected_sha, "unexpected patched HAL contents: " + name)
    require(not git(source, "status", "--porcelain", "--untracked-files=normal").strip(),
            "upstream checkout changed while preparing overlay")
    report = dict(source_commit=SOURCE_COMMIT, headers_commit=HEADERS_COMMIT,
                  hal_version=HAL_VERSION, hal_crate_sha256=HAL_SHA256,
                  patch_sha256=PATCH_SHA256,
                  upstream_lock_sha256=sha(original_lock),
                  patched_lock_sha256=sha(source_files["Cargo.lock"]),
                  changed_hal_files={name: dict(before=hashes[0], after=hashes[1])
                                     for name, hashes in PATCH_FILES.items()},
                  upstream_checkout_unchanged=True, cargo_registry_unchanged=True,
                  ios_only=True, runtime_verified=False)
    (output / "ios-mainqueue-patch.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--crate", type=Path,
                        help="optional cached PUBLIC .crate; exact checksum is still required")
    args = parser.parse_args()
    if args.crate:
        with args.crate.open("rb") as stream:
            data = stream.read(8_000_001)
    else:
        url = "https://static.crates.io/crates/wgpu-hal/wgpu-hal-29.0.3.crate"
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read(8_000_001)
    require(len(data) <= 8_000_000, "crate download is too large")
    report = prepare(args.source, args.output, data,
                     Path(__file__).with_name("wgpu-hal-ios-mainqueue.patch"))
    print("WGPU_IOS_MAINQUEUE_PATCH " + json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
