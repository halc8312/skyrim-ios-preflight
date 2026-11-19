/* SPDX-License-Identifier: GPL-3.0-only */
#include "postprocess.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#if defined(POSTFX_NATIVE_DXVK)
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_vulkan.h>
#endif

namespace {
void checked(HRESULT result, const char* action) {
    if (FAILED(result)) {
        char code[18];
        std::snprintf(code, sizeof code, " (0x%08x)", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(action) + code);
    }
}
#define TRY(call) checked((call), #call)
void expect(bool ok, const char* action) {
    if (!ok)
        throw std::runtime_error(action);
}

struct Window {
    HWND native = nullptr;
#if defined(POSTFX_NATIVE_DXVK)
    SDL_Window* sdl = nullptr;
    Window() {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) || SDL_Vulkan_LoadLibrary(nullptr))
            throw std::runtime_error(SDL_GetError());
        sdl = SDL_CreateWindow("D3D9 color test", 0, 0, 64, 48, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!sdl)
            throw std::runtime_error(SDL_GetError());
        native = reinterpret_cast<HWND>(sdl);
    }
    ~Window() {
        SDL_DestroyWindow(sdl);
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
    }
#else
    Window() {
        native = CreateWindowW(L"STATIC", L"D3D9 color test", WS_POPUP, 0, 0, 64, 48, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        expect(native != nullptr, "hidden Win32 window");
    }
    ~Window() {
        if (native)
            DestroyWindow(native);
    }
#endif
};

constexpr DWORD rgba[] = {0x23281030, 0xb4f0c060, 0x7f20ea10, 0xddcc4c20};
unsigned quadrant(unsigned x, unsigned y, unsigned w, unsigned h) {
    return (y >= h / 2 ? 2u : 0u) + (x >= w / 2 ? 1u : 0u);
}
void fillScene(IDirect3DDevice9* d, IDirect3DSurface9* surface, unsigned w, unsigned h) {
    for (unsigned i = 0; i < 4; ++i) {
        RECT area = {LONG(i & 1 ? w / 2 : 0), LONG(i & 2 ? h / 2 : 0), LONG(i & 1 ? w : w / 2),
                     LONG(i & 2 ? h : h / 2)};
        TRY(d->ColorFill(surface, &area, rgba[i]));
    }
}
void checkPixels(IDirect3DDevice9* d, IDirect3DSurface9* output, const D3DSURFACE_DESC& format,
                 bool processed) {
    PostFXRef<IDirect3DSurface9> resolved, host;
    IDirect3DSurface9* src = output;
    if (format.MultiSampleType != D3DMULTISAMPLE_NONE) {
        TRY(d->CreateRenderTarget(format.Width, format.Height, format.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                  resolved.put(), nullptr));
        TRY(d->StretchRect(output, nullptr, resolved.get(), nullptr, D3DTEXF_NONE));
        src = resolved.get();
    }
    TRY(d->CreateOffscreenPlainSurface(format.Width, format.Height, format.Format, D3DPOOL_SYSTEMMEM,
                                       host.put(), nullptr));
    TRY(d->GetRenderTargetData(src, host.get()));
    D3DLOCKED_RECT pixels{};
    TRY(host->LockRect(&pixels, nullptr, D3DLOCK_READONLY));
    bool good = true;
    for (unsigned y = 0; good && y < format.Height; ++y) {
        auto* row = static_cast<const char*>(pixels.pBits) + y * pixels.Pitch;
        for (unsigned x = 0; x < format.Width; ++x) {
            DWORD expected = rgba[quadrant(x, y, format.Width, format.Height)];
            if (processed) {
                unsigned luma4 = ((expected >> 16) & 255) + 2 * ((expected >> 8) & 255) + (expected & 255);
                expected = (expected & 0xff000000) | (luma4 >= 510 ? 0x00ffff00 : 0x000000ff);
            }
            DWORD got;
            std::memcpy(&got, row + x * 4, 4);
            if (format.Format == D3DFMT_X8R8G8B8) {
                got |= 0xff000000;
                expected |= 0xff000000;
            }
            if (got != expected) {
                std::fprintf(stderr, "pixel %u,%u of %ux%u samples=%u got %08x expected %08x\n", x, y,
                             format.Width, format.Height, unsigned(format.MultiSampleType), unsigned(got),
                             unsigned(expected));
                good = false;
                break;
            }
        }
    }
    TRY(host->UnlockRect());
    expect(good, processed ? "two-tone GPU readback" : "source GPU readback");
}

// Deliberately hostile state. The pass must repaint the complete target and
// leave the game's own viewport, shaders, MRT, stream and draw masks intact.
struct GameState {
    PostFXRef<IDirect3DSurface9> main, extra, depth;
    PostFXRef<IDirect3DTexture9> texture;
    PostFXRef<IDirect3DVertexBuffer9> stream;
    PostFXRef<IDirect3DVertexDeclaration9> declaration;
    PostFXRef<IDirect3DVertexShader9> vertexShader;
    PostFXRef<IDirect3DPixelShader9> pixelShader;
    D3DVIEWPORT9 viewport = {3, 5, 27, 15, 0.25f, 0.75f};
    RECT scissor = {5, 6, 17, 14};
    float constants[4] = {0.14f, 0.27f, 0.42f, 0.61f};
    bool hasExtra = false;
    GameState(IDirect3DDevice9* d, bool multiple) {
        hasExtra = multiple;
        TRY(d->CreateRenderTarget(48, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, main.put(),
                                  nullptr));
        if (multiple)
            TRY(d->CreateRenderTarget(48, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, extra.put(),
                                      nullptr));
        TRY(d->CreateDepthStencilSurface(48, 32, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, depth.put(),
                                         nullptr));
        TRY(d->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, texture.put(), nullptr));
        TRY(d->CreateVertexBuffer(256, 0, 0, D3DPOOL_DEFAULT, stream.put(), nullptr));
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        TRY(d->CreateVertexDeclaration(elements, declaration.put()));
        // Original sentinel shaders: forward position and output constant c0.
        const DWORD vs[] = {0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000,
                            0x02000001, 0xc00f0000, 0x90e40000, 0x0000ffff};
        const DWORD ps[] = {0xffff0200, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff};
        TRY(d->CreateVertexShader(vs, vertexShader.put()));
        TRY(d->CreatePixelShader(ps, pixelShader.put()));
        TRY(d->SetDepthStencilSurface(nullptr));
        TRY(d->SetRenderTarget(0, main.get()));
        if (multiple)
            TRY(d->SetRenderTarget(1, extra.get()));
        TRY(d->SetDepthStencilSurface(depth.get()));
        TRY(d->SetViewport(&viewport));
        TRY(d->SetScissorRect(&scissor));
        TRY(d->SetStreamSource(0, stream.get(), 12, 24));
        TRY(d->SetVertexDeclaration(declaration.get()));
        TRY(d->SetVertexShader(vertexShader.get()));
        TRY(d->SetPixelShader(pixelShader.get()));
        TRY(d->SetPixelShaderConstantF(0, constants, 1));
        TRY(d->SetTexture(0, texture.get()));
        TRY(d->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
        TRY(d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_NEVER));
        TRY(d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN));
        TRY(d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
        TRY(d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
        TRY(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME));
        TRY(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        TRY(d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
        TRY(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        TRY(d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1));
        TRY(d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2));
    }
    void verify(IDirect3DDevice9* d) const {
        PostFXRef<IDirect3DSurface9> s;
        TRY(d->GetRenderTarget(0, s.put()));
        expect(s.get() == main.get(), "original primary render target");
        if (hasExtra) {
            TRY(d->GetRenderTarget(1, s.put()));
            expect(s.get() == extra.get(), "original second render target");
        }
        TRY(d->GetDepthStencilSurface(s.put()));
        expect(s.get() == depth.get(), "original depth/stencil surface");
        D3DVIEWPORT9 vp{};
        RECT clip{};
        TRY(d->GetViewport(&vp));
        TRY(d->GetScissorRect(&clip));
        expect(!std::memcmp(&vp, &viewport, sizeof vp) && !std::memcmp(&clip, &scissor, sizeof clip),
               "viewport/scissor restore");
        PostFXRef<IDirect3DVertexBuffer9> vb;
        UINT offset = 0, stride = 0, frequency = 0;
        TRY(d->GetStreamSource(0, vb.put(), &offset, &stride));
        TRY(d->GetStreamSourceFreq(0, &frequency));
        expect(vb.get() == stream.get() && offset == 12 && stride == 24 && frequency == 1,
               "stream/offset/stride/frequency restore");
        PostFXRef<IDirect3DVertexDeclaration9> dec;
        TRY(d->GetVertexDeclaration(dec.put()));
        expect(dec.get() == declaration.get(), "custom vertex declaration restore");
        PostFXRef<IDirect3DVertexShader9> vs;
        PostFXRef<IDirect3DPixelShader9> ps;
        TRY(d->GetVertexShader(vs.put()));
        TRY(d->GetPixelShader(ps.put()));
        expect(vs.get() == vertexShader.get() && ps.get() == pixelShader.get(), "bound shader restore");
        PostFXRef<IDirect3DBaseTexture9> tx;
        TRY(d->GetTexture(0, tx.put()));
        expect(tx.get() == texture.get(), "texture restore");
        float f[4];
        TRY(d->GetPixelShaderConstantF(0, f, 1));
        expect(!std::memcmp(f, constants, sizeof f), "shader constants restore");
        auto render = [&](D3DRENDERSTATETYPE state, DWORD value) {
            DWORD actual;
            TRY(d->GetRenderState(state, &actual));
            expect(actual == value, "render-state restore");
        };
        render(D3DRS_ALPHATESTENABLE, TRUE);
        render(D3DRS_ALPHAFUNC, D3DCMP_NEVER);
        render(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN);
        render(D3DRS_SCISSORTESTENABLE, TRUE);
        render(D3DRS_ZENABLE, D3DZB_TRUE);
        render(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        render(D3DRS_ALPHABLENDENABLE, TRUE);
        DWORD item = 0;
        TRY(d->GetSamplerState(0, D3DSAMP_ADDRESSU, &item));
        expect(item == D3DTADDRESS_WRAP, "sampler address restore");
        TRY(d->GetSamplerState(0, D3DSAMP_MAGFILTER, &item));
        expect(item == D3DTEXF_LINEAR, "sampler filter restore");
        TRY(d->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &item));
        expect(item == 1, "texture coordinate restore");
        TRY(d->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &item));
        expect(item == D3DTTFF_COUNT2, "texture transform restore");
    }
};

void run() {
    Window window;
    PostFXRef<IDirect3D9> d3d;
    *d3d.put() = Direct3DCreate9(D3D_SDK_VERSION);
    expect(d3d.get() != nullptr, "Direct3DCreate9");
    D3DADAPTER_IDENTIFIER9 adapter{};
    TRY(d3d->GetAdapterIdentifier(0, 0, &adapter));
    std::printf("D3D9 adapter %s\n", adapter.Description);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window.native;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 48;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    PostFXRef<IDirect3DDevice9> device;
    TRY(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window.native, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                          device.put()));
    IDirect3DDevice9* d = device.get();
    D3DCAPS9 caps{};
    TRY(d->GetDeviceCaps(&caps));
    TwoTonePass pass;
    unsigned cases = 0, pixels = 0, multisample = 0;
    PostFXRef<IDirect3DSurface9> target;
    auto frame = [&](unsigned w, unsigned h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE samples,
                     const GameState* game) {
        TRY(d->CreateRenderTarget(w, h, fmt, samples, 0, FALSE, target.put(), nullptr));
        D3DSURFACE_DESC desc{};
        TRY(target->GetDesc(&desc));
        fillScene(d, target.get(), w, h);
        checkPixels(d, target.get(), desc, false);
        TRY(pass.apply(d, target.get()));
        if (game)
            game->verify(d);
        checkPixels(d, target.get(), desc, true);
        ++cases;
        pixels += w * h;
    };
    frame(32, 18, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, nullptr);
    {
        const DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
        TRY(d->SetFVF(fvf));
        PostFXRef<IDirect3DVertexDeclaration9> before, after;
        TRY(d->GetVertexDeclaration(before.put()));
        frame(13, 7, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, nullptr);
        DWORD restored = 0;
        TRY(d->GetFVF(&restored));
        TRY(d->GetVertexDeclaration(after.put()));
        expect(restored == fvf && before.get() == after.get(), "FVF and declaration identity restore");
    }
    {
        GameState game(d, caps.NumSimultaneousRTs > 1);
        game.verify(d);
        frame(32, 18, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, &game);
        frame(17, 13, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, &game);
        frame(16, 11, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, &game);
        for (auto samples : {D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES}) {
            if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, samples,
                                                          nullptr))) {
                frame(29, 19, D3DFMT_A8R8G8B8, samples, &game);
                ++multisample;
            }
        }
        D3DSURFACE_DESC desc{};
        TRY(target->GetDesc(&desc));
        fillScene(d, target.get(), desc.Width, desc.Height);
        TRY(d->BeginScene());
        HRESULT nested = pass.apply(d, target.get());
        TRY(d->EndScene());
        expect(FAILED(nested), "nested scene must reject the pass");
        game.verify(d);
        checkPixels(d, target.get(), desc, false);
        expect(FAILED(pass.apply(d, game.depth.get())), "depth surface must be rejected");
        game.verify(d);
        pass.release();
        frame(11, 7, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, &game);
    }
    {
        PostFXRef<IDirect3DSurface9> backbuffer;
        TRY(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backbuffer.put()));
        D3DSURFACE_DESC desc{};
        TRY(backbuffer->GetDesc(&desc));
        fillScene(d, backbuffer.get(), desc.Width, desc.Height);
        checkPixels(d, backbuffer.get(), desc, false);
        TRY(pass.apply(d, backbuffer.get()));
        checkPixels(d, backbuffer.get(), desc, true);
        TRY(d->Present(nullptr, nullptr, nullptr, nullptr));
        ++cases;
        pixels += desc.Width * desc.Height;
    }
    std::printf("Verified %u color passes, %u output pixels, %u multisample modes; MRTs=%u\n", cases,
                pixels, multisample, unsigned(caps.NumSimultaneousRTs));
}
} // namespace
int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "D3D9 test failed: %s\n", e.what());
        return 1;
    }
    return 0;
}
