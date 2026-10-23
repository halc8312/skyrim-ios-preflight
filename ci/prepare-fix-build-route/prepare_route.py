"""Prepare a device-only derivative of the reviewed, pinned public CI scripts.

Preparation never builds or contacts a service. The generated scripts are only
for a later explicitly authorized Apple runner; no private input is accepted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent
HARNESS = 'harness="$(cd "$(dirname "$0")/.." && pwd)"'
LOOP = "for variant in device simulator; do\n"
AFTER = "\n# The V2 image is independently compiled from public authored sources."
STAGE_CALL = 'bash "$harness/ci/run-ios-owned-host-tests.sh" "$work" "$simde"'
ORIGIN = "https://github.com/halc8312/skyrim-ios-preflight.git"
ORIGINS = {ORIGIN, ORIGIN.removesuffix(".git"),
           "git@github.com:halc8312/skyrim-ios-preflight.git"}
SHA = re.compile(r"[0-9a-f]{40}\Z")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], check=True,
                            capture_output=True, timeout=15)
    return result.stdout.decode("utf-8", errors="strict").strip()


def inputs(repo, commit, pins):
    if not SHA.fullmatch(commit):
        raise ValueError("exact_lowercase_source_commit_required")
    if git(repo, "rev-parse", "HEAD") != commit:
        raise ValueError("source_commit_mismatch")
    if git(repo, "remote", "get-url", "origin") not in ORIGINS:
        raise ValueError("unexpected_repository_origin")
    if git(repo, "status", "--porcelain=v1", "--untracked-files=normal"):
        raise ValueError("source_checkout_not_clean")
    result = {}
    for name, expected in pins["files"].items():
        path = repo / name
        if any(p.is_symlink() for p in [path, *path.parents] if p != repo.parent):
            raise ValueError("source_symlink")
        if not path.resolve(strict=True).is_relative_to(repo):
            raise ValueError("source_escape")
        with path.open("rb") as stream:
            data = stream.read(1_000_001)
        if len(data) > 1_000_000 or digest(data) != expected:
            raise ValueError("reference_pin_mismatch:" + name)
        data.decode("utf-8", errors="strict")
        result[name] = data
    return result


def derive(setup, stages, repo, output):
    # Raw input SHA is checked before this text-only LF representation.
    setup, stages = [value.decode("utf-8").replace("\r\n", "\n")
                     for value in (setup, stages)]
    if setup.count(HARNESS) != 1 or stages.count(HARNESS) != 1:
        raise ValueError("harness_anchor")
    if setup.count(STAGE_CALL) != 1 or stages.count(LOOP) != 1 or stages.count(AFTER) != 1:
        raise ValueError("stage_anchor")
    before, rest = stages.split(LOOP)
    device, _omitted = rest.split(AFTER)
    if not device.endswith("\ndone\n"):
        raise ValueError("device_loop_boundary")
    selected = before + "for variant in device; do\n" + device
    # Keep all pre-build tests and the complete WGPU builder. Its existing
    # device+simulator policy/coexistence checks are intentionally not pruned.
    selected = selected.replace(HARNESS, "harness=" + shlex.quote(str(repo)))
    setup = setup.replace(HARNESS, "harness=" + shlex.quote(str(repo)))
    setup = setup.replace(STAGE_CALL,
                          "bash " + shlex.quote(str(output / "device-stage.sh")) + ' "$work" "$simde"')
    return {"preflight.sh": setup.encode(), "device-stage.sh": selected.encode()}


def prepare(repo, commit, output, pins=None):
    repo, output = Path(repo).resolve(strict=True), Path(output)
    pins = pins if pins is not None else json.loads((ROOT / "reference-pins.json").read_text())
    if output.exists() or output.is_symlink() or output.resolve().is_relative_to(repo):
        raise ValueError("fresh_separate_output_required")
    source = inputs(repo, commit, pins)
    scripts = derive(source["ci/run-ios-owned-host-preflight.sh"],
                     source["ci/run-ios-owned-host-tests.sh"], repo, output.resolve())
    # Check again before output publication; no mutation of the source checkout.
    if inputs(repo, commit, pins) != source:
        raise ValueError("source_changed_during_preparation")
    output.mkdir(parents=False, exist_ok=False)
    for name, data in scripts.items():
        (output / name).write_bytes(data)
    provenance = dict(schema=1, source_commit=commit,
                      reviewed_reference_commit=pins["reviewed_reference_commit"],
                      source_pins=pins["files"],
                      derived_scripts={n: digest(b) for n, b in scripts.items()},
                      recipe="original-setup-and-prebuild-tests-plus-original-device-loop",
                      source_commit_signature_authenticated=False,
                      source_pin_is_external_review_input=True,
                      simulator_xctest_executed=False, audio_sdk_executed=False,
                      game_input_present=False, signing_credentials_present=False)
    (output / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return provenance


def verify(repo, commit, output):
    repo, output = Path(repo).resolve(strict=True), Path(output).resolve(strict=True)
    provenance = json.loads((output / "source-provenance.json").read_text())
    pins = json.loads((ROOT / "reference-pins.json").read_text())
    inputs(repo, commit, pins)
    if provenance["source_commit"] != commit or provenance["source_pins"] != pins["files"]:
        raise ValueError("provenance_mismatch")
    for name, expected in provenance["derived_scripts"].items():
        if digest((output / name).read_bytes()) != expected:
            raise ValueError("derived_script_changed")
    built = repo / "out/owned-host"
    meta = json.loads((built / "device-ipa-metadata.json").read_text())
    ipa = built / "OwnedGameHost-0.1.0-unsigned.ipa"
    if ipa.stat().st_size > 256_000_000:
        raise ValueError("ipa_size_limit")
    data = ipa.read_bytes()
    if (meta["ipa_bytes"], meta["ipa_sha256"]) != (len(data), digest(data)):
        raise ValueError("ipa_metadata_mismatch")
    # The original packager remains the authority for image shape/signature
    # checks. Verify its recorded three full-image hashes against the ZIP too.
    expected = {"OwnedGameHost", "Frameworks/PrivateGuest.framework/PrivateGuest",
                "Frameworks/WgpuNative.framework/WgpuNative"}
    if (set(meta["images"]) != expected or meta.get("signature_commands_present") is not False
            or meta.get("public_fixture_only") is not True or meta.get("unsigned_device_ipa") is not True
            or meta.get("provisioning_profile_present") is not False):
        raise ValueError("package_contract")
    with zipfile.ZipFile(ipa) as archive:
        names = archive.namelist()
        info = archive.infolist()
        if (not 1 <= len(info) <= 2000 or sum(item.file_size for item in info) > 256_000_000
                or len(names) != len({name.casefold() for name in names})
                or any(not name.startswith("Payload/OwnedGameHost.app/")
                       or "\\" in name or ":" in name or any(p in ("", ".", "..") for p in name.split("/"))
                       for name in names)
                or any(item.extra or item.flag_bits & 1 for item in info)):
            raise ValueError("ipa_path_or_size_contract")
        if archive.testzip() is not None:
            raise ValueError("ipa_duplicate_or_crc")
        for name in expected:
            record = meta["images"][name]
            image = archive.read("Payload/OwnedGameHost.app/" + name)
            if (record["bytes"], record["sha256"]) != (len(image), digest(image)):
                raise ValueError("image_hash_mismatch")
    log = (built / "device-xcodebuild.log").read_text(errors="replace")
    if log.splitlines().count("** BUILD SUCCEEDED **") != 1 or "** BUILD FAILED **" in log:
        raise ValueError("device_build_terminal_missing")
    for name in ("public-host-tests.log", "public-xma-init-tests.log", "public-xma-device-tests.log",
                 "public-audio-observer-tests.log", "public-signed-policy-tests.log", "public-driver-tests.log",
                 "public-wgpu-build.log", "fixture-device-build.log", "host-device-build.log",
                 "device-ipa-audit.log"):
        if (built / name).stat().st_size == 0:
            raise ValueError("required_original_gate_log_empty")
    result = dict(schema=1, scope="public-device-build-only", source_commit=commit,
                  unsigned_device_build_succeeded=True, ipa_bytes=len(data), ipa_sha256=digest(data),
                  original_prebuild_unit_gates_invoked=True,
                  original_wgpu_device_and_simulator_policy_gates_invoked=True,
                  simulator_host_built=False, simulator_xctest_executed=False,
                  audio_sdk_executed=False, physical_iphone_verified=False,
                  game_executed=False, private_framework_packaged=False,
                  source_commit_signature_authenticated=False)
    (built / "build-only-scope.json").write_text(json.dumps(result, indent=2) + "\n")
    (built / "build-only-source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "verify"))
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    operation = prepare if args.operation == "prepare" else verify
    print(json.dumps(operation(args.repo, args.source_commit, args.output), sort_keys=True))


if __name__ == "__main__":
    main()
