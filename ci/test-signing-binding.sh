#!/usr/bin/env bash
# Public SDK-built SyntheticGuest only. No game or private-guest path is read.
set -euo pipefail
test "$(uname -s)" = Darwin
test "$#" -eq 2
framework="$1"
output="$2"
harness="$(cd "$(dirname "$0")/.." && pwd)"
tools="$harness/ci/signing-binding"
test -d "$framework"
test -f "$framework/SyntheticGuest"
test "$(basename "$framework")" = SyntheticGuest.framework
test ! -e "$output"
mkdir -p "$output/signed"
xcodebuild -version > "$output/apple-tools.txt"
xcrun --sdk iphoneos --show-sdk-version >> "$output/apple-tools.txt"
python3 -m unittest discover -s "$tools" -p 'test_*.py' -v \
  2>&1 | tee "$output/unit-tests.log"
# This expected SHA comes directly from the original SDK output BEFORE signing,
# independently of the manifest supplied to the reconstruction verifier.
expected="$(shasum -a 256 "$framework/SyntheticGuest" | awk '{print $1}')"
cp "$framework/SyntheticGuest" "$output/SyntheticGuest.unsigned.bin"
python3 "$tools/signing_binding.py" manifest "$framework/SyntheticGuest" \
  "$output/unsigned-manifest.json" | tee "$output/manifest.log"
cp -R "$framework" "$output/signed/SyntheticGuest.framework"
signed="$output/signed/SyntheticGuest.framework"
# No Apple account, keychain identity, certificate or provisioning profile.
/usr/bin/codesign --force --sign - --timestamp=none "$signed" \
  > "$output/codesign.log" 2>&1
/usr/bin/codesign --verify --strict --verbose=2 "$signed" \
  > "$output/codesign-verify.log" 2>&1
/usr/bin/codesign --display --verbose=4 "$signed" \
  > "$output/codesign-details.log" 2>&1
cp "$signed/SyntheticGuest" "$output/SyntheticGuest.signed.bin"
python3 - "$tools" "$framework/SyntheticGuest" "$output" "$expected" "$harness" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

tools, original_path, output, expected, harness = sys.argv[1:]
sys.path.insert(0, tools)
import signing_binding as binding
output = Path(output)
original = binding.read_bounded(Path(original_path))
signed = binding.read_bounded(output / 'SyntheticGuest.signed.bin')
manifest = binding.load_manifest(binding.read_bounded(output / 'unsigned-manifest.json', 16384))
assert binding.sha(original) == expected, 'The original public SDK fixture changed.'
assert binding.sha(binding.read_bounded(output / 'SyntheticGuest.unsigned.bin')) == expected
report = {
    'kind': 'public_apple_sdk_adhoc_signing_binding_probe',
    'source_commit': subprocess.check_output(['git', '-C', harness, 'rev-parse', 'HEAD'], text=True).strip(),
    'platform': 'iphoneos', 'public_fixture_only': True,
    'unsigned_sha256': expected, 'signed_sha256': binding.sha(signed),
    'unsigned_bytes': len(original), 'signed_bytes': len(signed),
    'actual_codesign_command_succeeded': True, 'codesign_verify_strict_passed': True,
    'signing_identity': 'adhoc', 'apple_account_used': False,
    'source_fixture_unchanged': True, 'game_inputs_used': False,
    'signature_authority_authenticated': False, 'attestation': False,
    'private_guest_framework_verified': False, 'physical_iphone_verified': False,
    'loader_integration_allowed': False,
}
# Public fixture diagnostics only. No broad exclusion is added if Apple emits
# an unsupported shape: preserve the signed/unsigned sample, report and fail.
try:
    result = binding.verify_binding(signed, manifest, expected_unsigned_sha256=expected)
    report['normalization'] = result
    negatives = {}
    for label, position in [('text_payload_byte', manifest['first_data']),
                            ('original_linkedit_byte', manifest['linkedit_fileoff'])]:
        changed = bytearray(signed)
        changed[position] ^= 1
        try:
            binding.verify_binding(bytes(changed), manifest, expected_unsigned_sha256=expected)
        except binding.Rejected:
            negatives[label] = True
        else:
            negatives[label] = False
    report['one_byte_mutation_rejected'] = negatives
    report['success'] = all(negatives.values())
except binding.Rejected as error:
    report['normalization_rejection_category'] = str(error)
    report['success'] = False
# The allowed masks remain exactly the prototype's original forty bytes.
allowed = set(range(16, 24))
allowed.update(range(32 + manifest['sizeofcmds'], 48 + manifest['sizeofcmds']))
linkedit = manifest['linkedit_command_offset']
allowed.update(range(linkedit + 32, linkedit + 40))
allowed.update(range(linkedit + 48, linkedit + 56))
report['changed_original_bytes_outside_allowed_regions'] = sum(
    original[index] != signed[index] for index in range(min(len(original), len(signed)))
    if index not in allowed)
assert binding.sha(binding.read_bounded(Path(original_path))) == expected
(output / 'sdk-signing-result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, sort_keys=True))
raise SystemExit(0 if report['success'] else 1)
PY
