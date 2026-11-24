# ENB effect execution check (Linux, software Vulkan)

`enb_exec_test` lowers authored ENB-style effect files (`ios/runtime-port/enb_fx.c`), compiles their passes with
libvkd3d-shader 2.0 and runs them with `EnbEffectExec` (`ios/runtime-port/enb_exec.cpp`) on the port's pinned native DXVK
D3D9 device. It reads every output pixel back and compares it with a CPU value. It has no ENB binary, preset or game input.

Checked: one pixel-shader pass with a constant that changes between runs; two passes with a vertex shader at full and at
half intermediate resolution; a second technique; a pass writing a named `RenderTarget64` that a later pass samples; an
unknown render-target name refused; a missing image reported (not a black frame); a scene in progress refused; a
non-render-target output refused; the game's render target, viewport, texture, blend and write-mask states, sampler and FVF
restored after every run; a shader that does not compile reported with its message and nothing kept; release and reload.

`EnbRuntime` (`ios/runtime-port/enb_runtime.cpp`) is checked on a preset folder the test writes itself (mixed-case names,
an `#include`, ini values for UI variables, a bloom stage whose image the main effect samples, a depth-of-field effect that
joins only when a depth image is given): the quality ladder (bloom at 1/1, 1/4 and dropped with a neutral image), the effect
chain, one broken effect switched off and named while the rest runs, and a missing include reported.

This is the pixel arithmetic and state handling on a software driver. It says nothing about Apple GPU speed, MoltenVK output
or real presets.

## Reproduce

```sh
apt-get install -y mesa-vulkan-drivers libsdl2-dev libvulkan-dev xvfb ninja-build cmake spirv-headers \
    flex bison autoconf automake libtool perl libjson-perl
# vkd3d 2.0 (same source the iOS dependency build uses, hash in ios/dependencies/build-dependencies.sh)
curl -LO https://dl.winehq.org/vkd3d/source/vkd3d-2.0.tar.xz
echo 9ad29bb236808186a47ec66e853b21e6fca59b9dc62a9474b05d5e3eda2710ef  vkd3d-2.0.tar.xz | sha256sum -c
tar xJf vkd3d-2.0.tar.xz && mkdir vkd3d-build && cd vkd3d-build
../vkd3d-2.0/configure --disable-doxygen-doc --disable-demos --disable-tests --without-xcb --without-ncurses \
    --without-opengl --without-spirv-tools
make -j4 include/private/vkd3d_version.h include/private/spirv_grammar.h libs/vkd3d-shader/hlsl.tab.h \
    libs/vkd3d-shader/preproc.tab.h && make -j4 libvkd3d-shader.la
cd ..
cmake -S pc-aot/experiments/enb-exec -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSKYDREAM_SOURCE=$PWD/pc-aot/vendor/skydream -DENB_VKD3D_INCLUDE=$PWD/vkd3d-2.0/include \
    -DENB_VKD3D_LIBRARY=$PWD/vkd3d-build/.libs/libvkd3d-shader.so
cmake --build build --parallel 4
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json SDL_VIDEODRIVER=x11 xvfb-run -a build/enb_exec_test
```

The libvkd3d-shader headers come from the source tree plus the generated `vkd3d_version.h` in the build tree's
`include/private`; if the compiler cannot find `vkd3d_types.h`, add `-I vkd3d-2.0/include` (it is in the source tree).
