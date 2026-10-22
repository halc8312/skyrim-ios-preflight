# Experimental signing byte binding

This is a local, read-only prototype for comparing a narrowly supported signed
Mach-O image with an independently trusted **full SHA-256 of its unsigned
original**. It reconstructs original bytes in memory, hashes the entire result,
and never writes a reconstructed executable. It is not a loader, signature
authenticator, certificate check, entitlement policy or attestation system.

The hand-authored public fixture suite has 23 tests. A separate source-only
Xcode 26.6 / iPhoneOS SDK 26.5 experiment signed the public `SyntheticGuest`
framework with `codesign -s -` and passed `codesign --verify --strict`. That
first CI run (`f58e52b`) refused its larger reserved signature allocation.
The narrow reservation rule below now passes local read-only replay of that
downloaded SDK output; a fresh CI run with this revision remains pending.
No private framework, game content, AltServer output or physical iPhone was
used. **Do not integrate this prototype into the real loader yet.** A later
AltServer experiment remains necessary for that tool's output.

The manifest must be created before signing and its original full SHA-256 must
be supplied independently from a trusted local binding. Taking an expected hash
from an untrusted manifest does not establish provenance. The strict schema
records original length, header command counts/length, first file-backed text
section, original `__LINKEDIT` offset/sizes and the fixed 16 KiB VM policy.

## Supported mutation shape

- Thin little-endian arm64 subtype 0 `MH_DYLIB`, iOS device or simulator build
  version; one terminal `__LINKEDIT`. FAT, arm64e, legacy/unknown commands and an
  already signed original refuse.
  SDK `LC_SOURCE_VERSION` is preserved byte for byte. `LC_ENCRYPTION_INFO_64`
  is supported only with `cryptid=0`, zero reserved padding, and an inactive
  encryption range inside file-backed text payload; its fields cannot change.
  A zero-fill-only segment can have an empty file interval at file offset zero;
  this does not exempt its VM range from overlap/bounds checks.
- The original header padding is entirely zero and has at least 16 bytes free.
  File-backed sections must have matching segment-relative file/VM offsets and
  segment names. Symbol/string tables, dynamic symbol tables, dyld info and
  section relocations must reference data inside the original `__LINKEDIT`,
  excluding spare header bytes; dynamic symbol indexes are bounded too.
  Nonempty zero-fill sections require offset zero and must lie in the segment's
  VM tail beyond its file-backed extent, so they cannot occupy header padding.
  A new 16-byte `LC_CODE_SIGNATURE` is appended to the existing load commands;
  existing command positions/order remain unchanged. Only `ncmds` and
  `sizeofcmds` increase by 1 and 16 respectively.
- The original file ends exactly at `__LINKEDIT`'s end. At most 15 zero bytes
  align the appended signature to 16 bytes; the signature is the final data.
- `__LINKEDIT.filesize` grows exactly to the new EOF. Its `vmsize` remains the
  original value if sufficient, otherwise rounds the new size up to 16 KiB.
  No other original field may change.
- A bounded embedded SuperBlob envelope with known child slots/magics, distinct
  nonoverlapping child ranges, and only zero gaps/tail is recognized. Child
  bodies, CodeDirectory hashes and CMS are **not authenticated or fully parsed**.
  An accepted byte binding can therefore accompany an invalid OS signature.
  Allocation is exactly the SuperBlob length rounded to 16 bytes, or that size
  plus exactly 18,000 bytes when an empty eight-byte CMS wrapper is present.
  The latter narrowly supports Apple's default ad-hoc CMS estimate; every extra
  byte must be zero. Arbitrary reservations and nonempty-CMS reserved forms
  remain unsupported. The observed SDK allocation was 18,400 bytes for a
  396-byte SuperBlob, with 18,004 zero bytes after its logical end.

After restoring just those four original header/segment fields, zeroing the new
command slot, and truncating the append, every original byte participates in
the full SHA-256 comparison. Code, data, exports, chained fixups, existing load
commands and all original padding remain bound. Unsupported signer changes
refuse; there is no fallback and no normalization of unrelated bytes.

The CLI rejects duplicate JSON keys/nonfinite numbers and uses one bounded read
for the manifest. It has `manifest UNSIGNED NEW_JSON` (exclusive new destination) and
`verify SIGNED MANIFEST --expected-unsigned-sha256 TRUSTED_HASH`. It only prints
fixed result metadata or a sanitized rejection; inputs remain read-only. Run
public tests with `python -m unittest discover -s THIS_DIRECTORY -v`.

## Primary-source basis and limits

Apple's [Mach-O definitions](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h)
define `mach_header_64`, `segment_command_64`, and the 16-byte
`linkedit_data_command`. Apple's
[codesign_allocate](https://github.com/apple-oss-distributions/cctools/blob/main/misc/codesign_allocate.c)
adds a signature command in spare header space, increments the two header
fields, aligns signature data, and extends the linkedit sizes. Its optional
page-alignment path can also change the string table size; **that form is
deliberately unsupported**, as are other original-byte rewrites.

LLVM's [Mach-O signature layout](https://www.llvm.org/doxygen/MachOLayoutBuilder_8h_source.html)
describes terminal signature data and 16-byte alignment. Apple's
[signature blob definitions](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/cs_blobs.h)
provide the structural magics. Apple's
[TN3126](https://developer.apple.com/documentation/technotes/tn3126-inside-code-signing-hashes)
describes embedded Mach-O signatures through `LC_CODE_SIGNATURE`.

Apple's [CodeSigner default CMS estimate](https://github.com/apple-oss-distributions/Security/blob/main/OSX/libsecurity_codesigning/lib/CodeSigner.cpp#L308-L312)
is 18,000 bytes. Its [signer passes an estimate before building the final blob](https://github.com/apple-oss-distributions/Security/blob/main/OSX/libsecurity_codesigning/lib/signer.cpp#L648-L678),
and [MachOEditor aligns the allocation and permits a shorter actual blob](https://github.com/apple-oss-distributions/Security/blob/main/OSX/libsecurity_codesigning/lib/signerutils.cpp#L175-L228).
These sources plus the observed public SDK output support the exact empty-CMS
reservation rule, rather than a general trailing-zero allowance.

Those sources motivate a conservative supported subset; they do not establish
that every current Apple or third-party signer uses this subset. Synthetic
success proves the reconstruction algorithm only. The verifier's fixed false
`apple_signer_tested` flag means that it does not execute or authenticate a
signer; the separate CI report records actual signing commands. OS signature
validation, bundle resources/provisioning, other real signing behavior, loaded-image identity,
file-to-load races and all iOS runtime behavior remain separate gates.
