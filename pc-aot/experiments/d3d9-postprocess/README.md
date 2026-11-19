# D3D9 two-tone post-processing experiment

The probe filters a completed, synthetic D3D9 scene just before `Present`. The authored pixel shader chooses blue or yellow from the sampled luminance and preserves alpha. It uses only public D3D9 interfaces. It runs separately from SkyrimIOS and has no game, ENB or mod inputs.

## Run the pixel and state checks

On Windows with the Windows SDK, the default build uses the system D3D9 implementation.

```sh
cmake -S experiments/d3d9-postprocess -B out/postfx
cmake --build out/postfx --config Release
ctest --test-dir out/postfx -C Release -V
```

For the port's native DXVK path, configure with `-DSKYDREAM_SOURCE=/path/to/skydream` at commit `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a` and a native SDL2 package. An explicit SDL2 setup can use `-DPOSTFX_SDL_INCLUDE=/path/to/include/SDL2 -DPOSTFX_SDL_LIBRARY=/path/to/libSDL2`. The host needs a Vulkan-capable SDL window and an available Vulkan driver. This builds the pinned DXVK directly from its CMake target. The host probe does not apply the app's iOS DXVK patches.

The probe fills four colored quadrants, verifies the input, runs the pass, then reads back every output pixel. It exercises A8R8G8B8 and X8R8G8B8, odd dimensions, resizing, resource recreation and each advertised 2×/4× MSAA mode. It restores a custom vertex declaration, FVF, shaders, texture bindings, viewport, scissor, vertex stream, render targets, depth and render states. The last test uses the actual device backbuffer and calls `Present`. Both an unsupported depth target and an invocation during a scene must fail.

Windows D3D9 on an RTX 4060 Ti passed 9 draws and 5,891 output pixels, including both 2× and 4× MSAA. Pinned DXVK 1.9.2 on Linux with Mesa lavapipe passed 8 draws and 5,340 pixels, including its advertised 4× mode. A direct draw into the multisampled target left some unfiltered subsamples at the top and left edges on DXVK. The pass now processes the resolved single-sample image and transfers the complete filtered result back to the MSAA target.

## Reproduce the shader checks

Microsoft FXC in the Windows SDK can regenerate all 45 embedded shader DWORDs from `two_tone.hlsl`.

```sh
python experiments/d3d9-postprocess/verify_shaders.py hlsl --fxc /path/to/fxc.exe
```

To check the Apple shader path, run the DXVK probe with `DXVK_SHADER_DUMP_PATH` pointing to a fresh, empty directory. Provide its two shaders, SPIRV-Cross at `0a88b2d5` with `ios/dependencies/patch_spirv_cross.py` applied, and Apple's Metal tools. Specify a new output directory.

```sh
python experiments/d3d9-postprocess/verify_shaders.py metal \
    --dump /path/to/dump --cross /path/to/spirv-cross \
    --tools /path/to/metal/bin --output /path/to/new-result
```

Apple's Windows toolchain produced an iOS 18, MSL 3.2 library with both vertex and fragment entry points. It reported one unused generated specialization constant in each stage. This offline check uses the shaders that DXVK dumps before its pipeline-specific binding remap and uses explicit SPIRV-Cross options. The actual MoltenVK pipeline, Apple GPU readback, iPhone frame cost and ENB preset or binary compatibility remain untested.

## Integration boundary

The pinned bridge has direct device and swap-chain `Present` paths. Their iOS adapters are in `ios/runtime-port/prepare_runtime.py`. A future opt-in trial can select the presenting chain's backbuffer, run `TwoTonePass::apply` once before `Present`, and release its cached resources before reset or device teardown. The common case adds one scene copy, one fullscreen draw and one full state capture/restore per frame. An MSAA backbuffer adds one single-sample output surface and a GPU transfer. The smallest next check is identical final pixels on a real MoltenVK device, followed by GPU time with the effect on and off.
