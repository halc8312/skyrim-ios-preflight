# Build and install your own copy

This guide is for **`pc-aot/` on the active title branch**. It uses your own game and signing account. This is an experimental developer workflow, not a one-click installer. Build12 has iPhone evidence; the new complete Mac guest/signing route still needs its first independent end-to-end run.

Keep original game files, saves and MODs unchanged. New ignored output directories preserve failed attempts for diagnosis.

## Environment

| Computer | Supported work |
| --- | --- |
| Windows | Prepare the reviewed English executable; optionally cross-compile generated guest C using LLVM/public Darwin headers. |
| WSL or macOS with Python 3.12 | Discover and translate your game locally. |
| Mac with full Xcode | Build the public host, compile the guest with Apple SDK, sign and install after the Windows input-preparation step. |
| Public CI | Public host only. Never upload game files, generated guest code or signing material. |
| iPhone alone | Install an already signed, device-authorized app and copy owned data; source builds require a computer. |

The documented route needs Windows once to prepare the reviewed input, then a Mac for compilation, signing and installation. A Windows-only or independently reproduced Mac-only input-preparation route is not supplied.

The existing host CI uses Apple silicon, **Xcode 26.6 / iPhoneOS SDK 26.5**. Select full Xcode and complete first-launch setup, including iOS support and Metal Toolchain. Other Xcode versions are outside that build evidence. [Apple component instructions](https://developer.apple.com/documentation/xcode/downloading-and-installing-additional-xcode-components).

iOS 18.0+ is the build target. iPhone 17 Pro Max has gameplay evidence. **15 Pro Max / iOS 26 is unverified**, although A17 Pro is Apple9 with BC texture support. Its actual memory allowance, heat and FPS must be measured. [Apple specifications](https://support.apple.com/en-us/111828), [Metal feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf).

## 1 Source and tools

The default branch contains older research. Clone the active branch explicitly:

```sh
git clone --branch codex/skyrim-pc-aot-foundation --single-branch https://github.com/halc8312/skyrim-ios-preflight.git
cd skyrim-ios-preflight/pc-aot
git rev-parse HEAD
```

Record this commit. Use the same checkout for host and guest; do not update halfway through. On the Mac, with Homebrew already installed:

```sh
brew install python@3.12 cmake make bison
export PATH="$(brew --prefix python@3.12)/libexec/bin:$(brew --prefix bison)/bin:$(brew --prefix make)/libexec/gnubin:$PATH"
mkdir -p private
python3.12 -m venv private/self-build-env
. private/self-build-env/bin/activate
python3 -m pip install --only-binary=:all: capstone==5.0.9 pefile==2024.8.26
git clone --no-checkout https://github.com/meizfl/skydream.git vendor/skydream
git -C vendor/skydream checkout --detach ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a
python3 scripts/check_self_build.py
```

The Python packages stay in the virtual environment. Git, Perl, flex, curl and tar are also required. GNU Make must be 4.3+ and Bison 3.x. If the vendor directory exists, inspect it instead of cloning over it. Keep it clean; reviewed patches are applied to isolated copies. Do not reset somebody else's work.

## 2 Owned input

Start with a clean **English Steam 2011 / LE 1.9.32.0** copy. First test the base game. Japanese language data can be added later; do not substitute the Japanese 1.9.31 executable.

| Input | SHA-256 |
| --- | --- |
| Reviewed original TESV.exe | `69362f77bc56bf17570a33106d020b6c67fd8f32a294b82f8b687b9cf71efc72` |
| Prepared candidate | `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605` |

The candidate is 17,462,064 bytes. The reproduced preparation used default settings of [Steamless at this revision](https://github.com/atom0s/Steamless/tree/cd770bf9749d3e4f438d23ac643917ad1a804257). Neither executable is distributed here. Review the tool's license and your applicable use conditions.

Preparation uses Windows MSBuild and .NET Framework 4.8 reference assemblies. In a Visual Studio Developer PowerShell with those tools installed, use a separate folder:

```powershell
git clone https://github.com/atom0s/Steamless.git steamless-source
git -C steamless-source checkout --detach cd770bf9749d3e4f438d23ac643917ad1a804257
MSBuild steamless-source/Steamless.sln '/t:Steamless_CLI;Steamless_Unpacker_Variant21_x86' /p:Configuration=Release /p:Platform=x86 /p:TargetFrameworkVersion=v4.8 /p:RestorePackages=false
```

Check the solution build succeeded; its platform mapping builds both the CLI and x86 plugin. Copy your original `TESV.exe` into a **new** input folder and verify it with `Get-FileHash`. Run `steamless-source/Steamless/bin/x86/Release/Steamless.CLI.exe` against that copy with no extra options. Its output `TESV.exe.unpacked.exe` must not already exist. Verify the output hash above and preserve the original. [Preparation audit](UNPACKING.md).

Copy your own candidate to `private/input/TESV_unpacked.exe` in this checkout, and your clean English installation to `private/owned-game/` with `Data` immediately below it. Transfers stay between your computers, outside Git and public CI.

```sh
python3 scripts/check_self_build.py --candidate private/input/TESV_unpacked.exe
```

Stop on a mismatch. Editing the expected hash does not make another version compatible. Do not substitute online executables or another person's generated guest.

## 3 Discovery and generation

From `pc-aot/`, in the activated Python environment, use new output directories:

```sh
python3 scripts/run_discovery.py \
  --input private/input/TESV_unpacked.exe \
  --sha256 d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605 \
  --output private/self-build-discovery --timeout-seconds 21600

python3 scripts/prepare_title_guest.py \
  --candidate private/input/TESV_unpacked.exe \
  --discovery private/self-build-discovery/analysis/functions.json \
  --output private/self-build-guest --generate
```

The discovery output must match `1fc28ce90d668d635ace8fd641d95720782e24c0f64eaf08cbaf8bf170d15b6d`. A mismatch stops for investigation. The scripts retain original guest bodies and explicit unsupported-operation failures. This does not prove coverage of every game path. The timeout is a bound, not an ETA.

## 4 Compile guest and host

On the Mac with the selected SDK:

```sh
python3 scripts/build_title_guest.py private/self-build-guest --jobs 2
bash scripts/build-title-host.sh
```

Guest compilation resumes only with identical source/header/toolchain contracts. After a contract change, preserve the old stage and generate a fresh one. One or two compile jobs are supported.

The host script downloads pinned public dependencies, runs required tests and builds an unsigned app. It prints a new `local/title-host/run-.../artifacts/` directory. Keep that exact successful directory, including `result.json`, ZIP and three ABI headers. Required failures block a success package.

Optional Windows guest compilation after POSIX generation:

```powershell
py -3.12 scripts/build_title_guest.py private/self-build-guest --jobs 2 --llvm-bin 'C:/Program Files/LLVM/bin' --darwin-headers 'C:/your-tools/zig/lib/libc/include/any-darwin-any'
```

Supply LLVM containing `clang.exe`, `llvm-nm.exe`, `ld64.lld.exe` and complete public Darwin headers, for example from an official Zig distribution. Tool/header identities are recorded. This does not replace the Apple host build or Mac signing. A complete Windows-only installation route is not supplied here.

**If you compile the guest on Windows, also assemble on that same Windows checkout.** Copy the Mac's successful host artifact directory there and run step 5 with Windows paths and `py -3.12`. Then transfer only the unsigned IPA to the Mac for step 6. The guest result records absolute paths: moving the compiled stage to a Mac and reusing its result is not supported.

## 5 Local assembly

Set `HOST` to the successful directory printed in step 4:

```sh
HOST="local/title-host/run-REPLACE_WITH_YOUR_RUN/artifacts"
python3 scripts/assemble_title_ipa.py \
  --host-artifacts "$HOST" \
  --guest-result private/self-build-guest/arm64-ios/result.json \
  --input private/input/TESV_unpacked.exe \
  --input-sha256 d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605 \
  --output private/self-build/SkyrimIOS-unsigned.ipa
```

This verifies host bytes, guest/input binding and shared ABI headers. It does not sign or upload. Omit `--diagnostics-pairing`; that is for a separately configured private receiver.

## 6 Sign for your registered iPhone

Use your own Apple Developer identity and a genuine development or Ad Hoc profile including your iPhone. Register an explicit App ID; replace `com.example.skyrimios` below with yours. Do not share keys or passwords. [Apple profile instructions](https://developer.apple.com/help/account/provisioning-profiles/create-an-ad-hoc-provisioning-profile/).

The profile must authorize `com.apple.developer.kernel.extended-virtual-addressing` and `com.apple.developer.kernel.increased-memory-limit`. The helper includes `increased-debugging-memory-limit` only if granted. Plist text cannot grant capabilities. Free Personal Team profiles and third-party signing services are not validated alternatives. [Memory entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit).

Save your profile at `private/signing/profile.mobileprovision`. Its certificate and private key must already be in the Mac keychain:

```sh
security find-identity -v -p codesigning
python3 scripts/sign_title_ipa.py \
  --unsigned-ipa private/self-build/SkyrimIOS-unsigned.ipa \
  --profile private/signing/profile.mobileprovision \
  --identity YOUR_FULL_40_HEX_CERTIFICATE_SHA1 \
  --bundle-id com.example.skyrimios \
  --output private/self-build/SkyrimIOS-signed.ipa
```

Framework then app are signed; signature and entitlement/certificate readback are checked. Existing outputs are never replaced and failure evidence remains private. Profile decoding is not an independent Apple trust-chain audit; signing success does not prove device acceptance. Private keys are not exported or logged.

## 7 Prepare and transfer Data

```sh
python3 scripts/prepare_owned_data.py \
  --game-dir private/owned-game \
  --candidate private/input/TESV_unpacked.exe \
  --output private/self-build-data/Skyrim
```

The helper copies the vanilla English file list, verifies copied bytes, then publishes `private/self-build-data/owned-data-manifest.json` with `complete=true`. Keep the manifest on the computer; copy only `Skyrim` to the phone. Copy hashes prove transfer integrity, not official provenance of modified files.

No saves, MODs, profile, signing settings or caches are copied. Base game is the default. `--include-dlc` copies complete available vanilla ESM/BSA pairs only; it does not configure load order or establish DLC compatibility.

Install your signed IPA with Xcode's device-management Apps view or Apple Configurator. Enable Developer Mode when required. [Apple installation instructions](https://developer.apple.com/documentation/xcode/managing-apps-on-devices).

With the app closed, use Finder → your iPhone → Files → SkyrimIOS to copy the entire `Skyrim` folder. [Apple File Sharing](https://support.apple.com/en-gb/119585). App Documents must contain:

```text
Skyrim/
  TESV_unpacked.exe
  Skyrim_default.ini
  Data/
    Skyrim.esm
    Update.esm
    Update.bsa
    Skyrim - ... .bsa
    Strings/...
    Interface/Translate_ENGLISH.txt
    Video/BGS_Logo.bik
```

Avoid `Skyrim/Skyrim/`. The app creates a separate profile. Launching before input transfer deliberately stops startup; copy via Finder first rather than bypassing the check.

## 8 First test

1. Keep defaults and shader experiments OFF. Check title, New Game and controls. Do not assume another phone's FPS or memory allowance.
2. Make a new save, fully close, reopen and load it.
3. Return Home for 20 seconds, then check the same scene, controls and sound.
4. Keep graphics fixed when comparing first and second traversals. Initial shader compilation can still pause gameplay.

Self-builds do not automatically send logs to the developer. An unpaired Diagnostics screen reports sending unavailable. Keep full logs private; report source commit, device/iOS, failing step and a relevant non-personal error excerpt. Never post game files, saves, profiles, device IDs or signing reports.

## Troubleshooting and checks

| Symptom | Next step |
| --- | --- |
| Python/package failure | Activate the right 3.12 environment and exact package versions. |
| Input/discovery mismatch | Preserve the attempt and investigate preparation differences. |
| Bison/Make error | Put Homebrew Bison 3 and GNU Make 4.3+ ahead of older system tools. |
| ABI mismatch | Use the same checkout and a fresh generation stage. |
| Capability rejected | Obtain an actually authorized profile; do not bypass the check. |
| Install failure | Check device registration, expiry and matching App ID. |
| Startup failure | Check input hash, folder layout and private startup log. |
| Restart after Home | Preserve logs and iOS termination report; process reclamation remains possible. |

Game-free helper checks:

```sh
python3 -m unittest discover -s tests -p 'test_build_title_guest.py' -v
python3 -m unittest discover -s tests -p 'test_title_assembly.py' -v
python3 -m unittest discover -s tests -p 'test_title_signing.py' -v
python3 -m unittest discover -s tests -p 'test_owned_data.py' -v
python3 scripts/check-public-tree.py
```

At commit `6e7f567`, the four suites ran on Xcode 26.6 / SDK 26.5: 46 passed and one case-sensitive filesystem fixture was skipped (that case passed on Linux). This includes an actual ARM64 iPhoneOS synthetic compile/link and local `codesign -` signature/entitlement verification. The latter is not an Apple account's installable signature. [Recorded evidence](STATUS.md).

These do not replace a fresh full guest build, a valid account/profile or iPhone gameplay. Read the [licensing boundary](../README.md#source-and-licensing-boundaries) before distributing outputs.
