/* SPDX-License-Identifier: GPL-3.0-only */
#define COBJMACROS
#include "d3d9_backend.h"
#include "cases.h"
#include <stdlib.h>
#include <string.h>

__declspec(dllimport) HRESULT __cdecl ReadDepthState(IDirect3DDevice9*, DWORD*);
_Static_assert(sizeof(void*) == 4, "Windows x86 oracle");
_Static_assert(sizeof(IDirect3DDevice9Vtbl) == CLASSIC_DEVICE9_SLOTS * 4, "D3D9 layout");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, AddRef) == CLASSIC_COM_ADDREF_SLOT * 4, "AddRef slot");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, Release) == CLASSIC_COM_RELEASE_SLOT * 4, "Release slot");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetRenderState) == CLASSIC_COM_GET_RENDER_STATE_SLOT * 4,
               "GetRenderState slot");
_Static_assert(D3DRS_ZENABLE == CLASSIC_RS_ZENABLE, "depth state enum");

struct Fixture {
    IDirect3DDevice9 view;
    ULONG refs;
    struct DepthCase data;
    char trace[5];
    size_t calls;
};
static void observe(struct Fixture* f, char method) {
    if (f->calls >= sizeof(f->trace) - 1)
        abort();
    f->trace[f->calls++] = method;
}
static ULONG STDMETHODCALLTYPE add(IDirect3DDevice9* self) {
    struct Fixture* f = (struct Fixture*)self;
    observe(f, 'A');
    return ++f->refs;
}
static ULONG STDMETHODCALLTYPE drop(IDirect3DDevice9* self) {
    struct Fixture* f = (struct Fixture*)self;
    observe(f, 'R');
    return --f->refs;
}
static HRESULT STDMETHODCALLTYPE query(IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD* out) {
    struct Fixture* f = (struct Fixture*)self;
    observe(f, 'Q');
    if (state != D3DRS_ZENABLE || !out)
        return D3DERR_INVALIDCALL;
    if (SUCCEEDED((HRESULT)f->data.hr))
        *out = (DWORD)f->data.depth;
    return (HRESULT)f->data.hr;
}

/* The system D3D9 device is an independent reference for the trusted native adapter. */
static int live_device(void) {
    HWND window = CreateWindowW(L"STATIC", L"Closed D3D9 adapter check", WS_POPUP, 0, 0, 32, 32, NULL, NULL,
                                GetModuleHandleW(NULL), NULL);
    IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = NULL;
    ClassicDeviceProxy proxy = {0};
    int ok = 0;
    if (!api || !window)
        goto end;
    D3DPRESENT_PARAMETERS present = {0};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_X8R8G8B8;
    present.hDeviceWindow = window;
    if (FAILED(IDirect3D9_CreateDevice(api, 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                       &present, &device)))
        goto end;
    uint8_t object[CLASSIC_DEVICE9_BYTES], stack[32] = {0}, out[4] = {0};
    ClassicGuestSpan view = {0x1000, object, sizeof object}, frame = {0x2000, stack, sizeof stack};
    ClassicGuestSpan result = {0x3000, out, sizeof out};
    if (classic_device_open(&proxy, classic_d3d9_depth_backend(device), view, frame, result) !=
        CLASSIC_DEVICE_OK)
        goto end;
    const uint32_t call[] = {0x12345678, view.base, D3DRS_ZENABLE, result.base};
    memcpy(stack, call, sizeof call);
    for (unsigned setting = 0; setting < 2; ++setting) {
        DWORD via_dll = 17;
        if (FAILED(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, setting)) ||
            FAILED(ReadDepthState(device, &via_dll)) || via_dll != setting)
            goto end;
        uint32_t sp = frame.base, eax = 0xf00d, via_proxy = 0xffffffff;
        if (classic_device_call(&proxy, CLASSIC_COM_GET_RENDER_STATE_TOKEN, &sp, &eax) !=
                CLASSIC_DEVICE_OK ||
            sp != frame.base + sizeof call || eax)
            goto end;
        memcpy(&via_proxy, out, sizeof via_proxy);
        if (via_dll != via_proxy)
            goto end;
    }
    ok = 1;
end:
    classic_device_close(&proxy);
    if (device)
        IDirect3DDevice9_Release(device);
    if (api)
        IDirect3D9_Release(api);
    if (window)
        DestroyWindow(window);
    return ok;
}

int main(void) {
    IDirect3DDevice9Vtbl table = {0};
    table.AddRef = add;
    table.Release = drop;
    table.GetRenderState = query;
    for (unsigned i = 0; i < CASES; ++i) {
        struct Fixture fake = {0};
        fake.view.lpVtbl = &table;
        fake.refs = 1;
        fake.data = cases[i];
        DWORD output = INITIAL_WORD;
        HRESULT hr = ReadDepthState(cases[i].flags & NO_DEVICE ? NULL : &fake.view,
                                    cases[i].flags & NO_OUTPUT ? NULL : &output);
        report(i, (uint32_t)hr, (uint32_t)output, (uint32_t)fake.refs, fake.trace);
    }
    if (!live_device()) {
        fputs("Windows native D3D9 device state verification failed\n", stderr);
        return 1;
    }
    return 0;
}
