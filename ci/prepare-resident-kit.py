"""Copy only an exact public text allowlist into a fresh SDK workspace."""
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
import tomllib

MANIFEST = "tools/ios-resident-guest-probe/apple-backend/source-allowlist.txt"
ALLOWLIST_SHA256 = "b0a0c046b02020c240bed722450fc628518c8f3fcbab4514c1bec837c57d7b7d"
SOURCE_MANIFEST_SHA256 = "fdb272beabab5007f03ab29b49a78a632db5c7e61d8c6cfb2ecedeeebb4b5533"
MEMBERS = ["crates/" + name for name in
           ("runtime-core", "kernel-hle", "vfs", "xex", "d3d-hle", "xma-decoder")]


def prepare(root, output):
    root = Path(root).resolve(strict=True)
    output = Path(output)
    if output.exists() or output.resolve().is_relative_to(root):
        raise ValueError("resident_output_must_be_fresh_and_separate")
    manifest = (root / MANIFEST).read_bytes()
    if len(manifest) > 32768 or hashlib.sha256(manifest).hexdigest() != ALLOWLIST_SHA256:
        raise ValueError("resident_manifest_size")
    frozen = (root / "public-source-manifest.json").read_bytes()
    if len(frozen) > 65536 or hashlib.sha256(frozen).hexdigest() != SOURCE_MANIFEST_SHA256:
        raise ValueError("resident_source_manifest_sha256")
    records = json.loads(frozen)["source_files"]
    names = manifest.decode("utf-8").splitlines()
    if not 1 <= len(names) <= 200 or names != sorted(set(names)) or MANIFEST not in names:
        raise ValueError("resident_manifest_order_or_count")
    if set(records) != set(names):
        raise ValueError("resident_source_manifest_set")
    files = []
    for name in names:
        parts = PurePosixPath(name)
        if parts.is_absolute() or str(parts) != name or any(p in ("..", ".", "target", "build", "inputs") for p in parts.parts) or "\\" in name or ":" in name:
            raise ValueError("resident_manifest_path")
        if not name.startswith(("skyrim-recomp-web/", "tools/ios-resident-guest-probe/",
                                "tools/windows-ios-guest-probe/owned-guest-abi/")):
            raise ValueError("resident_manifest_root")
        source = root / name
        for parent in [source, *source.parents]:
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError("resident_source_symlink")
        resolved = source.resolve(strict=True)
        if not resolved.is_relative_to(root) or not resolved.is_file():
            raise ValueError("resident_source_escape")
        with resolved.open("rb") as stream:
            data = stream.read(1_000_001)
        if len(data) > 1_000_000 or b"\0" in data:
            raise ValueError("resident_source_not_bounded_text")
        data.decode("utf-8")
        if records[name] != {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}:
            raise ValueError("resident_source_hash_mismatch")
        files.append((name, data))
    if sum(len(data) for _, data in files) > 10_000_000:
        raise ValueError("resident_sources_size")
    workspace_path = "skyrim-recomp-web/Cargo.toml"
    original = next(data for name, data in files if name == workspace_path)
    text = original.decode("utf-8")
    model = tomllib.loads(text)
    expected = copy.deepcopy(model)
    for key in ("members", "default-members"):
        expected["workspace"][key] = MEMBERS
        replacement = key + " = [\n" + "".join(f'    "{member}",\n' for member in MEMBERS) + "]"
        text, count = re.subn(r"(?ms)^" + key + r"\s*=\s*\[.*?^\]", replacement, text, count=1)
        if count != 1:
            raise ValueError("resident_workspace_anchor")
    if tomllib.loads(text) != expected:
        raise ValueError("resident_workspace_unexpected_change")
    output.mkdir(parents=False, exist_ok=False)
    for name, data in files:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(text.encode("utf-8") if name == workspace_path else data)
    if any((root / name).read_bytes() != data for name, data in files):
        raise ValueError("resident_source_changed_during_copy")
    if (root / MANIFEST).read_bytes() != manifest:
        raise ValueError("resident_manifest_changed_during_copy")
    print("resident_public_source_files=" + str(len(files)))
    print("resident_source_allowlist_sha256=" + hashlib.sha256(manifest).hexdigest())
    print("resident_frozen_source_manifest_sha256=" + hashlib.sha256(frozen).hexdigest())
    print("resident_staged_workspace_sha256=" + hashlib.sha256(text.encode("utf-8")).hexdigest())
    print("resident_workspace_members=6; original_sources_unchanged=true")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare-resident-kit.py PUBLIC_KIT NEW_OUTPUT")
    prepare(*sys.argv[1:])
