/* SPDX-License-Identifier: GPL-3.0-only */
// Standalone D3D9 probe. Owns every drawn surface and never touches a game device.
#include "probe_checks.h"
#include <cstdlib>
#include <cstring>

namespace postfx_probe {
namespace {
constexpr unsigned width = 31, height = 17;
constexpr unsigned stencilStripeY = height / 2 + 1;
constexpr RECT interior = {1, 1, LONG(width) - 1, LONG(height) - 1};
constexpr D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr D3DFORMAT resz = static_cast<D3DFORMAT>(MAKEFOURCC('R', 'E', 'S', 'Z'));
constexpr D3DFORMAT colorFormat = D3DFMT_A8R8G8B8;
constexpr DWORD red = 0xffff0000, green = 0xff00ff00, blue = 0xff0000ff;

// FXC /T ps_2_0 /O3 depth_view.hlsl. Only the executable tokens are needed.
constexpr DWORD depthViewCode[] = {
    0xffff0200, 0x05000051, 0xa00f0000, 0x3f800000, 0x00000000, 0x00000000,
    0x00000000, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000,
    0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
    0x80070000, 0x80000000, 0x02000001, 0x80080000, 0xa0000000, 0x02000001,
    0x800f0800, 0x80e40000, 0x0000ffff};

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

Vertex vertex(float u, float v, float z, DWORD color) {
    return {u * width - 0.5f, v * height - 0.5f, z, 1.0f, color, u, v};
}

void triangle(IDirect3DDevice9* device, float z, DWORD color) {
    // Cover even the outermost MSAA subsamples. The scissor supplies the exact near/far split.
    const Vertex vertices[] = {vertex(-1, -1, z, color), vertex(4, -1, z, color), vertex(-1, 4, z, color)};
    TRY(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)));
}

void setSceneState(IDirect3DDevice9* device) {
    TRY(device->SetVertexShader(nullptr));
    TRY(device->SetPixelShader(nullptr));
    TRY(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
    TRY(device->SetTexture(0, nullptr));
    TRY(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    TRY(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    TRY(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    TRY(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    TRY(device->SetRenderState(D3DRS_LIGHTING, FALSE));
    TRY(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    TRY(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
    TRY(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
    TRY(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS));
    TRY(device->SetRenderState(D3DRS_STENCILENABLE, FALSE));
    TRY(device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffffu));
    TRY(device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE));
    TRY(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    TRY(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf));
}

void drawDepth(IDirect3DDevice9* device, bool multisampled) {
    setSceneState(device);
    // The one-pixel guard contains known clears at every sample. The interior
    // starts empty and must get its depth from real geometry.
    const D3DRECT leftClear = {0, 0, LONG(width / 2), LONG(height)};
    const D3DRECT insideClear = {interior.left, interior.top, interior.right, interior.bottom};
    const D3DRECT stencilStripe = {0, LONG(stencilStripeY), LONG(width), LONG(height)};
    TRY(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, red, 0.75f, 0x25));
    TRY(device->Clear(1, &leftClear, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, green, 0.25f, 0));
    TRY(device->Clear(1, &insideClear, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0));
    TRY(device->Clear(1, &stencilStripe, D3DCLEAR_STENCIL, 0, 1.0f, 0x52));
    TRY(device->BeginScene());
    TRY(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
    TRY(device->SetScissorRect(&interior));
    triangle(device, 0.75f, red);
    const RECT left = {interior.left, interior.top, LONG(width / 2), interior.bottom};
    TRY(device->SetScissorRect(&left));
    triangle(device, 0.25f, green);
    if (multisampled) {
        TRY(device->SetScissorRect(&interior));
        TRY(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0));
        TRY(device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xfffffffeu));
        triangle(device, 0.125f, blue);
        TRY(device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffffu));
        TRY(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf));
    }
    TRY(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    TRY(device->EndScene());
}

void resolveDepth(IDirect3DDevice9* device, IDirect3DTexture9* dst) {
    // Fixed DXVK handles this advertised vendor operation in SetRenderState.
    // No ordinary StretchRect from a depth surface into a texture is assumed.
    DWORD previous;
    TRY(device->GetRenderState(D3DRS_POINTSIZE, &previous));
    TRY(device->SetTexture(0, dst));
    TRY(device->SetRenderState(D3DRS_POINTSIZE, 0x7fa05000u));
    TRY(device->SetRenderState(D3DRS_POINTSIZE, previous));
    TRY(device->SetTexture(0, nullptr));
}

void visualizeDepth(IDirect3DDevice9* device, IDirect3DSurface9* picture, IDirect3DTexture9* source,
                    IDirect3DPixelShader9* shader) {
    TRY(device->SetDepthStencilSurface(nullptr));
    TRY(device->SetRenderTarget(0, picture));
    TRY(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xffff00ff, 1.0f, 0));
    TRY(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE));
    TRY(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    TRY(device->SetPixelShader(shader));
    TRY(device->SetTexture(0, source));
    TRY(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    TRY(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    TRY(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    TRY(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    TRY(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    TRY(device->BeginScene());
    triangle(device, 0, 0xffffffffu);
    TRY(device->EndScene());
    TRY(device->SetTexture(0, nullptr));
    TRY(device->SetPixelShader(nullptr));
}

void verifyPixels(IDirect3DDevice9* device, IDirect3DSurface9* source, DWORD left, DWORD right,
                  DWORD rightOutsideDraw) {
    PostFXRef<IDirect3DSurface9> host;
    TRY(device->CreateOffscreenPlainSurface(width, height, colorFormat, D3DPOOL_SYSTEMMEM, host.put(), nullptr));
    TRY(device->GetRenderTargetData(source, host.get()));
    D3DLOCKED_RECT locked{};
    TRY(host->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    unsigned failures = 0;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = static_cast<const unsigned char*>(locked.pBits) + y * locked.Pitch;
        for (unsigned x = 0; x < width; ++x) {
            DWORD pixel;
            std::memcpy(&pixel, row + x * 4, 4);
            const bool drawCanPass = x > 0 && y > 0 && x + 1 < width && y < stencilStripeY;
            const DWORD expectedRight = drawCanPass ? right : rightOutsideDraw;
            const DWORD expected = x < width / 2 ? left : expectedRight;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                const int error = int((pixel >> shift) & 255) - int((expected >> shift) & 255);
                if (std::abs(error) > (shift == 24 ? 0 : 1)) {
                    if (failures < 2 || (y == height / 2 && (x == width / 4 || x == width * 3 / 4)))
                        std::fprintf(stderr, "depth probe pixel (%u,%u): got %08x expected %08x\n",
                                     x, y, unsigned(pixel), unsigned(expected));
                    ++failures;
                    break;
                }
            }
        }
    }
    TRY(host->UnlockRect());
    if (failures)
        std::fprintf(stderr, "%u of %u pixels differed\n", failures, width * height);
    expect(!failures, "depth probe GPU readback");
}

void checkSourceAfterSampling(IDirect3DDevice9* device, IDirect3DSurface9* scene, IDirect3DSurface9* depth,
                              IDirect3DSurface9* singleColor, unsigned samples) {
    TRY(device->SetRenderTarget(0, scene));
    TRY(device->SetDepthStencilSurface(depth));
    setSceneState(device);
    TRY(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    TRY(device->SetRenderState(D3DRS_STENCILENABLE, TRUE));
    TRY(device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL));
    TRY(device->SetRenderState(D3DRS_STENCILREF, 0x25));
    TRY(device->SetScissorRect(&interior));
    TRY(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
    TRY(device->BeginScene());
    triangle(device, 0.5f, blue);
    TRY(device->EndScene());
    TRY(device->SetRenderState(D3DRS_STENCILENABLE, FALSE));
    TRY(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    // Only sample zero inside the right upper region passed both tests.
    // Other samples, the guard and the lower stencil stripe keep the original color.
    if (samples > 1) {
        TRY(device->StretchRect(scene, nullptr, singleColor, nullptr, D3DTEXF_NONE));
        const unsigned amountBlue = (255u + samples / 2) / samples;
        verifyPixels(device, singleColor, green, 0xff000000u | ((255u - amountBlue) << 16) | amountBlue, red);
    } else {
        verifyPixels(device, scene, green, blue, red);
    }
}

void runCase(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, bool useResz,
             D3DMULTISAMPLE_TYPE sampleType) {
    const unsigned samples = sampleType == D3DMULTISAMPLE_NONE ? 1u : static_cast<unsigned>(sampleType);
    PostFXRef<IDirect3DSurface9> scene, picture, depth, bound;
    PostFXRef<IDirect3DTexture9> texture, container;
    TRY(device->CreateRenderTarget(width, height, colorFormat, sampleType, 0, FALSE, scene.put(), nullptr));
    TRY(device->CreateRenderTarget(width, height, colorFormat, D3DMULTISAMPLE_NONE, 0, FALSE, picture.put(), nullptr));
    TRY(device->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, texture.put(), nullptr));
    if (useResz) {
        TRY(device->CreateDepthStencilSurface(width, height, D3DFMT_D24S8, sampleType, 0, FALSE, depth.put(), nullptr));
    } else {
        TRY(texture->GetSurfaceLevel(0, depth.put()));
    }
    TRY(device->SetDepthStencilSurface(nullptr));
    TRY(device->SetRenderTarget(0, scene.get()));
    TRY(device->SetDepthStencilSurface(depth.get()));
    TRY(device->GetDepthStencilSurface(bound.put()));
    const HRESULT containerStatus = bound->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(container.put()));
    if (!useResz) {
        TRY(containerStatus);
        expect(container.get() != nullptr, "INTZ surface must return its texture");
    }
    drawDepth(device, samples > 1);
    if (useResz)
        resolveDepth(device, texture.get());
    visualizeDepth(device, picture.get(), useResz ? texture.get() : container.get(), shader);
    verifyPixels(device, picture.get(), 0xff404040u, 0xffbfbfbfu, 0xffbfbfbfu);
    checkSourceAfterSampling(device, scene.get(), depth.get(), picture.get(), samples);
    TRY(device->SetDepthStencilSurface(nullptr));
    std::printf("depth=%s samples=%u source_texture=%d verified_depth=%u verified_depth_stencil_reuse=%u\n",
                useResz ? "D24S8-to-INTZ-RESZ" : "bound-INTZ", samples,
                int(SUCCEEDED(containerStatus) && container.get()), width * height, width * height);
}
}

bool checkDepthInputs(IDirect3D9* api, IDirect3DDevice9* device) {
    D3DDISPLAYMODE display{};
    TRY(api->GetAdapterDisplayMode(0, &display));
    const auto supports = [&](DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) {
        return SUCCEEDED(api->CheckDeviceFormat(0, D3DDEVTYPE_HAL, display.Format, usage, resource, format));
    };
    const bool hasIntz = supports(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, intz);
    const bool hasPlain = supports(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8);
    const bool hasResz = supports(D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, resz);
    std::printf("Depth capabilities: INTZ texture=%d D24S8 surface=%d RESZ=%d\n",
                int(hasIntz), int(hasPlain), int(hasResz));
    if (hasPlain) {
        PostFXRef<IDirect3DSurface9> plain;
        PostFXRef<IDirect3DTexture9> owner;
        TRY(device->CreateDepthStencilSurface(width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE,
                                              0, FALSE, plain.put(), nullptr));
        HRESULT status = plain->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(owner.put()));
        std::printf("Standalone D24S8 exposes a texture container: %d (0x%08x)\n",
                    int(SUCCEEDED(status) && owner.get()), unsigned(status));
    }
    if (!hasIntz) {
        std::puts("SKIP: adapter does not advertise a sampleable INTZ depth texture");
        return false;
    }
    PostFXRef<IDirect3DPixelShader9> shader;
    TRY(device->CreatePixelShader(depthViewCode, shader.put()));
    runCase(device, shader.get(), false, D3DMULTISAMPLE_NONE);
    if (!hasResz || !hasPlain) {
        std::puts("Skipped copying standalone D24S8 into INTZ on this adapter");
        return true;
    }
    for (auto mode : {D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES}) {
        if (FAILED(api->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, colorFormat, TRUE, mode, nullptr)) ||
            FAILED(api->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, mode, nullptr))) {
            std::printf("Standalone D24S8 capture unavailable for sample type %u\n", unsigned(mode));
            continue;
        }
        runCase(device, shader.get(), true, mode);
    }
    return true;
}
} // namespace postfx_probe
