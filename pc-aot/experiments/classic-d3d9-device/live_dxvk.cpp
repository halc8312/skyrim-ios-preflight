/* SPDX-License-Identifier: GPL-3.0-only */
#include "d3d9_backend.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstdio>

extern "C" uint32_t classic_owned_depth_read(ClassicDeviceBackend, uint32_t*);

static bool compare_depth(IDirect3DDevice9* device) {
    DWORD original;
    if (FAILED(device->GetRenderState(D3DRS_ZENABLE, &original)))
        return false;
    bool ok = true;
    for (DWORD requested = 0; requested < 2; ++requested) {
        DWORD actual = 42;
        uint32_t through_guest = 43;
        if (FAILED(device->SetRenderState(D3DRS_ZENABLE, requested)) ||
            FAILED(device->GetRenderState(D3DRS_ZENABLE, &actual)) ||
            classic_owned_depth_read(classic_d3d9_depth_backend(device), &through_guest) ||
            actual != requested || through_guest != actual) {
            ok = false;
            break;
        }
    }
    return SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE, original)) && ok;
}

int main() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        std::fprintf(stderr, "SDL Vulkan initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("Owned D3D9 state", 0, 0, 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = nullptr;
    bool ok = false;
    if (window && api) {
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = reinterpret_cast<HWND>(window);
        p.BackBufferFormat = D3DFMT_X8R8G8B8;
        p.BackBufferWidth = 32;
        p.BackBufferHeight = 32;
        if (SUCCEEDED(api->CreateDevice(0, D3DDEVTYPE_HAL, p.hDeviceWindow,
                                        D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &device)))
            ok = compare_depth(device);
    }
    if (device)
        device->Release();
    if (api)
        api->Release();
    if (window)
        SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    if (!ok) {
        std::fputs("Translated caller did not obtain both real DXVK values\n", stderr);
        return 1;
    }
    std::puts("Translated i386 caller obtained depth off/on from the actual DXVK D3D9 device");
    return 0;
}
