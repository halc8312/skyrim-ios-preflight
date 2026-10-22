This is a separate **public synthetic Apple backend**, not an iOS Skyrim build.
The original Linux resident probe and its results are unchanged. The new default
Linux tests exercise metadata and file policy. Rust `cargo check` for
`aarch64-apple-ios` checks Rust/core type compatibility without an Apple SDK.
Neither result compiles or executes the native Apple helper, dynamic fixture,
Swift test or iPhone app.

The only accepted descriptor has image `[0x82000000,0x2000]`, code
`[0x82001000,0x10]` and exactly the sorted entries
`[0x82001000,0x82001004,0x82001008]`. Its XEX is constructed from the existing
self-authored public `../src/synthetic.rs`. Its two function imports are Kernel
module 0 ordinals `0x83` and `0x19`; it has no variable imports. StrictAll accepts
these two registered handlers. This says nothing about the real game's 215
function imports or their semantics.

The loader requires the fixed main-bundle child
`Frameworks/ResidentAppleFixture.framework/ResidentAppleFixture`. The caller
provides a separately trusted, **post-signing complete-file SHA-256**. There is no
signature normalization or production binding exception. Symlink components,
file identity changes, SHA mismatches, wrong Mach-O platform/architecture,
unknown metadata, loaded header/RX differences, foreign API symbols and writable
function tables refuse registration. The selected file descriptor and dyld
handle remain resident; the sorted function table is copied into owned Rust
storage. `dladdr` and public dyld queries verify each of the seven entry points.
Darwin loading uses the canonical pathname, so before/after fd/header/RX checks
detect races; they are not pre-execution inode attestation or a defense against
an already compromised process. Mapped-inode, OS-provider authenticity and
cryptographic attestation are not claimed. Signature bytes are only a bounded
opaque region to the parser; normal dyld/code-signing policy remains in force.

`NativeHost.mm` uses real Apple SDK definitions for all 49 C ABI layout values
and opaque `jmp_buf` size/alignment. All must agree before struct exchange.
Native jump/unwind providers must be outside the fixture and app; separate
image identity alone does not authenticate a provider. The next SDK gate must
inspect the actual providers. Only the guest/kernel callback path permits
`C-unwind`; native memory/math/query helpers do not. No Swift frame lies between
guest C++ and Kernel's Rust `ThreadExit` catch. The one-shot global owner has no
unload/reset operation. A failed partial registration requires a fresh process.

The public fixture reuses only `PrivateGuestABI.h`, `owned_abi.cpp` and
`jump_tail.S` from the generic owned ABI directory. It deliberately does **not**
use `private_layout.cpp`, generated C++, a private framework, an actual XEX,
game assets or logs. A fresh local copy of the public context header receives
the already reviewed acquire/release pointer-and-element overlay. The original
header is untouched. Entry one performs a same-thread native jump within its
still-live C++ frame; entry two returns normally; guest main dispatches the real
frequency handler and ExTerminateThread. Expected outcomes are a 50 MHz value,
dirty byte zero, exactly two registered imports, C++ entry cleanup count one,
Busy for active publication and OK for publication after unwinding. Locally
defined tails and `returns_twice` IR must be inspected before claiming that
those paths, rather than a direct system call, were exercised.

For a future source-only Mac CI, use official Xcode/SDK and Rust
`nightly-2026-10-11`; add `aarch64-apple-ios` and
`aarch64-apple-ios-sim` std targets. The standalone lockfile has no `guest-sys`.
Its six public workspace crates are runtime-core, kernel-hle, vfs, xex, d3d-hle
and xma-decoder. `source-allowlist.txt` lists the exact local public text inputs;
third-party registry dependencies are obtained by locked Cargo, and SIMDE is
the same pinned public source used by the existing SDK probe. A CI stage may
clone the pinned upstream source and apply these allowlisted public patches, or
copy the six crates and replace only the staged workspace members/default list
with those six names. Do not upload arbitrary build trees or use broad globs.

`build_next_sdk_gate.sh PUBLIC_SOURCE GENERIC_ABI SIMDE NEW_OUTPUT device|simulator`
prepares fresh public headers, compiles the actual generic adapter/tails and
fixture, strictly links using the official SDK, audits exactly seven exports,
requires local jump/resume definitions and `returns_twice` IR, compiles the
native SDK provider and builds one Rust staticlib. It does not dispatch CI,
package/sign an app or run a test. Unknown SDK shapes are failures to investigate,
not permission to weaken these checks. The public disassembly must confirm
BR-only tails and actual fixture call targets in addition to this script's
structural checks.

`sdk-test/` is a separate, not-yet-SDK-built XcodeGen test host maintained by the
independent wrapper reviewer. Populate its staged staticlib/header inputs and
`Frameworks/ResidentAppleFixture.framework` and
its exact signed-hash resource from the reviewed SDK outputs. For
Simulator, finish install-name/Info.plist changes, use ordinary ad-hoc
`codesign --force --sign - --timestamp=none`, verify strict, then compute the full
binary SHA. The official wrapper resource is
`ResidentAppleFixture.signed-sha256.txt`: exactly 64 lowercase bytes plus one LF;
Swift passes only those first 64 bytes to the Rust C ABI.
Build the app/tests, verify that the embedded binary still has that hash, sign
the enclosing app/test bundles normally and test one fresh process. Do not
resign the embedded framework after pinning its hash. If a device signing step
changes it, update the separately trusted post-signing binding by the reviewed
process or refuse loading; this prototype does not solve device resigning.

The one XCTest requires all result fields and `checked_flags==0x3fff`; the Rust
path also requires a second host binding to be rejected. The guest-main wait
is three seconds, Swift stops the isolated app after fifteen seconds, XCTest
allows twenty seconds and its driver bounds build/test work at 180 seconds.
SDK init or synchronous loader work is not forcibly interrupted by the
three-second guest wait. A passing run would
prove only this public runtime/jump/ThreadExit integration on that SDK target.
Device signing, physical iPhone execution, actual game loading, renderer/input
integration and gameplay remain separate gates. Existing fixed-two-entry tests
are not relaxed or replaced.

Primary platform references: [Apple dladdr](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/dladdr.3.html),
[Apple dyld source](https://github.com/apple-oss-distributions/dyld/blob/main/dyld/DyldAPIs.cpp),
[NSBundle bundlePath](https://developer.apple.com/documentation/foundation/bundle/bundlepath).
