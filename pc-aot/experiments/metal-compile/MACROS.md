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
KeyboardInterrupt/SystemExit, and atomic report replacement. Native Objective-C++ compilation and NSNumber diagnostics
are pending an Apple-host run. No game data or production capture is involved.

`macro_fixture.metal` preserves an unsuccessful earlier method as an explicitly
negative fixture. Stringification worked, but `host_name` rejected the negative
and fractional tokens. This file is not an input to the diagnostic runner or an
expected-success shader build. Original failures remain in ignored local logs.

References:

- [Apple preprocessorMacros API](https://developer.apple.com/documentation/metal/mtlcompileoptions/preprocessormacros)
- [Apple library compilation API](https://developer.apple.com/documentation/metal/mtldevice/makelibrary(source:options:))
- [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
