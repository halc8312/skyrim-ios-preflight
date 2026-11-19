# A 32-bit D3D9 device boundary

This standalone experiment exposes three virtual methods to one statically translated, self-authored Windows x86 DLL. The guest holds a 32-bit device reference. It can retain that reference, read the `D3DRS_ZENABLE` value, and release its reference. Its result includes the original 32-bit `HRESULT`.

## Which parts are shared with the port?

`ios/private-abi/PcGuestABI.h` binds trusted host callbacks to the AOT module, including dispatch for indirect calls. Its `cpu_layout` is checked across the native host/guest builds. In the pinned Skydream source, `runtime/d3d9_bridge.cpp` builds guest vtables from thunk addresses and keeps the native interfaces in a host registry. `runtime/d3d9_bridge_gen.cpp` implements the game-facing `GetRenderState` using `GPTR` for the caller's output. The original game's helpers assume the trusted game memory layout.

The experiment uses the same x86 stdcall contract and the fixed Windows D3D9 vtable layout. The Windows SDK confirms the 119 slots and the byte offsets of `AddRef` (4), `Release` (8) and `GetRenderState` (232). The fixture calls ordinary `IDirect3DDevice9` methods. `device_bridge.c` recognizes those three guest tokens and validates the receiver, complete stack frame and four-byte output. In the trusted backend, the native D3D9 call writes to a local `DWORD`. On success, the adapter copies its little-endian value into the approved guest output region. Failed calls preserve the previous contents.

The guest owns proxy references, with one corresponding retained native reference over the proxy's lifetime. A known token plus a forged or expired receiver is rejected. The vtable, output and stack occupy disjoint guest ranges with disjoint native backing. A fresh instance owns their mapping and must serialize calls and retire it before exposing another binding at the same guest address. This experiment passes only its pinned DLL to AOT. The fixed title lifter's general memory reads assume trusted code, so exposing all title thunks or the whole game memory to external modules would require a separate isolation design.

## Reproduce the checks

From `pc-aot`, build the authored i386 DLL on Windows with LLVM 19.1.1 and the Windows SDK. The test checks its exact size, hash and native results. The executable also compares the DLL and the typed backend against a real Windows D3D9 device with depth disabled and enabled.

```sh
python experiments/classic-d3d9-device/compare.py windows /path/to/new-windows-run
```

On Linux, use the project's pinned translation Python, the same Windows result directory, and a Skydream checkout containing `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a`. Each run needs a new output directory. The fixture has a single export and two HIGHLOW relocations. Skydream's own PE image loader, discovery and AOT stages apply both relocations at each chosen base. The comparisons cover native status, result, method order, refcount and preservation of the x86 caller's registers and stack.

```sh
python experiments/classic-d3d9-device/compare.py aot /path/to/new-aot \
    --reference /path/to/new-windows-run --skydream /path/to/skydream --cc gcc --sanitize
```

The CMake default runs the portable boundary checks. To additionally execute the translated callers on a real native DXVK device, pass the AOT output, the native DXVK library built by the neighboring `d3d9-postprocess` experiment, and native SDL2. This mode exercises the entire path at both guest bases and compares depth off/on with direct D3D9 reads. CMake `ctest -V` reports each base separately.

```sh
cmake -S experiments/classic-d3d9-device -B /path/to/build \
    -DCLASSIC_AOT_ROOT=/path/to/new-aot -DSKYDREAM_SOURCE=/path/to/skydream \
    -DCLASSIC_DXVK_LIBRARY=/path/to/libdxvk_d3d9.so \
    -DCLASSIC_SDL_INCLUDE=/path/to/include/SDL2 -DCLASSIC_SDL_LIBRARY=/path/to/libSDL2.so
cmake --build /path/to/build
ctest --test-dir /path/to/build -V
```

Windows x86 and each of the `0x100000` and `0x200000` AOT builds matched all seven synthetic cases. Both translated layouts read the real depth state from fixed DXVK on Vulkan. On Windows the trusted backend and original x86 DLL also agreed on the real D3D9 device. The adapter, backend, two translated entry points and their harnesses compiled to six iOS 18 ARM64 Mach-O objects using public Darwin headers. The production title and its device handles stay outside the probe. Game-time callbacks, D3D9 alteration or proxy-library installation, third-party ENB binaries and Apple GPU execution remain separate work.
