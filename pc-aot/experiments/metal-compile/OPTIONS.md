# Isolated compile-option snapshots

`options_probe.py` builds and runs authored `MTLCompileOptions` objects on a Mac.
It creates no Metal device, source library, pipeline or command queue. There is
no shader source argument or game input. macOS 13.3 or later and SDK 15 or later
are required; Metal 4 hardware is unnecessary.

```sh
python3 experiments/metal-compile/options_probe.py --output local/metal-options/new-run --budget 180
python -m unittest discover -s tests -p test_metal_compile_options.py -v
```

The runner requires a fresh output directory, preserves its sources, tool logs
and failures, and gives each child the remaining total budget. Successful native
execution means option getters were observed. It does not establish shader
compilation, GPU behavior, Windows argument equivalence or production parity.

The snapshot includes language version, the two modern math options and legacy
`fastMathEnabled`, invariance, optimization, preprocessor macros, library type,
symbol visibility, logging, install name, linked-library count, undefined-symbol
policy and threadgroup limits. The language enum is retained raw; only the
explicitly exercised 2.4 value is normalized to a text version.

Each field distinguishes a supported getter from an unavailable SDK or OS API.
The Apple documentation places the modern math properties and logging at macOS
15 / iOS 18, required threadgroup size at 26, and floating-point conversion
rounding mode at 27. The latter is deliberately unavailable with a 26 SDK.

Fresh objects record `default_observed`. Other cases keep their explicit setter
list separately. Unassigned fields are not inferred to be defaults; math values
affected by a legacy setter are identified separately. These are facts about
the authored fixture, not reconstructed assignment history of game requests.

Macro snapshots preserve the returned NSNumber `objCType`, observed storage
width, integer decimal strings and bit patterns, or floating-point bit patterns
and negative zero. The original authored constructor type and width are kept in
a separate declaration table. NSNumber may normalize its representation; its
getter type does not recover the original declaration. Value loss, if observed,
is reported rather than silently repaired. Neither these bit patterns nor
NSNumber's type prove how Metal converts the value to preprocessor text.

Nil and empty macro/library collections remain distinct. Nonempty linked
libraries have no established binary identity in this probe and cannot be
declared equivalent. Unsupported macro classes are explicit failures of value
capture. No fallback converts an unknown option to a presumed default.

Fixed MoltenVK `458870543b9bcf6b0edd6f90aaa776707b310d96` sets language, modern
math or legacy fast-math, optimization and invariance in `MVKDevice.mm`
`getMTLCompileOptions` (lines 5217–5268). `MVKShaderModule.mm` (lines 559–647)
constructs specialization macro NSNumbers separately, using signed/unsigned
integer widths or float/double constructors before submitting the library.
This probe does not call those functions, reproduce their runtime inputs or
claim that an authored case represents a game shader.

Primary references:

- [Apple MTLCompileOptions](https://developer.apple.com/documentation/metal/mtlcompileoptions)
- [Modern and legacy math coupling](https://developer.apple.com/documentation/metal/mtlcompileoptions/mathmode)
- [Macro value types](https://developer.apple.com/documentation/metal/mtlcompileoptions/preprocessormacros)
- [Linked dynamic libraries](https://developer.apple.com/documentation/metal/mtlcompileoptions/libraries)
- [Required threadgroup size](https://developer.apple.com/documentation/metal/mtlcompileoptions/requiredthreadsperthreadgroup)
- [Conversion rounding mode](https://developer.apple.com/documentation/metal/mtlcompileoptions/floatingpointconversionroundingmode)
