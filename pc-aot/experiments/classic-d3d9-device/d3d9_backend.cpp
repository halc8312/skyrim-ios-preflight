/* SPDX-License-Identifier: GPL-3.0-only */
#include "d3d9_backend.h"

namespace {
void retain(void* object) { static_cast<IDirect3DDevice9*>(object)->AddRef(); }
void release(void* object) { static_cast<IDirect3DDevice9*>(object)->Release(); }

uint32_t depth_state(void* object, uint32_t* out) {
    DWORD value = 0;
    HRESULT result = static_cast<IDirect3DDevice9*>(object)->GetRenderState(D3DRS_ZENABLE, &value);
    if (SUCCEEDED(result))
        *out = static_cast<uint32_t>(value);
    return static_cast<uint32_t>(result);
}
} // namespace

ClassicDeviceBackend classic_d3d9_depth_backend(IDirect3DDevice9* device) {
    return {device, retain, release, depth_state};
}
