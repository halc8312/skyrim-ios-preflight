/* SPDX-License-Identifier: GPL-3.0-only */
#include "postprocess.h"

namespace {
// Executable tokens from our two_tone.hlsl, compiled by FXC with /T ps_2_0 /O3.
// The optional compiler/parameter-table comment is omitted.
constexpr DWORD shaderCode[] = {
    0xffff0200, 0x05000051, 0xa00f0000, 0x3e800000, 0x3f000000, 0x3e800000, 0xbf000000, 0x05000051,
    0xa00f0001, 0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000008,
    0x80080001, 0x80e40000, 0xa0e40000, 0x03000002, 0x80010001, 0x80ff0001, 0xa0ff0000, 0x04000058,
    0x80030000, 0x80000001, 0xa0000001, 0xa0550001, 0x04000058, 0x80040000, 0x80000001, 0xa0550001,
    0xa0000001, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};
constexpr DWORD vertexFormat = D3DFVF_XYZRHW | D3DFVF_TEX1;

// D3DSBT_ALL omits render targets and depth/stencil. Putting RT0 back also
// resets the viewport/scissor. DrawPrimitiveUP clears stream 0. Preserve all
// of these explicitly, including a caller with a null vertex declaration.
struct SavedState {
    static constexpr unsigned maxTargets = 4;
    unsigned targetCount = 0;
    PostFXRef<IDirect3DSurface9> targets[maxTargets], depth;
    PostFXRef<IDirect3DStateBlock9> block;
    PostFXRef<IDirect3DVertexBuffer9> stream;
    PostFXRef<IDirect3DVertexDeclaration9> declaration;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    UINT streamOffset = 0, stride = 0, frequency = 0;
    DWORD fvf = 0;

    HRESULT capture(IDirect3DDevice9* d) {
        D3DCAPS9 caps{};
        HRESULT r = d->GetDeviceCaps(&caps);
        if (FAILED(r))
            return r;
        targetCount = caps.NumSimultaneousRTs;
        if (!targetCount || targetCount > maxTargets)
            return D3DERR_NOTAVAILABLE;
        for (unsigned i = 0; i < targetCount; ++i) {
            r = d->GetRenderTarget(i, targets[i].put());
            if (FAILED(r) && !(i && r == D3DERR_NOTFOUND))
                return r;
        }
        r = d->GetDepthStencilSurface(depth.put());
        if (FAILED(r) && r != D3DERR_NOTFOUND)
            return r;
        if (FAILED(r = d->GetViewport(&viewport)) || FAILED(r = d->GetScissorRect(&scissor)) ||
            FAILED(r = d->GetStreamSource(0, stream.put(), &streamOffset, &stride)) ||
            FAILED(r = d->GetStreamSourceFreq(0, &frequency)) ||
            FAILED(r = d->GetVertexDeclaration(declaration.put())) || FAILED(r = d->GetFVF(&fvf)))
            return r;
        return d->CreateStateBlock(D3DSBT_ALL, block.put());
    }

    HRESULT restore(IDirect3DDevice9* d) {
        HRESULT first = D3D_OK;
        auto restoreCall = [&](HRESULT r) {
            if (SUCCEEDED(first) && FAILED(r))
                first = r;
        };
        restoreCall(d->SetDepthStencilSurface(nullptr));
        for (unsigned i = 1; i < targetCount; ++i)
            restoreCall(d->SetRenderTarget(i, nullptr));
        restoreCall(d->SetRenderTarget(0, targets[0].get()));
        for (unsigned i = 1; i < targetCount; ++i)
            restoreCall(d->SetRenderTarget(i, targets[i].get()));
        restoreCall(d->SetDepthStencilSurface(depth.get()));
        restoreCall(block->Apply());
        // D3D9 may report a nonzero FVF for an application-created declaration
        // with an equivalent layout. SetFVF would bind a different COM object.
        restoreCall(declaration.get() || !fvf ? d->SetVertexDeclaration(declaration.get())
                                              : d->SetFVF(fvf));
        restoreCall(d->SetStreamSource(0, stream.get(), streamOffset, stride));
        restoreCall(d->SetStreamSourceFreq(0, frequency));
        restoreCall(d->SetViewport(&viewport));
        restoreCall(d->SetScissorRect(&scissor));
        return first;
    }
};

HRESULT draw(IDirect3DDevice9* d, IDirect3DSurface9* target, IDirect3DTexture9* source,
             IDirect3DPixelShader9* effect, const D3DSURFACE_DESC& desc, unsigned targets) {
    HRESULT r = d->SetDepthStencilSurface(nullptr);
    if (FAILED(r))
        return r;
    for (unsigned i = 1; i < targets; ++i)
        if (FAILED(r = d->SetRenderTarget(i, nullptr)))
            return r;
    D3DVIEWPORT9 viewport = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    if (FAILED(r = d->SetRenderTarget(0, target)) || FAILED(r = d->SetViewport(&viewport)) ||
        FAILED(r = d->SetVertexShader(nullptr)) || FAILED(r = d->SetPixelShader(effect)) ||
        FAILED(r = d->SetFVF(vertexFormat)) || FAILED(r = d->SetStreamSourceFreq(0, 1)) ||
        FAILED(r = d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0)) ||
        FAILED(r = d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE)) ||
        FAILED(r = d->SetTexture(0, source)))
        return r;
    const struct {
        D3DSAMPLERSTATETYPE type;
        DWORD value;
    } sampling[] = {
        {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
        {D3DSAMP_MINFILTER, D3DTEXF_POINT},    {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE},     {D3DSAMP_SRGBTEXTURE, 0},
    };
    for (auto s : sampling)
        if (FAILED(r = d->SetSamplerState(0, s.type, s.value)))
            return r;
    const struct {
        D3DRENDERSTATETYPE type;
        DWORD value;
    } states[] = {
        {D3DRS_ZENABLE, D3DZB_FALSE},
        {D3DRS_ZWRITEENABLE, 0},
        {D3DRS_STENCILENABLE, 0},
        {D3DRS_ALPHATESTENABLE, 0},
        {D3DRS_ALPHABLENDENABLE, 0},
        {D3DRS_SEPARATEALPHABLENDENABLE, 0},
        {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_SCISSORTESTENABLE, 0},
        {D3DRS_CLIPPING, 1},
        {D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
        {D3DRS_POINTSPRITEENABLE, 0},
        {D3DRS_FOGENABLE, 0},
        {D3DRS_LIGHTING, 0},
        {D3DRS_CLIPPLANEENABLE, 0},
        {D3DRS_WRAP0, 0},
        {D3DRS_DITHERENABLE, 0},
        {D3DRS_SRGBWRITEENABLE, 0},
        {D3DRS_MULTISAMPLEANTIALIAS, TRUE},
        {D3DRS_MULTISAMPLEMASK, 0xffffffff},
        {D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                     D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA},
    };
    for (auto s : states)
        if (FAILED(r = d->SetRenderState(s.type, s.value)))
            return r;
    // In D3D9 the first texel's center is at (0, 0) in screen space.
    const float right = static_cast<float>(desc.Width) - 0.5f,
                bottom = static_cast<float>(desc.Height) - 0.5f;
    struct Vertex {
        float x, y, z, rhw, u, v;
    };
    const Vertex quad[] = {{-0.5f, -0.5f, 0, 1, 0, 0},
                           {right, -0.5f, 0, 1, 1, 0},
                           {-0.5f, bottom, 0, 1, 0, 1},
                           {right, bottom, 0, 1, 1, 1}};
    return d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}
} // namespace

void TwoTonePass::release() {
    scene_.reset();
    filtered_.reset();
    shader_.reset();
    device_ = nullptr;
    cached_ = {};
}

HRESULT TwoTonePass::prepare(IDirect3DDevice9* d, const D3DSURFACE_DESC& target) {
    if (d == device_ && target.Width == cached_.Width && target.Height == cached_.Height &&
        target.Format == cached_.Format)
        return D3D_OK;
    PostFXRef<IDirect3DTexture9> nextScene;
    HRESULT r = d->CreateTexture(target.Width, target.Height, 1, D3DUSAGE_RENDERTARGET, target.Format,
                                 D3DPOOL_DEFAULT, nextScene.put(), nullptr);
    if (FAILED(r))
        return r;
    if (d != device_) {
        PostFXRef<IDirect3DPixelShader9> nextShader;
        if (FAILED(r = d->CreatePixelShader(shaderCode, nextShader.put())))
            return r;
        shader_.swap(nextShader);
    }
    scene_.swap(nextScene);
    filtered_.reset();
    device_ = d;
    cached_ = target;
    return D3D_OK;
}

HRESULT TwoTonePass::apply(IDirect3DDevice9* d, IDirect3DSurface9* output) {
    if (!d || !output)
        return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC target{};
    HRESULT r = output->GetDesc(&target);
    if (FAILED(r))
        return r;
    if (!(target.Usage & D3DUSAGE_RENDERTARGET) ||
        (target.Format != D3DFMT_A8R8G8B8 && target.Format != D3DFMT_X8R8G8B8))
        return D3DERR_NOTAVAILABLE;
    if (FAILED(r = prepare(d, target)))
        return r;
    const bool multisampled = target.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (multisampled && !filtered_.get()) {
        r = d->CreateRenderTarget(target.Width, target.Height, target.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                  filtered_.put(), nullptr);
        if (FAILED(r))
            return r;
    }
    PostFXRef<IDirect3DSurface9> sample;
    if (FAILED(r = scene_->GetSurfaceLevel(0, sample.put())))
        return r;
    // Resolve the completed scene into a different, sampleable single-sample
    // texture before issuing the draw. Source and render target never alias.
    if (FAILED(r = d->StretchRect(output, nullptr, sample.get(), nullptr, D3DTEXF_NONE)))
        return r;
    SavedState state;
    if (FAILED(r = state.capture(d)) || FAILED(r = d->BeginScene()))
        return r;
    // The fixed DXVK viewport has a half-pixel offset. A direct draw into a
    // multisampled backbuffer can leave edge subsamples of the old image.
    // Filter the resolved image and transfer all samples in one surface blit.
    r = draw(d, multisampled ? filtered_.get() : output, scene_.get(), shader_.get(), target,
             state.targetCount);
    HRESULT ended = d->EndScene();
    HRESULT restored = state.restore(d);
    if (FAILED(restored))
        return restored;
    if (FAILED(r))
        return r;
    if (FAILED(ended))
        return ended;
    return multisampled ? d->StretchRect(filtered_.get(), nullptr, output, nullptr, D3DTEXF_NONE) : D3D_OK;
}
