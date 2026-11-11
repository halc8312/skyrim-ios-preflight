"""Copy only an exact public text allowlist into a fresh SDK workspace."""
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
import tomllib

MANIFEST = "source-allowlist.txt"
ALLOWLIST_SHA256 = "f721de77fb81291a70c1dbd7e87a26d914da41c1268e1843228c03cab7c93f73"
SOURCE_MANIFEST_SHA256 = "5f0608c03b87694d791adf9f45766f171b5d7bc1b0985f363e2c23464a31c498"
MEMBERS = ["crates/" + name for name in
           ("d3d-hle", "gpu-webgpu", "kernel-hle", "runtime-core", "vfs",
            "xenos-edram", "xenos-gfx", "xenos-render", "xenos-shader", "xenos-state",
            "xenos-texture", "xenos-texture-cache", "xex", "xma-decoder")]


def allowed_source_root(name):
    return (name in (MANIFEST, "tools/windows-ios-guest-probe/audit_private_guest_symbols.py",
                     "tools/native-metal-engine/fixtures/constants.rs")
            or name.startswith(("skyrim-recomp-web/", "tools/ios-resident-guest-probe/",
                                "tools/ios-owned-game-host/", "tools/ios-signed-image-policy/",
                                "tools/windows-ios-guest-probe/owned-guest-abi/",
                                "tools/native-metal-engine/bridge/", "tools/native-metal-engine/gpu-hal/",
                                "tools/native-metal-engine/vendor/naga-29.0.3/",
                                "tools/native-metal-engine/vendor/wgpu-hal-29.0.3/")))


def prepare(root, output):
    root = Path(root).resolve(strict=True)
    output = Path(output)
    if output.exists() or output.resolve().is_relative_to(root):
        raise ValueError("owned_host_output_must_be_fresh_and_separate")
    manifest = (root / MANIFEST).read_bytes()
    if len(manifest) > 65536 or hashlib.sha256(manifest).hexdigest() != ALLOWLIST_SHA256:
        raise ValueError("owned_host_manifest_size")
    frozen = (root / "public-source-manifest.json").read_bytes()
    if len(frozen) > 160000 or hashlib.sha256(frozen).hexdigest() != SOURCE_MANIFEST_SHA256:
        raise ValueError("owned_host_source_manifest_sha256")
    records = json.loads(frozen)["source_files"]
    names = manifest.decode("utf-8").splitlines()
    if (not 1 <= len(names) <= 900 or names != sorted(set(names)) or MANIFEST not in names
            or len({name.casefold() for name in names}) != len(names)):
        raise ValueError("owned_host_manifest_order_or_count")
    if set(records) != set(names):
        raise ValueError("owned_host_source_manifest_set")
    files = []
    for name in names:
        parts = PurePosixPath(name)
        if parts.is_absolute() or str(parts) != name or any(p in ("..", ".", "target", "build", "inputs", "diagnostics", "Frameworks", "Libraries", "Resources") for p in parts.parts) or "\\" in name or ":" in name:
            raise ValueError("owned_host_manifest_path")
        if not allowed_source_root(name):
            raise ValueError("owned_host_manifest_root")
        source = root / name
        for parent in [source, *source.parents]:
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError("owned_host_source_symlink")
        resolved = source.resolve(strict=True)
        if not resolved.is_relative_to(root) or not resolved.is_file():
            raise ValueError("owned_host_source_escape")
        with resolved.open("rb") as stream:
            data = stream.read(1_000_001)
        if len(data) > 1_000_000 or b"\0" in data:
            raise ValueError("owned_host_source_not_bounded_text")
        data.decode("utf-8")
        if records[name] != {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}:
            raise ValueError("owned_host_source_hash_mismatch")
        files.append((name, data))
    if sum(len(data) for _, data in files) > 20_000_000:
        raise ValueError("owned_host_sources_size")
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
            raise ValueError("owned_host_workspace_anchor")
    if tomllib.loads(text) != expected:
        raise ValueError("owned_host_workspace_unexpected_change")
    output.mkdir(parents=False, exist_ok=False)
    for name, data in files:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(text.encode("utf-8") if name == workspace_path else data)
    if any((root / name).read_bytes() != data for name, data in files):
        raise ValueError("owned_host_source_changed_during_copy")
    if (root / MANIFEST).read_bytes() != manifest:
        raise ValueError("owned_host_manifest_changed_during_copy")
    if (root / "public-source-manifest.json").read_bytes() != frozen:
        raise ValueError("owned_host_source_manifest_changed_during_copy")
    print("owned_host_public_source_files=" + str(len(files)))
    print("owned_host_source_allowlist_sha256=" + hashlib.sha256(manifest).hexdigest())
    print("owned_host_frozen_source_manifest_sha256=" + hashlib.sha256(frozen).hexdigest())
    print("owned_host_staged_workspace_sha256=" + hashlib.sha256(text.encode("utf-8")).hexdigest())
    print("owned_host_workspace_members=14; original_sources_unchanged=true")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare-owned-host-kit.py PUBLIC_KIT NEW_OUTPUT")
    prepare(*sys.argv[1:])
