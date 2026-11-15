# Skyrim 2011 on iPhone

An experimental iPhone runtime for a personally owned **Skyrim 2011 / Legendary Edition**. The Windows x86 game is translated locally to C and compiled ahead of time to ARM64. Graphics run through DXVK and MoltenVK. This is not an official Bethesda release.

**Start here: [Build and install your own copy](docs/BUILD_YOUR_COPY.md).**

This repository contains development sources and tools, not Skyrim, a ready-to-play IPA, generated game code, signing keys or saves. Owning LE supplies the game input; it does not supply an Apple signing identity or grant redistribution rights for every dependency.

## Current status

| Area | Evidence and limits |
| --- | --- |
| Gameplay | Build12 has been played on iPhone 17 Pro Max. Stutters and stability issues remain under investigation. |
| Home and return | One recorded background interval of about 21 seconds resumed in the same process. iOS can still terminate the app under memory pressure. |
| Language | English default; Japanese text/voice selection is implemented. Language data must come from the owner's copy. |
| iPhone 15 Pro Max with iOS 26 | Suitable test candidate, **not yet gameplay-verified**. A17 Pro supports the BC texture formats used here; FPS, heat and memory headroom need measurement. |
| Build target | ARM64 iPhoneOS 18.0+. This is not a verified minimum-device list. |
| Self-build | Portable guest compilation, data preparation and Mac signing helpers are provided. The newly documented complete setup has not yet been reproduced on a fresh Mac. |

[Development status](docs/STATUS.md) separates completed checks from proposals. The five shader experiments default to OFF; keep settings fixed for comparisons.

## Requirements

- Your own **English Windows Skyrim 2011 / LE 1.9.32.0**. SE/AE and the Japanese 1.9.31 executable are different inputs. Exact input hashes are enforced.
- A Mac with full Xcode and iPhoneOS SDK for the complete build/sign/install route. Preparing the reviewed Steam executable uses a Windows tool. Windows/WSL can generate and cross-compile the guest; this alone is not an installable app.
- Python 3.12+, Git and the tools in the guide.
- Your own signing identity and registered-device provisioning profile with the required memory capabilities. Free Personal Team profiles are not a validated alternative.

The read-only prerequisites check does not install tools, start CI or change the game. The guide identifies steps that download public dependencies.

## How it fits together

```text
Owned English game → separate preparation → local ARM64 guest
Public runtime → Apple SDK host
Host + your guest → local assembly and signing → your iPhone
Owned Data folder → separate transfer to your iPhone
```

A public host or simulator check alone is not a playable game. Older research/probe workflows remain for development; use the title guide above for installation.

## Source and licensing boundaries

Dependencies retain their own notices. The pinned [Skydream notice](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/NOTICE.md) identifies unresolved redistribution permission for inherited runtime/recompiler code. Do not treat this entire project or game-derived output as covered by a blanket GPL grant. Personal local build instructions do not resolve those permissions.

Keep game inputs, generated C, initialization data, IPAs, profiles and private logs in ignored `private/` or `local/` folders. Never attach them to public issues or CI artifacts. Ordinary self-builds omit diagnostic receiver credentials.

## Development references

- [Build and install](docs/BUILD_YOUR_COPY.md)
- [Current evidence and known issues](docs/STATUS.md)
- [Source audit](docs/SOURCE_AUDIT.md)
- [Performance work](docs/PERFORMANCE_WORK.md)
- [Native migration research](docs/NATIVE_MIGRATION.md)
- [Input preparation audit](docs/UNPACKING.md) and [translation research](docs/TRANSLATION.md) — historical records, not the quick-start guide
