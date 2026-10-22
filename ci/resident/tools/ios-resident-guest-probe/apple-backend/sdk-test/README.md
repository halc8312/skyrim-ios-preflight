# Independent public Apple resident XCTest

This app has a separate bundle identifier, executable and XCTest target from
the existing source-only app and its fifteen tests. Its app entry does not call
Rust. Exactly one test invokes the one-shot public resident backend; no reset,
retry, existing Kernel, game input, generated game code or private framework is
used. Only the self-authored three-entry fixture is accepted by that backend.

Stage this directory in a new CI workspace, then add these public build outputs:

```text
include/ResidentAppleProbe.h             backend/native/ResidentAppleProbe.h
include/PrivateGuestABI.h                public owned ABI header
native/NativeHost.h                      backend/native/NativeHost.h
native/NativeHost.mm                     backend/native/NativeHost.mm
Libraries/libios_resident_apple_backend.a native Rust target's static library
Frameworks/ResidentAppleFixture.framework signed public Simulator fixture
Resources/ResidentAppleFixture.signed-sha256.txt
```

The resource must contain exactly 64 lowercase hexadecimal SHA-256 characters
and one LF. CI computes that complete hash from its independently selected
framework binary **after** signing and **before** app resource packaging.
Neither Swift nor Rust derives the expected hash from the runtime candidate.
The framework is embedded with `link: false`, `codeSign: false` and unchanged
headers: app linking must not load it early or change the signed binary after
hashing. The driver builds for testing first, then checks the packaged
binary/resource remain identical and uses SDK `otool -L` to reject any direct
load dependency on `ResidentAppleFixture` before `test-without-building`.

The app compiles the SDK-only NativeHost and links the real Rust static library
with C++/Foundation. Verify Rust's actual `native-static-libs` requirements on
the SDK target; a missing or changed dependency is a build failure, not a reason
to weaken symbol resolution. The seven fixture APIs load only through the
backend's checked handle. The test requires every result field, the fourteen
`RA_CHECK_ALL` outcome bits, and the external post-signing hash to agree.

Run after the existing fifteen tests have passed, with a fresh output directory:

```sh
python3 run_sdk_tests.py --stage NEW_STAGED_PROJECT --output NEW_RESULTS \
  --destination 'platform=iOS Simulator,id=SELECTED_SIMULATOR_UDID'
```

Rust independently bounds its guest-main wait at three seconds. Swift bounds
the synchronous call at fifteen seconds and terminates this isolated app on a
timeout; XCTest also has a twenty-second allowance. The driver disables parallel
testing/repetition and bounds the complete build/test subprocess at 180 seconds,
terminating its process group on expiry. It requires one named passing case,
one complete structured result and `TEST SUCCEEDED`; zero tests cannot pass.
Logs and the `.xcresult` remain in the new output directory. Pure driver tests
exercise normal/exclusive logs, a real bounded child timeout and false-pass
rejection without any Apple SDK or game input.

This checks public synthetic integration only. Success does not authenticate a
signing authority, attest mapped inode identity, verify a private framework,
demonstrate physical iPhone behavior, or establish game compatibility. Native
jump/unwind details retain the backend's explicit limitations. The C-unwind path
stays in Rust/C++ on the guest thread; the outer C API catches Rust failures
before returning through its ordinary Swift caller.
