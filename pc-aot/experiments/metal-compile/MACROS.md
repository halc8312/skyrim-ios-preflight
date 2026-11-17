# Authored macro diagnostic observation

`macro_probe.py` observes eight authored macro expansions through deliberate
`static_assert` failures. These failures are the test input: a successful shader
compilation is an unexpected result. No pipeline, command queue or GPU work is
created. The Mac side uses the legacy `MTLDevice` library API; it does not require
Metal 4, but it still requires an available ordinary Metal device.

```sh
# Windows, using the installed Apple tools; output must be a new directory.
python experiments/metal-compile/macro_probe.py --tools local/tools/metal-6.2/bin --output local/metal-compile/macro-new --budget 90 --timeout 30

# Mac with SDK 15 or later. A missing device produces unsupported_native.
python3 experiments/metal-compile/macro_probe.py --output local/metal-compile/macro-new --budget 90 --timeout 30

# Portable controls; also discovered by the existing test_metal_compile_*.py suite.
python -m unittest discover -s tests -p test_metal_compile_macros.py -v
```

The fixed authored profile is macOS 15, MSL 3.1, safe/precise math, invariance
false and default optimization. Windows and Mac CLI observations use explicit
`-D` token strings. The Mac then submits the same source twice: once with typed
NSNumber constructors (plus one NSString control), and once with NSString macro
values. Effective option getter snapshots, authored constructor inputs, raw
diagnostics and individually observed token spellings remain distinct.

Only the actual observed `path:line:column: error: static_assert failed` format is
accepted. The parser requires every authored case exactly once, the expected
source and assertion line, complete markers, and no unrelated errors. Source
echoes and notes cannot supply observations. Windows Metal 6.2's escaped path
spelling is accepted only for the exact submitted path. Unknown/localized
formats, missing markers, duplicates or truncation remain unsupported; Apple
does not promise this diagnostic formatting as a stable API.

`offline_tokens_observed` means the deliberate CLI rejection exposed all eight
submitted tokens. `diagnostic_tokens_observed` additionally means both native
diagnostic sets were captured. A difference in NSNumber versus NSString tokens
is retained as a comparison result, not hidden by normalizing their spelling.
Neither status means successful shader compilation, type/value equivalence,
production option parity, Windows-library runtime loading, or GPU correctness.
In particular, a negative-zero token is not a proof that an evaluated expression
preserves its sign. Suffixes and signed-64-bit minimum literals also need separate
type and value tests before an automatic production conversion is justified.

The first Windows Metal 6.2 runner observation captured all eight tokens. Thirteen
portable controls passed, including source-echo rejection, ambiguous diagnostics,
unexpected compiler success, truncation, scope claims, fresh output, both
KeyboardInterrupt/SystemExit, and atomic report replacement. No game data or
production capture is involved.

The actual Mac run at public source `5539d19` compiled the native probe and
observed all eight expansions through the Apple Paravirtual device, Xcode 26.6 /
SDK 26.5 / Metal compiler 32023.883. The NSString route matched the explicit CLI
tokens in all eight cases. NSNumber tokens differed from the string controls in
seven cases; some differences were integer suffixes, not established value
changes. At that point, two floating-point observations still needed a GPU
value check:

| Authored value | NSNumber token | NSString / CLI token |
| --- | --- | --- |
| Float bits `0x3f800001` | `1` | `1.00000011920928955078125f` |
| Float bits `0x80000000` | `-0` | `-0.0f` |

The separate option getter retained both original float bit patterns. Therefore
getter equality alone does not establish equal compiler input. The next Mac run
at public source `c86a082` also completed the authored GPU value probe:

| Original float bits | NSNumber GPU bits | NSString GPU bits |
| --- | --- | --- |
| `3f800001` | `3f800000` | `3f800001` |
| `80000000` | `00000000` | `80000000` |

Both routes passed the direct-literal, buffer-input and unwritten-guard controls.
This establishes a difference for these two authored cases on that Paravirtual
Mac device. It does not establish a Skyrim bug or all-type equivalence.
Replacing NSNumber with more precise strings could change existing semantics;
do not automatically make that a production precision fix. Token reports and
compiler diagnostics remain under
`local/metal-compile/mac-run-03/evidence/macro-diagnostics/`. GPU value reports
are under `local/metal-compile/mac-run-04/evidence/macro-values/`, with the same
results confirmed in `local/metal-compile/mac-run-05/evidence/macro-values/`.

`macro_value.py` follows those two authored float cases through ordinary Metal
compute readback. It keeps original bits, numeric getters, diagnostic tokens and
GPU bits in separate fields; fixed literal, buffer-input and unwritten-guard
controls check that the expected work ran. Differences between NSNumber and
NSString are observations, not an automatic failure or a production fix. It
uses no Metal 4 API. Its 14 portable controls and actual Apple execution passed;
the latter evidence is retained in
`local/metal-compile/mac-run-04/evidence/macro-values/`.

`macro_integer.py` extends the question to unsigned integer input with three
separate cases: `numberWithUnsignedInt(0xffffffff)`, the previously observed
unsuffixed token as NSString, and the explicitly unsigned `"4294967295u"` string.
It reads back `uint(value)`, `sizeof(value)` and `-1 < value`, alongside direct
uint-literal, input-buffer, signature and unwritten-guard controls. Only the
explicit uint case has a fixed expected expression triple; the other two are
observations. It does not recapture NSNumber's expansion token or infer the
original type from the three expressions. A rejected case retains its raw
compiler error and makes the aggregate fail even when later controls pass.
Its 12 portable controls and actual Apple execution passed in run 6.

[Run 6](https://codemagic.io/app/6ad977fcf240708df6941076/build/6afcb807d133a39c4a125e73)
at public source `c3c12ae` observed the following on the Apple Paravirtual device,
macOS 26.5.1 / SDK 26.5. Every library, pipeline and GPU command succeeded without
an error, and all literal, input-buffer, signature and unwritten-guard controls
matched. This probe compiled authored source through the Mac legacy runtime;
it did not load a Windows-built library.

| Macro input | `uint(value)` | `sizeof(value)` bytes | `-1 < value` |
| --- | --- | --- | --- |
| `numberWithUnsignedInt(0xffffffff)` | 4294967295 | 8 | 1 |
| NSString `"4294967295"` | 4294967295 | 8 | 1 |
| NSString `"4294967295u"` | 4294967295 | 4 | 0 |

The NSNumber getter reported signed 64-bit storage (`objCType=q`) while
`unsignedIntValue` retained `ffffffff`; the authored constructor still took an
unsigned 32-bit input. Getter representation, the fixed string inputs and GPU
expressions remain separate observations. The equal uint conversions do not
erase the different size and comparison results. This is not general type
equivalence, a production fix, Metal 4 runtime or iPhone performance evidence.
Original reports are in `local/metal-compile/mac-run-06/evidence/macro-integers/`.

```sh
python3 experiments/metal-compile/macro_integer.py --output local/metal-compile/macro-integer-new --budget 90
```

```sh
python3 experiments/metal-compile/macro_value.py --output local/metal-compile/macro-value-new --budget 90 --timeout 30
```

`macro_fixture.metal` preserves an unsuccessful earlier method as an explicitly
negative fixture. Stringification worked, but `host_name` rejected the negative
and fractional tokens. This file is not an input to the diagnostic runner or an
expected-success shader build. Original failures remain in ignored local logs.

References:

- [Apple preprocessorMacros API](https://developer.apple.com/documentation/metal/mtlcompileoptions/preprocessormacros)
- [Apple library compilation API](https://developer.apple.com/documentation/metal/mtldevice/makelibrary(source:options:))
- [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
