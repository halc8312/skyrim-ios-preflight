# Warm display grade in the D3D9 presentation draw

This experiment adds one authored display-space color grade to the last DXVK draw of a presented image. It preserves alpha, slightly reduces saturation, and raises red relative to blue. Each presentation samples the game's original backbuffer, including the game HUD. The optional DXVK diagnostic HUD is drawn afterward. UIKit overlays also sit outside this pass. Scene lighting, materials, shadows and depth effects are outside its scope.

## Integration point and cost

Skydream's device and swapchain `Present` wrappers both reach `D3D9SwapChainEx::PresentImage`. DXVK already binds the game's backbuffer as input and a separate acquired image as output on the swapchain's private context. The grade runs after the existing sample, filter or sample average and before the game's optional gamma ramp. When a sync interval repeats an image, each repetition derives from the original backbuffer. [D3D9 bridge](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/d3d9_bridge.cpp#L496), [swapchain](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/third_party/dxvk/src/d3d9/d3d9_swapchain.cpp#L804), [present blitter](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/third_party/dxvk/src/dxvk/dxvk_swapchain_blitter.cpp#L23).

| Backbuffer | Existing presentation path | Extra work with the grade |
| --- | --- | --- |
| Single sample, same size | Fetch the source texel and draw | RGB arithmetic in the fragment |
| Single sample, scaled | Linearly sample the source and draw | Same arithmetic after filtering |
| MSAA, same size | Sample and average the covered samples in the presentation shader | Same arithmetic after averaging |
| MSAA, scaled | Resolve into DXVK's existing single-sample image, then scale and draw | Same arithmetic after filtering |

The shader uses a dot product, mix, three gains and a clamp. It adds zero scene images, image samples, copies, draws, render passes, queue submissions or game-state saves. Scaled MSAA retains DXVK's existing intermediate image. The selected four fragment arrays total 12,704 bytes of additional packaged SPIR-V. The tested unlinked arm64 object gained 12,895 bytes in `__TEXT`. The enabled mode can incur first-use shader compilation on the target device.

The earlier public D3D9 experiment copies the source into a readable texture before drawing and restores the game state afterward. The Classic COM experiment verifies a narrow interface to a D3D9 device. This grade executes entirely in the existing host presentation context and takes no plugin inputs. Compatibility with ENB binaries or presets is untested. [D3D9 post-processing](../d3d9-postprocess/README.md), [Classic D3D9 device](../classic-d3d9-device/README.md).

## Reproduce

Use the fixed Skydream checkout `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a`, Python 3.9+, glslangValidator, spirv-val, spirv-dis and native SDL2 with Vulkan. Run these commands from this directory. The generator checks the pinned fragment and C++ source contents and writes to a new directory outside the checkout.

```sh
export GRADE_DXVK=/path/to/skydream/third_party/dxvk
python3 prepare.py --dxvk "$GRADE_DXVK" --output /path/to/new-shaders
python3 -m unittest -v test_prepare
cmake -S . -B /path/to/new-build -DCMAKE_BUILD_TYPE=Release \
  -DGRADE_DXVK="$GRADE_DXVK" -DGRADE_GENERATED=/path/to/new-shaders \
  -DGRADE_SDL_INCLUDE=/path/to/include/SDL2 -DGRADE_SDL_LIBRARY=/path/to/libSDL2.so \
  -DGRADE_BUILD_STAGED_HOST=ON
cmake --build /path/to/new-build --parallel
ctest --test-dir /path/to/new-build --output-on-failure
/path/to/new-build/present_grade --bench
```

For a direct comparison with the original compiled DXVK library, set `GRADE_BUILD_STAGED_HOST=OFF` and supply `GRADE_DXVK_LIBRARY=/path/to/libdxvk_d3d9.so`. The source tests use `GRADE_GLSLANG` and `GRADE_SPIRV_DIS` when those tools live outside PATH. The generator accepts `--glslang` and `--spirv-val`.

To use a private iOS test target, prepare the shader directory from its DXVK source stage, include `staged_host.cmake` after creating the existing `dxvk_d3d9` target, then call `skyrim_trial_present_grade(dxvk_d3d9 "${_dxvk}" "/path/to/new-shaders")`. This replaces one source in that target. The iPhone dependency build normally consumes prebuilt shader headers. The staged host selects the new headers when `SKYRIM_TRIAL_PRESENT_GRADE=1` is present at swapchain blitter creation. Unset, `0`, or other values use the original checked-in SPIR-V. A change requires recreating the blitter, for example by relaunching the test app. [iPhoneOS dependency target](../../ios/dependencies/CMakeLists.txt).

The offline Apple shader check uses the project's fixed SPIRV-Cross and Apple's Metal developer tools. On Windows or in WSL, keep the result on a filesystem visible to the Windows executables.

```sh
python3 verify_ios.py --compiled /path/to/new-shaders --cross /path/to/spirv-cross \
  --metal-tools /path/to/metal/bin --output /path/to/new-ios-result
```

An optional physical-GPU benchmark uses the Python `vulkan==1.3.275.1` binding and the system Vulkan driver. It runs the fixed upstream vertex shader and same-size presentation fragments directly. It checks five pixels and alpha against a CPU reference after each batch, outside the two queue timestamps. The 4× case pre-fills two samples with the source color and two with the graded color before measuring.

```sh
python3 benchmark_vulkan.py --dxvk "$GRADE_DXVK" --compiled /path/to/new-shaders --samples 1
python3 benchmark_vulkan.py --dxvk "$GRADE_DXVK" --compiled /path/to/new-shaders --samples 4
```

## Verification

- Native DXVK on Mesa lavapipe (LLVM 20.1.2) passed 16 cases covering A8R8G8B8 and X8R8G8B8, cropped and scaled views, gamma off/on, single-sample and 4× MSAA with mixed per-sample colors. This adapter exposed 4× and skipped 2×. The original and disabled variants produced byte-identical output over 7,712 pixels. The enabled RGB outputs matched the CPU reference within two 8-bit levels, and A8 alpha matched exactly. Sampling the source again and querying deliberately non-default D3D9 state after drawing verified both were preserved.
- The complete scratch DXVK build passed both actual presentation entry points and the shader-selection checks for `1`, `0`, and `10`. Pixel reads use a readable destination drawn by the same blitter. They cover the shader result before the operating system's display compositor.
- Across those conditions every original, disabled and enabled presentation issued one draw, one render pass and one queue submission. The generated SPIR-V also keeps the original image-access instruction counts, bindings, push offsets and specialization IDs. The generator's failure cases preserve the input tree and do not publish a partial shader directory.
- In three consecutive software-renderer runs, the median paired queue-time increase was 92, 49 and 40 µs per draw at 480×270. The corresponding elapsed-time increases, including recording, submission and waiting for completion, were 101, 92 and 31 µs. Each run alternated 12 pairs of 16 draws after warm-up. These timings describe this host and workload.
- On a physical RTX 4060 Ti at 1920×1080, the median paired differences for the 1× fragment were +0.128, +0.096 and +0.103 µs per draw in three runs; the original took about 20.24 µs. With mixed 4× samples, the same-size resolve took about 151.25 µs and the differences were +0.029, +0.017 and −0.021 µs. The 4× changes fell within run-to-run variation. These are isolated Windows Vulkan timings.
- SPIRV-Cross and Apple's Metal compiler linked the three Apple-relevant fragment entry points into a 17,993-byte iOS 18 library. Clang also compiled the staged blitter against the app's prepared DXVK headers and public Darwin headers into an arm64 Mach-O object with iOS 18 deployment metadata. A signed iPhone run is still needed for output on the display, Metal GPU time at game resolution, first-use stalls and thermal behavior.
