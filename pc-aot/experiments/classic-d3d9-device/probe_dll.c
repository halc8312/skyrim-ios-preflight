/* SPDX-License-Identifier: GPL-3.0-only */
/* The only PE input accepted by the comparison is compiled from this file. */
#define COBJMACROS
#include <d3d9.h>

/* Both the access to the pointer and its pointee must survive PE32 rebasing. */
static const D3DRENDERSTATETYPE depth_state = D3DRS_ZENABLE;
static const D3DRENDERSTATETYPE* volatile depth_state_ptr = &depth_state;

__declspec(dllexport) HRESULT __cdecl ReadDepthState(IDirect3DDevice9* device, DWORD* output) {
    if (!device)
        return D3DERR_INVALIDCALL;
    IDirect3DDevice9_AddRef(device);
    HRESULT hr = IDirect3DDevice9_GetRenderState(device, *depth_state_ptr, output);
    IDirect3DDevice9_Release(device);
    return hr;
}
