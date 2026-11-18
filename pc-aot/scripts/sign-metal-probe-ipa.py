#!/usr/bin/env python3
"""Local, pinned, one-image MetalProbe signing. No secret unseal or installation.

Requires the existing reviewed local zsign and Apple identity verification files.
Inputs remain private; the caller supplies an already-authorized temporary key.
Only the fixed authored probe is accepted, with no game or guest dependencies.
"""
from __future__ import annotations
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import struct
import sys
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = "org.skyrimios.metalprobe"
APP_VERSION = "3"
APP = "Payload/MetalProbe.app"
UNSIGNED_FILES = {"MetalProbe", "Info.plist", "fixture.metal", "phone_fixture.metal", "BuildInfo.json"}
SIGNED_FILES = UNSIGNED_FILES | {"embedded.mobileprovision", "_CodeSignature/CodeResources"}
FIXTURE_SHA256 = "e45755523aabf64eb236c01a77ae220e271b4ae6d0eabcf120d9a80cabbada87"
PHONE_FIXTURE_SHA256 = "26ed56a8f4d264965eb624911d42cbf925f1329f0f103d58f683fe1c52d07ad6"
SIGNER_HELPER_SHA256 = "2238760e2f997174797d3e9adf1afc504136165a4167a11162a6937d36953827"
PROFILE_HELPER_SHA256 = "9ba5eb3f3f6d8b425cbb07cf15667ca860c184bcd683ca83e8c741008a6bff84"
CERTIFICATE_HELPER_SHA256 = "12ce42f38cb222b00fdd16c32fcf121d33a5703e2ef798308a7e4bff4cd9c5ea"
MAX_IPA = 32 << 20
PROVENANCE_INPUTS = {
    "experiments/metal-compile/runtime.mm",
    "experiments/metal-compile/runtime_host.h",
    "experiments/metal-compile/fixture.metal",
    "experiments/metal-compile/phone_fixture.metal",
    "experiments/metal-compile/phone_workloads.h",
    "experiments/metal-compile/ios/main.mm",
    "experiments/metal-compile/ios/ProbeRunStore.h",
    "experiments/metal-compile/ios/ProbeRunStore.mm",
    "experiments/metal-compile/ios/Info.plist",
    "experiments/metal-compile/offline.py",
    "scripts/build-metal-probe-ios.sh",
    "scripts/build-metal-probe-ios.py",
}


class Refusal(ValueError):
    pass


def need(condition, reason):
    if not condition:
        raise Refusal(reason)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def plain(path):
    path = Path(path)
    need(path.is_absolute() and ".." not in path.parts, "absolute_plain_path_required")
    need(not any(p.is_symlink() for p in (path, *path.parents)), "linked_path_refused")
    return path


class Input:
    def __init__(self, path, expected, limit):
        self.path = plain(path)
        need(re.fullmatch("[0-9a-f]{64}", expected or "") is not None, "expected_input_hash_required")
        self.fd = os.open(self.path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        self.closed = False
        try:
            info = os.fstat(self.fd)
            need(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= limit, "input_size_or_type")
            self.identity = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
            self.expected = expected
            self.limit = limit
            need(digest(self.bytes()) == expected, "input_hash_mismatch")
        except BaseException:
            self.close()
            raise

    def bytes(self):
        os.lseek(self.fd, 0, os.SEEK_SET)
        chunks, count = [], 0
        while chunk := os.read(self.fd, 65536):
            count += len(chunk)
            need(count <= self.limit, "input_grew")
            chunks.append(chunk)
        return b"".join(chunks)

    def recheck(self):
        plain(self.path)
        info = self.path.stat()
        need((info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns) == self.identity,
             "input_changed_during_signing")
        need(digest(self.bytes()) == self.expected, "input_changed_during_signing")

    def close(self):
        if not self.closed:
            os.close(self.fd)
            self.closed = True


def load_pinned(path, expected, name):
    path = plain(path)
    raw = path.read_bytes()
    need(digest(raw) == expected, "reviewed_helper_pin_changed")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    exec(compile(raw, str(path), "exec"), module.__dict__)
    return module


def device_image_contract(raw):
    """Narrow app platform gate; the pinned signer also checks full image layout."""
    need(len(raw) >= 32, "truncated_device_image")
    magic, cpu, subtype, kind, count, size, _, reserved = struct.unpack_from("<8I", raw)
    need((magic, cpu, subtype, kind, reserved) == (0xFEEDFACF, 0x0100000C, 0, 2, 0),
         "plain_arm64_executable_required")
    end, offset, build, identifier = 32 + size, 32, None, None
    need(0 < count <= 512 and end <= len(raw), "image_load_command_bounds")
    for _ in range(count):
        need(offset + 8 <= end, "image_load_command_bounds")
        command, length = struct.unpack_from("<2I", raw, offset)
        need(length >= 8 and length % 8 == 0 and offset + length <= end, "image_load_command_bounds")
        if command == 0x32:
            need(length >= 24 and build is None, "image_build_version")
            build = struct.unpack_from("<4I", raw, offset + 8)
            need(length == 24 + 8 * build[3], "image_build_version")
        elif command == 0x1B:
            need(length == 24 and identifier is None, "image_uuid")
            identifier = raw[offset + 8:offset + length]
        elif command in (0x1D, 0x24, 0x25, 0x2F, 0x30):
            raise Refusal("unsigned_modern_device_image_required")
        elif command in (0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023):
            need(length >= 24, "dylib_load_bounds")
            name_offset = struct.unpack_from("<I", raw, offset + 8)[0]
            need(24 <= name_offset < length, "dylib_load_bounds")
            encoded = raw[offset + name_offset:offset + length]
            need(b"\0" in encoded, "dylib_name_unterminated")
            name = encoded.split(b"\0", 1)[0]
            need(name.startswith((b"/usr/lib/", b"/System/Library/Frameworks/"))
                 and b".." not in name.split(b"/"), "non_system_dependency_refused")
        offset += length
    need(offset == end and identifier is not None and build is not None
         and build[0] == 2 and build[1] == 26 << 16 and build[2] >= 26 << 16,
         "ios26_device_image_required")


def bundle_contract(members):
    need(set(members) == UNSIGNED_FILES, "fixed_unsigned_file_set_required")
    info = plistlib.loads(members["Info.plist"])
    need(info.get("CFBundleIdentifier") == BUNDLE and info.get("CFBundleExecutable") == "MetalProbe"
         and info.get("CFBundleVersion") == APP_VERSION
         and info.get("CFBundlePackageType") == "APPL" and info.get("MinimumOSVersion") == "26.0"
         and info.get("CFBundleSupportedPlatforms") == ["iPhoneOS"] and info.get("UIDeviceFamily") == [1],
         "isolated_device_probe_identity_required")
    need(digest(members["fixture.metal"]) == FIXTURE_SHA256, "authored_fixture_mismatch")
    need(digest(members["phone_fixture.metal"]) == PHONE_FIXTURE_SHA256, "authored_phone_fixture_mismatch")
    provenance = json.loads(members["BuildInfo.json"])
    need(provenance.get("bundle_id") == BUNDLE and provenance.get("simulator") is False,
         "device_build_provenance_required")
    inputs = provenance.get("inputs")
    need(isinstance(inputs, dict), "source_provenance_set_mismatch")
    need(inputs.get("experiments/metal-compile/fixture.metal") == FIXTURE_SHA256,
         "fixture_provenance_mismatch")
    need(inputs.get("experiments/metal-compile/phone_fixture.metal") == PHONE_FIXTURE_SHA256,
         "phone_fixture_provenance_mismatch")
    workloads = inputs.get("experiments/metal-compile/phone_workloads.h")
    need(isinstance(workloads, str) and re.fullmatch("[0-9a-f]{64}", workloads),
         "phone_workloads_provenance_missing_or_invalid")
    need(set(inputs) == PROVENANCE_INPUTS, "source_provenance_set_mismatch")
    need(all(isinstance(value, str) and re.fullmatch("[0-9a-f]{64}", value) for value in inputs.values()),
         "source_provenance_hash_invalid")
    need(all(provenance.get(k) is False for k in ("signed_for_device", "installed", "physical_iphone_verified",
         "gpu_execution_verified", "metal4_runtime_verified", "gameplay_improvement_verified")),
         "unsigned_build_must_not_claim_runtime_success")
    device_image_contract(members["MetalProbe"])
    return info


def read_unsigned(raw):
    need(type(raw) is bytes and 0 < len(raw) <= MAX_IPA, "ipa_bytes_snapshot_required")
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        need(0 < len(archive.infolist()) <= 8, "ipa_member_count")
        seen, members, total = set(), {}, 0
        for item in archive.infolist():
            name = item.filename
            need(name == item.orig_filename and name not in seen, "duplicate_or_normalized_zip_path")
            seen.add(name)
            need(name in {APP + "/" + f for f in UNSIGNED_FILES}, "unexpected_zip_member")
            mode = item.external_attr >> 16
            need(stat.S_IFMT(mode) in (0, stat.S_IFREG) and not mode & 0o7000,
                 "zip_special_or_privileged_file")
            need(not item.is_dir() and not item.flag_bits & 1 and not item.extra
                 and item.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), "unsupported_zip_metadata")
            total += item.file_size
            need(0 < item.file_size <= 16 << 20 and total <= MAX_IPA, "zip_expansion_limit")
            raw = archive.read(item)
            need(len(raw) == item.file_size, "zip_member_length")
            members[name[len(APP) + 1:]] = raw
        bundle_contract(members)
        return members


def read_pinned_unsigned(pin):
    raw = pin.bytes()
    need(digest(raw) == pin.expected, "input_changed_during_signing")
    return read_unsigned(raw)


def profile_contract(profile, certificate_der, device, team, now=None):
    """Called only after Apple CMS+chain and modern/legacy agreement checks."""
    need(isinstance(team, str) and re.fullmatch("[A-Z0-9]{10}", team), "certificate_team_required")
    need(profile.get("TeamIdentifier") == [team] and profile.get("ApplicationIdentifierPrefix") == [team],
         "profile_team_mismatch")
    need(isinstance(device, str) and re.fullmatch("[A-Za-z0-9-]{8,128}", device), "device_record_invalid")
    devices = profile.get("ProvisionedDevices")
    need(isinstance(devices, list) and device in devices and len(devices) == len(set(devices)),
         "registered_device_not_authorized")
    need(profile.get("ProvisionsAllDevices", False) is False and "iOS" in profile.get("Platform", []),
         "registered_ios_profile_required")
    need(certificate_der in profile.get("DeveloperCertificates", []), "profile_certificate_mismatch")
    now = (now or datetime.now(timezone.utc)).astimezone(timezone.utc)
    def date(value):
        need(isinstance(value, datetime), "profile_validity_missing")
        return value.replace(tzinfo=timezone.utc) if value.tzinfo is None else value.astimezone(timezone.utc)
    need(date(profile.get("CreationDate")) <= now < date(profile.get("ExpirationDate")), "profile_expired_or_future")
    ent = profile.get("Entitlements")
    base = {"application-identifier", "com.apple.developer.team-identifier", "get-task-allow"}
    need(isinstance(ent, dict) and base <= set(ent) and set(ent) <= base | {"keychain-access-groups"},
         "unneeded_profile_capabilities_refused")
    need(ent.get("application-identifier") == team + "." + BUNDLE
         and ent.get("com.apple.developer.team-identifier") == team
         and type(ent.get("get-task-allow")) is bool, "profile_exact_app_mismatch")
    if "keychain-access-groups" in ent:
        groups = ent["keychain-access-groups"]
        need(isinstance(groups, list) and all(g in {team + ".*", team + "." + BUNDLE, "com.apple.token"} for g in groups),
             "foreign_keychain_group_refused")
    # No keychain sharing, memory allowances, app groups or network capability.
    return {key: ent[key] for key in sorted(base)}


def verify_authority(directory, work, profile_raw, certificate_der, device):
    directory = plain(directory)
    cert = load_pinned(directory / "verify_development_certificate.py", CERTIFICATE_HELPER_SHA256,
                       "metal_probe_existing_identity")
    cert.OPENSSL = Path("/usr/bin/openssl")
    previous = sys.modules.get("verify_development_certificate")
    sys.modules["verify_development_certificate"] = cert
    try:
        verifier = load_pinned(directory / "verify_profile.py", PROFILE_HELPER_SHA256, "metal_probe_apple_profile")
    finally:
        if previous is None:
            del sys.modules["verify_development_certificate"]
        else:
            sys.modules["verify_development_certificate"] = previous
    identity = cert.verify()  # Reviewed existing public certificate/CSR and pinned Apple chain only.
    need(identity.get("certificate_chain_valid") is True, "existing_identity_chain_failed")
    need(certificate_der == (directory / "development.cer").read_bytes(), "existing_identity_certificate_mismatch")
    authority = work / "authority"
    (authority / "apple-pki").mkdir(parents=True)
    for name in verifier.ROOTS:
        for suffix in (".cer", ".pem"):
            source = plain(directory / "apple-pki" / (name + suffix))
            shutil.copyfile(source, authority / "apple-pki" / source.name)
    verifier.BASE = authority
    verifier.OPENSSL = Path("/usr/bin/openssl")
    outer = verifier.cms(profile_raw, "outer", "AppleIncRootCertificate",
                         "Apple iPhone OS Provisioning Profile Signing", "Apple iPhone Certification Authority")
    profile = plistlib.loads(outer)
    inner = verifier.cms(profile["DER-Encoded-Profile"], "inner", "AppleRootCA-G3",
                         "WWDR Provisioning Profile Signing", "Apple System Integration CA 4", "1.2.840.113635.100.12.19")
    nodes = verifier.der_nodes(inner)
    need(len(nodes) == 1 and nodes[0].tag == 0x31, "profile_der_root")
    expected = {k: v for k, v in profile.items() if k != "DER-Encoded-Profile"}
    expected["DeveloperCertificates"] = [hashlib.sha256(c).digest() for c in expected["DeveloperCertificates"]]
    expected["ProfileDistributionType"] = "LIMITED"
    need(verifier.decode(nodes[0]) == expected, "profile_der_plist_disagreement")
    parsed = verifier.x509.load_der_x509_certificate(certificate_der)
    units = parsed.subject.get_attributes_for_oid(verifier.NameOID.ORGANIZATIONAL_UNIT_NAME)
    need(len(units) == 1, "ambiguous_certificate_team")
    return profile_contract(profile, certificate_der, device, units[0].value)


def output_paths(output, work):
    output, work = plain(output), plain(work)
    private = (ROOT / "private" / "metal-probe-signing").resolve()
    need(output.is_relative_to(private) and work.is_relative_to(private), "private_probe_output_required")
    need(not output.is_relative_to(work) and not work.is_relative_to(output), "separate_work_output_required")
    need(not os.path.lexists(output) and not os.path.lexists(work) and output.suffix == ".ipa", "fresh_outputs_required")
    need(output.parent.is_dir() and work.parent.is_dir(), "output_parent_required")
    return output, work


def publish_verified(app, output, expected):
    need(set(expected) == SIGNED_FILES, "verified_file_set_required")
    pending = output.parent / ("." + output.name + ".pending-" + uuid.uuid4().hex)
    with pending.open("xb") as stream:
        with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as archive:
            for relative, expected_digest in sorted(expected.items()):
                path = plain(app / relative)
                raw = path.read_bytes()
                need(digest(raw) == expected_digest, "verified_resource_changed")
                member = zipfile.ZipInfo(APP + "/" + relative)
                member.create_system = 3
                member.external_attr = (0o100755 if relative == "MetalProbe" else 0o100644) << 16
                member.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(member, raw)
        stream.flush()
        os.fsync(stream.fileno())
    with zipfile.ZipFile(pending) as archive:
        need(set(archive.namelist()) == {APP + "/" + name for name in expected} and archive.testzip() is None,
             "signed_ipa_coverage_or_crc")
        for name, expected_digest in expected.items():
            need(digest(archive.read(APP + "/" + name)) == expected_digest, "signed_ipa_resource_hash")
    result = digest(pending.read_bytes())
    os.link(pending, output)  # Fails if output appeared; never overwrites a prior IPA.
    pending.unlink()
    return result


def sign(args):
    need(os.name == "posix" and sys.platform.startswith("linux"), "linux_wsl_required")
    output, work = output_paths(args.output, args.work_dir)
    core = load_pinned(args.signer_helper, SIGNER_HELPER_SHA256, "metal_probe_signing_primitives")
    core.no_core()
    need(digest(core.SIGNER.read_bytes()) == core.SIGNER_SHA and digest(core.NODUMP.read_bytes()) == core.NODUMP_SHA,
         "local_tool_pin_changed")
    record = {"scope": "one authored MetalProbe app", "status": "failed", "crypto_verified": False,
              "profile_apple_chain_verified": False, "profile_device_and_app_verified": False,
              "installed": False, "os_signature_authenticated": False, "metal4_runtime_verified": False,
              "gameplay_improvement_verified": False, "revocation_checked": False}
    work.mkdir(mode=0o700)
    try:
        with contextlib.ExitStack() as stack:
            pins = {}
            for name, path, expected, maximum in (
                    ("ipa", args.input_ipa, args.expected_input_sha256, MAX_IPA),
                    ("profile", args.profile, args.expected_profile_sha256, 1 << 20),
                    ("certificate", args.certificate, args.expected_certificate_sha256, 1 << 20),
                    ("device", args.device_record, args.expected_device_record_sha256, 65536)):
                pins[name] = Input(path, expected, maximum)
                stack.callback(pins[name].close)
            key_path = plain(args.key)
            need(key_path.is_file() and 0 < key_path.stat().st_size <= 1 << 20, "bounded_existing_key_required")
            pins["key"] = Input(key_path, digest(key_path.read_bytes()), 1 << 20)
            stack.callback(pins["key"].close)
            need(len({p.path for p in pins.values()}) == len(pins), "separate_inputs_required")
            members = read_pinned_unsigned(pins["ipa"])
            original = members["MetalProbe"]
            old = core.image(original, 2)
            device = json.loads(pins["device"].bytes()).get("unique_device_id")
            runner = core.Runner(work, args.timeout_seconds)
            certificate = pins["certificate"].bytes()
            form = "PEM" if certificate.startswith(b"-----BEGIN CERTIFICATE-----") else "DER"
            cert_der = runner.run(["/usr/bin/openssl", "x509", "-inform", form, "-in", pins["certificate"].path, "-outform", "DER"])
            entitlements = verify_authority(args.identity_verifier_dir, work, pins["profile"].bytes(), cert_der, device)
            record["profile_apple_chain_verified"] = True
            record["profile_device_and_app_verified"] = True
            app = work / APP
            app.mkdir(parents=True)
            for name, raw in members.items():
                (app / name).write_bytes(raw)
                (app / name).chmod(0o755 if name == "MetalProbe" else 0o644)
            entitlement_path = work / "probe-entitlements.plist"
            entitlement_path.write_bytes(plistlib.dumps(entitlements, sort_keys=True))
            command = [core.SIGNER, "-f", "-2", "-k", pins["key"].path, "-c", pins["certificate"].path,
                       "-m", pins["profile"].path, "-e", entitlement_path, "-t", work, app]
            runner.run(command)
            capacity = core.canonicalize(app / "MetalProbe", old, original)
            runner.run(command)
            result = core.verify_image(app / "MetalProbe", 2, old, original, cert_der, entitlements, runner, 0)
            need(result["signature_bytes"] == capacity, "signature_capacity_unstable")
            final = core.local_tree(app)
            need(set(final) == SIGNED_FILES, "unexpected_generated_resource")
            need(all(final[name] == digest(raw) for name, raw in members.items() if name != "MetalProbe"),
                 "unsigned_resource_changed")
            need((app / "embedded.mobileprovision").read_bytes() == pins["profile"].bytes(), "embedded_profile_changed")
            for pin in pins.values():
                pin.recheck()
            output_hash = publish_verified(app, output, final)
            record.update(status="signed_and_verified", crypto_verified=True, images=1,
                          original_append_only_reconstruction_verified=True, output_sha256=output_hash,
                          code_pages=result["code_pages"], resources=result["resources"])
            return record
    except BaseException as error:
        record["reason"] = str(error) if isinstance(error, Refusal) else type(error).__name__
        raise
    finally:
        (work / "signing-result.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("input-ipa", "profile", "certificate", "key", "device-record", "signer-helper",
                 "identity-verifier-dir", "output", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("input", "profile", "certificate", "device-record"):
        parser.add_argument("--expected-" + name + "-sha256", required=True)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args(argv)
    try:
        result = sign(args)
    except Exception as error:
        result = {"status": "refused", "crypto_verified": False, "installed": False,
                  "reason": str(error) if isinstance(error, Refusal) else type(error).__name__}
        print(json.dumps(result))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
