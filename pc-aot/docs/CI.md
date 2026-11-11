# iPhone build and private CI evidence

The two manual Codemagic workflows build **SkyrimProbe**, a native diagnostic app. They do not translate, package, or run Skyrim. Both workflows are unrun on macOS as of 2026-11-12; Windows validation of the CI helpers is not an Apple SDK compile, an IPA signing result, or iPhone gameplay evidence.

## Pinned environment

`codemagic.yaml` selects `mac_mini_m2` and Xcode `26.6`. Codemagic documents this image as Xcode **26.6 / 17F113**, macOS **26.5.1 / 25F80**, with 12 GB RAM on M2. Its listed iOS 26.5 runtime includes iPhone 17 Pro Max. The script checks the actual Xcode version/build and ARM64 host, records the actual iPhoneOS SDK version/build/path and Clang version, and fails on a pin mismatch. SDK selection is `iphoneos`, not the listed simulator runtime. Actual device OS compatibility and installation remain to be measured. [Codemagic Xcode 26.6 image](https://docs.codemagic.io/specs-macos/xcode-26-6/), [machine types](https://docs.codemagic.io/knowledge-codemagic/machine-type/).

The Xcode scheme is `SkyrimProbe` in `ios/SkyrimProbe.xcodeproj`. Builds use Release, ARM64 only, `generic/platform=iOS`, iOS 18.0 minimum, and two compile jobs. The limit avoids using all CPU cores on a 12 GB worker; it is not a promise that a future translated game will fit. No dependency manager, download, simulator build, or new cloud resource is part of this configuration. The source layout follows Codemagic's native Xcode workflow model. [Native iOS build configuration](https://docs.codemagic.io/yaml-quick-start/building-a-native-ios-app/).

## Manual unsigned compile and link

Select `probe-unsigned` only after an authorized private repository/application is available and the desired source revision is committed. This file configures neither push/PR/schedule triggers nor a build dispatch. Existing Codemagic account access does not establish a usable Skyrim app identifier or authorize a paid build.

On an equivalent, already provisioned Mac, the same entry point is:

```sh
bash scripts/build-ios.sh unsigned
```

The script requires a clean committed checkout and checks `CM_COMMIT`, when provided, against `git rev-parse HEAD`. Signing is disabled. Successful compilation must produce an actual thin ARM64 Mach-O executable with iOS platform 2, minimum iOS 18.0, the selected SDK, no signature, and a dSYM with the same UUID. The result is named `SkyrimProbe-UNSIGNED-NOT-INSTALLABLE.app.zip`. **It cannot be installed on an iPhone.** A successful job establishes only probe compile/link and artifact validation.

The scripts also work with this source tree nested under `pc-aot/` in an existing private repository: call `bash pc-aot/scripts/build-ios.sh unsigned` from the repository root. The script selects its own source directory; the source preflight scopes its Git file listing to that subtree, while the build requires the entire checkout to be clean. A parent workflow must collect `pc-aot/local/ci/*/artifacts/*`. GitHub Actions run ID/attempt and `GITHUB_SHA` are supported for evidence and commit matching. A different provider still needs the exact pinned Xcode build; a matching runner label alone is insufficient. No GitHub Actions workflow or dispatch is created by these files.

## Separate private ad hoc archive and export

`probe-ad-hoc` is intentionally separate and fails without existing signing resources. No certificate/profile creation, automatic provisioning update, device registration, App Store Connect publishing, or TestFlight delivery is invoked. Codemagic's `ios_signing` supports referencing previously uploaded profiles/certificates and exposing a profile path through an environment variable. [Codemagic signing configuration](https://docs.codemagic.io/yaml-code-signing/signing-ios/).

Before a separately authorized signed run, configure these existing resources in the correct private Codemagic application/team:

| Configuration | Required value |
| --- | --- |
| Profile reference `skyrim-probe-ad-hoc` | An existing unexpired ad hoc profile for the exact probe app ID, with the intended registered iPhone included. Codemagic exposes its path as `SKYRIM_PROFILE_PATH`. |
| Certificate reference `skyrim-probe-distribution` | The existing distribution certificate and its private key matching that profile; Codemagic installs it into the build keychain. |
| Secret group `skyrim_probe_signing` | The variables below. Values never belong in the repository or a public report. |
| `SKYRIM_BUNDLE_ID` | A valid explicit app ID owned by the user. `org.skyrimios.probe` is only the unsigned/default identifier, not a claim of registration. |
| `SKYRIM_TEAM_ID` | The matching ten-character Apple team ID. |
| `SKYRIM_DEVICE_UDID` | The intended registered physical device's identifier, already present in the profile. |
| `SKYRIM_SIGNING_IDENTITY` | The matching existing distribution certificate's 40-character SHA-1 fingerprint, avoiding ambiguous identity selection. |

On a Mac with that profile, identity, and environment already configured:

```sh
bash scripts/build-ios.sh ad-hoc
```

The script decodes and checks the profile locally without printing its device list. It requires exact bundle/team matching, distribution entitlements, device membership, certificate membership, and a future expiry. It writes private signing/export configuration under the unique build directory, runs `archive`, then `-exportArchive` using Xcode's `release-testing` export method (ad hoc registered-device distribution). It never passes `-allowProvisioningUpdates`.

Both archived and exported apps undergo strict `codesign` verification, entitlement/team checks, profile/device checks, and extraction/comparison of the actual signing certificate fingerprint. The exported executable UUID must match the archive/dSYM. An IPA passing these gates is a candidate for installation on the registered device; installation, launch, performance, input, sound, and gameplay require separate physical-device evidence.

## Evidence and privacy boundary

Each invocation creates a new `local/ci/<UTC>-<mode>-<random>/` directory. It preserves raw logs, DerivedData, partial archives/exports, private signing configuration, and failure state without deleting previous runs. `set -euo pipefail` propagates compiler/export errors through `tee`; an EXIT handler records the original failure code. A machine termination can prevent the handler from running, so retain the Codemagic job log as well.

The narrow Codemagic artifact glob collects only that run's `artifacts/` files. On success these include:

- The unsigned, noninstallable app ZIP **or** the signed ad hoc IPA plus `.xcarchive.zip`.
- The corresponding `.dSYM.zip`, build/archive/export/verification logs, and `manifest.json`.
- Manifest mapping of exact Git commit/tree, Codemagic build ID, Xcode/SDK/compiler, executable and dSYM UUIDs, SHA-256 hashes, file sizes, signature state, mode, exit code, and timestamps.

Failure logs/manifests are also retained. Do not interpret a leftover candidate file from a failed packaging step as a verified build; `status: verified` and exit code zero are both required. SHA-256 identifies the archived bytes; it does not establish device success or deterministic Apple signing.

**All CI artifacts and logs stay private.** Signed app/archive contents include a provisioning profile and its device identifiers; dSYMs and logs can contain paths and implementation details. No public artifact destination, tester publishing, email attachment, GitHub release, or game-input upload is configured. Download and retain build evidence before the CI provider's retention period expires; no retention extension is provisioned here.

`scripts/check-public-tree.py` checks tracked files and nonignored untracked files. It rejects private/generated/vendor/build/data paths if added to Git, binary/game/archive/signing extensions, nontext binary payloads (including renamed PE/ELF/Mach-O/BSA files), private-key markers, and recognizable decompiler output. Ignored local inputs are neither opened nor uploaded. Allowlisted UTF-8 source types and a size limit make unexpected additions fail for review. This is a preventive gate, not proof of copyright provenance or exhaustive secret detection: arbitrary renamed generated text still requires human source review before sharing. Do not reference private game inputs from the probe Xcode project. The product gate independently allows only expected probe bundle/dSYM/archive members, rejecting foreign resources before packaging.

## Future generated-input cache contract

There is **no generated game input, translation pipeline, cache restore, or private upload in either workflow**. A future, separately authorized translation stage must keep raw inputs, derived C/data, and cache objects under ignored private storage. Reuse must be content-addressed and verified against a manifest containing:

1. SHA-256 of every original executable/library/data input, version/edition, and the exact permitted input set; no original paths, user names, or bytes in public metadata.
2. Translator source commit and executable hash; every analysis/translation option; patch/import-ABI schema versions; ordered inputs and deterministic seed, if any.
3. Output file paths, sizes, and SHA-256 values; unresolved instruction/call diagnostics; explicit success/failure status. Unsupported instructions or imports prevent cache acceptance.
4. For compiled-cache reuse, target triple, deployment target, compiler/Xcode/SDK build identifiers, exact flags, and relevant runtime/ABI source hashes.

The cache key is SHA-256 over a versioned, canonical JSON encoding of these inputs. Validate the manifest and all outputs before reuse; any mismatch is a cache miss. Store immutable objects atomically under a new key, preserve failed attempts, and never put derived game code/data in public Git or public artifacts. No cross-device gameplay claim follows from cache validation. A repeatable compiler input set is a future milestone; it is not implemented by this document.

## Local checks and remaining validation

Actual cloud result (2026-11-12): [GitHub Actions run 43071698303](https://github.com/halc8312/skyrim-ios-preflight/actions/runs/43071698303) passed unsigned iPhoneOS ARM64 compile/link, product checks and dSYM UUID matching at source `c764be0f8fdce122151ac45ab5187abbbc3a15d7`. The existing private repository runs this independent tree under `pc-aot/`; no new service account or signing credentials were created. The branch-only workflow uses the [documented macos-26 ARM64 image](https://github.com/actions/runner-images/blob/macos-26-arm64/20261102.0473/images/macos/macos-26-arm64-Readme.md) and `DEVELOPER_DIR=/Applications/Xcode_26.6.app/Contents/Developer`. Original downloaded output hashes and earlier failures are recorded in STATUS.md. This result is an unsigned diagnostic app, not an installable IPA, simulator execution or physical-device/gameplay validation.

These checks require only existing tools and do not run cloud builds:

```powershell
py -3.12 -m unittest discover -s tests -p 'test_ci*.py' -v
py -3.12 scripts/check-public-tree.py
wsl -d Ubuntu-24.04 -- bash -n /mnt/f/SkyrimIOS/scripts/build-ios.sh
```

The tests use explicitly synthetic Mach-O/profile fixtures to exercise rejection of wrong architecture/platform/deployment target, malformed signatures, dSYM/SDK mismatches, foreign app payloads, wrong/expired signing profiles, and forced Git additions of ignored private files. They verify manifest hashes and failure retention. YAML parsing uses already-installed PyYAML when present and otherwise reports a skip; the build scripts themselves use Python's standard library. Remaining validation: an authorized real macOS compile/link, real ad hoc archive/export with existing signing material, and registered-device installation and measurement. No Windows test substitutes for those checks.
