# Direct Metal 4 archive adoption boundary

Source review, 2026-11-18. This is a later experiment, separate from the
[library-compiler adapter](MOLTENVK_PLAN.md). No production patch or device
performance result is implied. The current direct archive probe has only passed
macOS/iPhone SDK compilation; its Metal 4 runtime cases remain unexecuted.

## Why a final-PSO hook is too late

The built MoltenVK revision is `458870543b9bcf6b0edd6f90aaa776707b310d96`.
In `MVKPipeline.mm:946–980`, `newMTLRenderPipelineDescriptor` first converts and
obtains the vertex function, builds vertex input using the resulting configuration,
and obtains the fragment function. Only then is its final descriptor available.
Adding archive lookup at `getOrCompilePipeline` therefore cannot avoid those
earlier library and function operations.

Moving the lookup earlier is a larger change than selecting another compiler:

| Existing dependency | Required evidence before bypassing it |
| --- | --- |
| `addVertexShaderToPipeline:1228–1288` obtains both the function and conversion results | Restore the exact rasterization decision, implicit-buffer requirements, physical-address capability and binding-use information. A PSO alone does not carry MoltenVK's host bookkeeping. |
| Vertex input is processed before fragment conversion | Preserve that ordering and the updated conversion configuration; do not build an independent descriptor from the original Vulkan state alone. |
| `MVKShaderLibrary::getMTLFunction:113–174` reads Metal's function-constant reflection | Preserve index, type, supplied bytes and omission behavior. Raw `VkSpecializationMapEntry::size` is not a substitute for `MTLFunctionConstant.type`. The fixed code does not inspect `mfc.required`; recording and validating required constants is a new archive-capture requirement, not existing host validation. |
| `MVKPipelineCache::readData:2629–2685` reconstructs libraries eagerly | A new PSO lookup does not remove source-library work already performed during cache restoration. Avoiding that startup cost requires separately designed lazy materialization or a different validated metadata route. |
| Existing cache matching and compile-required feedback | Preserve conversion-option matching, failure behavior and `FAIL_ON_PIPELINE_COMPILE_REQUIRED`; an archive miss must not silently count as an application-cache hit. |

These are requirements inferred from the pinned source, not proof that a
library-free route is impossible. The direct probe deliberately tests a much
smaller authored case before any such refactoring.

## Descriptor mapping must be explicit

Metal 4 uses function descriptors rather than already-created `MTLFunction`
objects. Its render descriptor also changes several state fields. A first
adapter must enumerate supported state rather than assume structure equivalence:

- Preserve the vertex descriptor, topology, sample count, rasterization state,
  all effective color formats, per-channel write masks, blend factors/operations,
  attachment remapping and special empty-attachment behavior.
- Translate alpha-to-coverage, alpha-to-one and blend booleans to their Metal 4
  state enums. Do not select an unspecialized state merely because it is available.
- The documented Metal 4 render descriptor does not expose the same depth/stencil
  format fields or tessellation settings as the legacy descriptor. Establish
  behavior with actual depth/stencil, sample-count and legacy-encoder readbacks;
  do not silently drop fields during conversion. Unsupported modes keep the
  existing path before starting a new compilation.
- Leave Metal 4 command queues, argument tables and resource-lifetime changes
  out of this experiment. A common `MTLRenderPipelineState` type is not by itself
  proof that the selected pipeline and old encoder combination preserves every
  required rendering mode.

The fixed source writes the relevant output state in
`MVKPipeline.mm:1781–1879`; the vertex/fragment bookkeeping is at
`:1228–1288` and `:1469–1547`. Private-API-only sample-mask/logic-op branches
must remain outside the public-API experiment, not be treated as checked support.

## Specialization and offline tool limits

`MTL4SpecializedFunctionDescriptor` supplies a base function descriptor and
function-constant values. The installed `metal4-pipelines-script(5)` describes
the corresponding `specialized_function_descriptors` and `constant_values`
fields. This schema is distinct from the legacy `.mtlp-json` schema.

Archive identity also needs the effective macro library variant. The fixed
`MVKShaderModule.mm:80–108` selects a separate library from macro values and
sizes before function-constant reflection. A root-library key plus function
constants alone cannot represent that prior specialization. Preserve the
original macro input and compile options without replacing NSNumber values
with different string tokens.

The first Windows 6.2 authored check used one compute function with uint index 0:
value 7 produced a 12,368-byte archive, and value 11 produced a different
12,384-byte archive, both for explicit `applegpu_g16p` / iOS 26. Those are
offline generation and architecture observations, not GPU output checks.

The omitted-value control failed its expected-rejection assertion: the tool
returned exit 0, warned that the required constant was unspecified, and emitted
an archive. Preserve that failed control. Do not interpret process success or
an archive's existence as complete specialization. Future capture/replay must
validate required constants before invoking the translator. The emitted
omitted-value program's runtime behavior has not been tested.

## Smallest useful runtime sequence

1. Run the existing recorded/unrecorded function probe on a capable device.
   Keep library-supplied and library-omitted cases in separate fresh processes.
   Reject an unexpected unrecorded success; preserve archive-load and lookup
   errors. Library-free means no app-created library is loaded or supplied,
   not proof that Metal does no internal compilation.
2. Add a specialization probe with one function and two constant values. Archive
   only value 7, verify its exact output, and distinguish value 11 and omitted
   required constants. Do not use different function names as a substitute for
   testing specialization identity. Retain the existing legacy specialization
   probe as an independent control.
3. Add a small render-state matrix: vertex input, blending/write masks,
   depth/stencil and MSAA. Require exact authored readbacks and explicit
   unsupported results before testing captured MoltenVK descriptors.
4. Only then design a default-off cache route with complete conversion metadata,
   an immutable format/version/device identity, bounded storage and preserved
   fallback semantics. Measure startup work as well as gameplay stalls; moving
   library compilation into startup is not eliminating it.

Primary references:

- [Pinned MoltenVK pipeline implementation](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm)
- [Pinned shader implementation](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm)
- [Metal 4 render descriptor](https://developer.apple.com/documentation/metal/mtl4renderpipelinedescriptor)
- [Metal 4 specialized function descriptor](https://developer.apple.com/documentation/metal/mtl4specializedfunctiondescriptor)
- [Metal 4 compilation workflow](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api)
- [Metal 4 core API and ownership differences](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)
