/* SPDX-License-Identifier: GPL-3.0-only */
#include "enb_exec.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
template <class T> struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& other) noexcept : p(other.p) { other.p = nullptr; }
    Ref& operator=(Ref&& other) noexcept { if (this != &other) { reset(); p = other.p; other.p = nullptr; } return *this; }
    T** put() { reset(); return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

struct Constant { std::string name; unsigned set, reg, count; std::vector<float> defaults; };

// D3DXSHADER_CONSTANTTABLE inside the "CTAB" comment of the bytecode.
bool parseTable(const std::vector<DWORD>& code, std::vector<Constant>& out) {
    for (size_t i = 1; i + 2 < code.size();) {
        const DWORD token = code[i];
        if ((token & 0xffff) == 0xfffe) { // comment token
            const size_t words = (token >> 16) & 0x7fff;
            if (i + 1 + words > code.size()) return false;
            if (words >= 8 && code[i + 1] == 0x42415443) { // 'CTAB'
                const unsigned char* base = reinterpret_cast<const unsigned char*>(&code[i + 2]);
                const size_t bytes = (words - 1) * 4;
                auto u32 = [&](size_t o) { DWORD v = 0; if (o + 4 <= bytes) std::memcpy(&v, base + o, 4); return v; };
                auto u16 = [&](size_t o) { unsigned v = 0; if (o + 2 <= bytes) v = base[o] | (base[o + 1] << 8); return v; };
                if (u32(0) != 28) return false;
                const DWORD count = u32(12), info = u32(16);
                if (count > 256 || info > bytes) return false;
                for (DWORD k = 0; k < count; ++k) {
                    const size_t o = info + k * 20;
                    const DWORD name = u32(o);
                    if (o + 20 > bytes || name >= bytes) return false;
                    const char* text = reinterpret_cast<const char*>(base + name);
                    const size_t room = bytes - name;
                    if (!std::memchr(text, 0, room)) return false;
                    Constant constant{text, u16(o + 4), u16(o + 6), u16(o + 8), {}};
                    const DWORD defaults = u32(o + 16);   // D3DXSHADER_CONSTANTINFO::DefaultValue
                    if (constant.set == 2 && defaults && constant.count && constant.count <= 64 && defaults + constant.count * 16 <= bytes) {
                        constant.defaults.resize(constant.count * 4);
                        std::memcpy(constant.defaults.data(), base + defaults, constant.count * 16);
                    }
                    out.push_back(std::move(constant));
                }
                return true;
            }
            i += 1 + words;
        } else {
            return false; // comments come first; no table
        }
    }
    return false;
}

struct Shader {
    Ref<IDirect3DPixelShader9> ps;
    Ref<IDirect3DVertexShader9> vs;
    std::vector<Constant> ps_constants, vs_constants;
};

struct PassObjects { Shader shader; bool hasVs = false; };

struct SavedState {
    static constexpr unsigned maxTargets = 4;
    unsigned targetCount = 0;
    Ref<IDirect3DSurface9> targets[maxTargets], depth;
    Ref<IDirect3DStateBlock9> block;
    Ref<IDirect3DVertexBuffer9> stream;
    Ref<IDirect3DVertexDeclaration9> declaration;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    UINT streamOffset = 0, stride = 0, frequency = 0;
    DWORD fvf = 0;
    Ref<IDirect3DBaseTexture9> textures[16];
    unsigned textureCount = 0;

    HRESULT capture(IDirect3DDevice9* d) {
        D3DCAPS9 caps{};
        HRESULT r = d->GetDeviceCaps(&caps);
        if (FAILED(r)) return r;
        targetCount = caps.NumSimultaneousRTs;
        if (!targetCount || targetCount > maxTargets) return D3DERR_NOTAVAILABLE;
        for (unsigned i = 0; i < targetCount; ++i) {
            r = d->GetRenderTarget(i, targets[i].put());
            if (FAILED(r) && !(i && r == D3DERR_NOTFOUND)) return r;
        }
        r = d->GetDepthStencilSurface(depth.put());
        if (FAILED(r) && r != D3DERR_NOTFOUND) return r;
        if (FAILED(r = d->GetViewport(&viewport)) || FAILED(r = d->GetScissorRect(&scissor)) ||
            FAILED(r = d->GetStreamSource(0, stream.put(), &streamOffset, &stride)) ||
            FAILED(r = d->GetStreamSourceFreq(0, &frequency)) ||
            FAILED(r = d->GetVertexDeclaration(declaration.put())) || FAILED(r = d->GetFVF(&fvf)))
            return r;
        textureCount = std::min<unsigned>(caps.MaxSimultaneousTextures ? caps.MaxSimultaneousTextures : 8, 16);
        for (unsigned i = 0; i < textureCount; ++i) d->GetTexture(i, textures[i].put());
        return d->CreateStateBlock(D3DSBT_ALL, block.put());
    }

    HRESULT restore(IDirect3DDevice9* d) {
        HRESULT first = D3D_OK;
        auto once = [&](HRESULT r) { if (SUCCEEDED(first) && FAILED(r)) first = r; };
        once(d->SetDepthStencilSurface(nullptr));
        for (unsigned i = 1; i < targetCount; ++i) once(d->SetRenderTarget(i, nullptr));
        once(d->SetRenderTarget(0, targets[0].get()));
        for (unsigned i = 1; i < targetCount; ++i) once(d->SetRenderTarget(i, targets[i].get()));
        once(d->SetDepthStencilSurface(depth.get()));
        once(block->Apply());
        for (unsigned i = 0; i < textureCount; ++i) once(d->SetTexture(i, textures[i].get()));
        once(declaration.get() || !fvf ? d->SetVertexDeclaration(declaration.get()) : d->SetFVF(fvf));
        once(d->SetStreamSource(0, stream.get(), streamOffset, stride));
        once(d->SetStreamSourceFreq(0, frequency));
        once(d->SetViewport(&viewport));
        once(d->SetScissorRect(&scissor));
        return first;
    }
};

DWORD filterOf(const char* text, DWORD fallback) {
    if (!std::strcmp(text, "Point") || !std::strcmp(text, "POINT")) return D3DTEXF_POINT;
    if (!std::strcmp(text, "Linear") || !std::strcmp(text, "LINEAR")) return D3DTEXF_LINEAR;
    if (!std::strcmp(text, "None") || !std::strcmp(text, "NONE")) return D3DTEXF_NONE;
    return fallback;
}

DWORD addressOf(const char* text) {
    if (!std::strcmp(text, "Wrap") || !std::strcmp(text, "WRAP")) return D3DTADDRESS_WRAP;
    if (!std::strcmp(text, "Mirror") || !std::strcmp(text, "MIRROR")) return D3DTADDRESS_MIRROR;
    if (!std::strcmp(text, "Border") || !std::strcmp(text, "BORDER")) return D3DTADDRESS_BORDER;
    return D3DTADDRESS_CLAMP;
}
} // namespace

struct EnbEffectExec::Impl {
    EnbLowered lowered{};
    std::vector<std::vector<PassObjects>> techniques;
    std::map<std::string, std::vector<float>> constants;
    std::map<std::string, IDirect3DBaseTexture9*> images;
    Ref<IDirect3DTexture9> scratch[2];
    D3DSURFACE_DESC scratchDesc[2]{};
    std::map<std::string, Ref<IDirect3DTexture9>> named; // RenderTarget<N> images the effect writes and samples
    Ref<IDirect3DVertexDeclaration9> declaration;
    Ref<IDirect3DVertexBuffer9> quad;
    IDirect3DDevice9* device = nullptr;
    unsigned long long draws = 0;
    bool loaded = false;

    ~Impl() { enb_lowered_free(&lowered); }

    HRESULT ensureScratch(unsigned index, unsigned w, unsigned h, D3DFORMAT format) {
        if (scratch[index].get() && scratchDesc[index].Width == w && scratchDesc[index].Height == h &&
            scratchDesc[index].Format == format)
            return D3D_OK;
        HRESULT r = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, scratch[index].put(), nullptr);
        if (FAILED(r)) return r;
        scratch[index]->GetLevelDesc(0, &scratchDesc[index]);
        return D3D_OK;
    }

    // RenderTarget<N>: N x N, 8 bits per channel; names with F (for example RenderTargetRGBA64F) use half floats.
    // Other names are refused instead of guessed.
    HRESULT namedTarget(const std::string& name, D3DFORMAT outputFormat, IDirect3DTexture9** out, unsigned* size) {
        const std::string prefix = "RenderTarget";
        if (name.compare(0, prefix.size(), prefix) != 0) return D3DERR_NOTFOUND;
        std::string rest = name.substr(prefix.size());
        unsigned n = 0;
        size_t i = 0;
        while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9' && n < 8192) n = n * 10 + unsigned(rest[i++] - '0');
        D3DFORMAT format = outputFormat;
        if (!n && rest.find("RGBA32") == 0) n = 0; // sized like the output below
        if (rest.find("RGBA64F") != std::string::npos) format = D3DFMT_A16B16G16R16F;
        else if (rest.find("RGBA64") != std::string::npos) format = D3DFMT_A16B16G16R16;
        else if (rest.find("R32F") != std::string::npos) format = D3DFMT_R32F;
        else if (rest.find("R16F") != std::string::npos) format = D3DFMT_R16F;
        else if (i != rest.size() && rest.find("RGBA32") != 0) return D3DERR_NOTFOUND;
        if (!n) n = *size;     // unsized names follow the output size
        auto it = named.find(name);
        if (it == named.end() || !it->second.get()) {
            Ref<IDirect3DTexture9> texture;
            HRESULT r = device->CreateTexture(n, n, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, texture.put(), nullptr);
            if (FAILED(r)) return r;
            named[name] = std::move(texture);
            it = named.find(name);
        }
        *out = it->second.get();
        *size = n;
        return D3D_OK;
    }

    HRESULT ensureGeometry() {
        if (declaration.get() && quad.get()) return D3D_OK;
        const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
                                              {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
        HRESULT r = device->CreateVertexDeclaration(elements, declaration.put());
        if (FAILED(r)) return r;
        r = device->CreateVertexBuffer(4 * 20, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, quad.put(), nullptr);
        if (FAILED(r)) return r;
        // Clip-space quad; the host's viewport carries the D3D9 half-pixel offset.
        const float v[4][5] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
        void* data = nullptr;
        if (FAILED(r = quad->Lock(0, sizeof v, &data, 0))) return r;
        std::memcpy(data, v, sizeof v);
        return quad->Unlock();
    }

    void applyConstants(IDirect3DDevice9* d, const Shader& s) {
        auto apply = [&](const std::vector<Constant>& list, bool pixel) {
            for (const Constant& c : list) {
                if (c.set != 2) continue; // float4 registers only
                auto it = constants.find(c.name);
                std::vector<float> data(std::max(1u, c.count) * 4, 0.0f);
                if (it != constants.end()) std::copy_n(it->second.begin(), std::min<size_t>(it->second.size(), data.size()), data.begin());
                else if (!c.defaults.empty()) data = c.defaults;      // the HLSL initializer
                else continue;
                if (pixel) d->SetPixelShaderConstantF(c.reg, data.data(), std::max(1u, c.count));
                else d->SetVertexShaderConstantF(c.reg, data.data(), std::max(1u, c.count));
            }
        };
        apply(s.ps_constants, true);
        apply(s.vs_constants, false);
    }

    HRESULT bindSamplers(IDirect3DDevice9* d, const Shader& s, IDirect3DBaseTexture9* color) {
        for (const Constant& c : s.ps_constants) {
            if (c.set != 3) continue;
            std::string image = c.name;
            DWORD minF = D3DTEXF_LINEAR, magF = D3DTEXF_LINEAR, mipF = D3DTEXF_NONE, u = D3DTADDRESS_CLAMP, v = D3DTADDRESS_CLAMP;
            for (unsigned i = 0; i < lowered.sampler_count; ++i) {
                const EnbSamplerBinding& b = lowered.samplers[i];
                if (c.name != b.name) continue;
                if (b.texture[0]) image = b.texture;
                if (b.min_filter[0]) minF = filterOf(b.min_filter, minF);
                if (b.mag_filter[0]) magF = filterOf(b.mag_filter, magF);
                if (b.mip_filter[0]) mipF = filterOf(b.mip_filter, mipF);
                if (b.address_u[0]) u = addressOf(b.address_u);
                if (b.address_v[0]) v = addressOf(b.address_v);
            }
            IDirect3DBaseTexture9* texture = image == "TextureColor" ? color : nullptr;
            if (!texture) { auto it = images.find(image); if (it != images.end()) texture = it->second; }
            if (!texture) { auto it = named.find(image); if (it != named.end()) texture = it->second.get(); }
            if (!texture) return D3DERR_NOTFOUND; // an image the effect samples is missing: no silent black
            HRESULT r;
            if (FAILED(r = d->SetTexture(c.reg, texture)) || FAILED(r = d->SetSamplerState(c.reg, D3DSAMP_MINFILTER, minF)) ||
                FAILED(r = d->SetSamplerState(c.reg, D3DSAMP_MAGFILTER, magF)) || FAILED(r = d->SetSamplerState(c.reg, D3DSAMP_MIPFILTER, mipF)) ||
                FAILED(r = d->SetSamplerState(c.reg, D3DSAMP_ADDRESSU, u)) || FAILED(r = d->SetSamplerState(c.reg, D3DSAMP_ADDRESSV, v)) ||
                FAILED(r = d->SetSamplerState(c.reg, D3DSAMP_SRGBTEXTURE, 0)))
                return r;
        }
        return D3D_OK;
    }

    HRESULT drawPass(IDirect3DDevice9* d, const PassObjects& pass, IDirect3DBaseTexture9* color, IDirect3DSurface9* target,
                     unsigned w, unsigned h, unsigned targets) {
        HRESULT r = d->SetDepthStencilSurface(nullptr);
        if (FAILED(r)) return r;
        for (unsigned i = 1; i < targets; ++i) if (FAILED(r = d->SetRenderTarget(i, nullptr))) return r;
        D3DVIEWPORT9 viewport = {0, 0, w, h, 0.0f, 1.0f};
        if (FAILED(r = d->SetRenderTarget(0, target)) || FAILED(r = d->SetViewport(&viewport)) ||
            FAILED(r = d->SetStreamSourceFreq(0, 1)))
            return r;
        const struct { D3DRENDERSTATETYPE type; DWORD value; } states[] = {
            {D3DRS_ZENABLE, D3DZB_FALSE}, {D3DRS_ZWRITEENABLE, 0}, {D3DRS_STENCILENABLE, 0}, {D3DRS_ALPHATESTENABLE, 0},
            {D3DRS_ALPHABLENDENABLE, 0}, {D3DRS_SEPARATEALPHABLENDENABLE, 0}, {D3DRS_FILLMODE, D3DFILL_SOLID},
            {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_SCISSORTESTENABLE, 0}, {D3DRS_CLIPPING, 1}, {D3DRS_FOGENABLE, 0},
            {D3DRS_LIGHTING, 0}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_SRGBWRITEENABLE, 0}, {D3DRS_MULTISAMPLEMASK, 0xffffffff},
            {D3DRS_COLORWRITEENABLE, 0xf}};
        for (auto s : states) if (FAILED(r = d->SetRenderState(s.type, s.value))) return r;
        if (pass.shader.vs.get()) {
            if (FAILED(r = ensureGeometry()) || FAILED(r = d->SetVertexDeclaration(declaration.get())) ||
                FAILED(r = d->SetStreamSource(0, quad.get(), 0, 20)) || FAILED(r = d->SetVertexShader(pass.shader.vs.get())))
                return r;
        } else {
            if (FAILED(r = d->SetVertexShader(nullptr)) || FAILED(r = d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1))) return r;
        }
        if (FAILED(r = d->SetPixelShader(pass.shader.ps.get()))) return r;
        applyConstants(d, pass.shader);
        if (FAILED(r = bindSamplers(d, pass.shader, color))) return r;
        ++draws;
        if (pass.shader.vs.get()) return d->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        const float right = float(w) - 0.5f, bottom = float(h) - 0.5f;
        struct Vertex { float x, y, z, rhw, u, v; };
        const Vertex quadVerts[] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {right, -0.5f, 0, 1, 1, 0}, {-0.5f, bottom, 0, 1, 0, 1}, {right, bottom, 0, 1, 1, 1}};
        return d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quadVerts, sizeof(Vertex));
    }
};

EnbEffectExec::EnbEffectExec() : impl_(new Impl) {}
EnbEffectExec::~EnbEffectExec() = default;

bool EnbEffectExec::load(IDirect3DDevice9* device, const EnbLowered& lowered, const Compile& compile, std::string& log) {
    release();
    Impl& m = *impl_;
    std::vector<std::vector<PassObjects>> built;
    for (unsigned t = 0; t < lowered.technique_count; ++t) {
        built.emplace_back();
        for (unsigned p = 0; p < lowered.techniques[t].pass_count; ++p) {
            const EnbPassProgram& program = lowered.techniques[t].passes[p];
            PassObjects pass;
            if (!program.ps_entry[0]) { log += std::string(lowered.techniques[t].name) + ": a pass without a pixel shader\n"; return false; }
            std::vector<DWORD> code;
            std::string text;
            if (!compile(lowered.hlsl ? lowered.hlsl : "", program.ps_entry, program.ps_profile, code, text)) {
                log += std::string(program.ps_entry) + ": " + text + "\n";
                return false;
            }
            if (!parseTable(code, pass.shader.ps_constants) || FAILED(device->CreatePixelShader(code.data(), pass.shader.ps.put()))) {
                log += std::string(program.ps_entry) + ": the pixel shader or its constant table was rejected\n";
                return false;
            }
            if (program.vs_entry[0]) {
                code.clear();
                if (!compile(lowered.hlsl ? lowered.hlsl : "", program.vs_entry, program.vs_profile, code, text)) {
                    log += std::string(program.vs_entry) + ": " + text + "\n";
                    return false;
                }
                if (!parseTable(code, pass.shader.vs_constants) || FAILED(device->CreateVertexShader(code.data(), pass.shader.vs.put()))) {
                    log += std::string(program.vs_entry) + ": the vertex shader or its constant table was rejected\n";
                    return false;
                }
            }
            built.back().push_back(std::move(pass));
        }
    }
    // Keep a copy of the facts the passes need (names and sampler states).
    m.lowered = EnbLowered{};
    m.lowered.technique_count = lowered.technique_count;
    std::memcpy(m.lowered.techniques, lowered.techniques, sizeof m.lowered.techniques);
    m.named.clear();
    m.lowered.sampler_count = lowered.sampler_count;
    std::memcpy(m.lowered.samplers, lowered.samplers, sizeof m.lowered.samplers);
    m.techniques = std::move(built);
    m.device = device;
    m.loaded = true;
    return true;
}

void EnbEffectExec::setConstant(const std::string& name, const float* values, unsigned count) {
    impl_->constants[name].assign(values, values + count);
}

void EnbEffectExec::setImage(const std::string& name, IDirect3DBaseTexture9* texture) {
    if (texture) impl_->images[name] = texture;
    else impl_->images.erase(name);
}

unsigned EnbEffectExec::techniqueCount() const { return unsigned(impl_->techniques.size()); }
unsigned EnbEffectExec::passCount(unsigned t) const { return t < impl_->techniques.size() ? unsigned(impl_->techniques[t].size()) : 0; }
unsigned long long EnbEffectExec::drawCount() const { return impl_->draws; }

void EnbEffectExec::release() {
    Impl& m = *impl_;
    m.techniques.clear();
    m.scratch[0].reset();
    m.scratch[1].reset();
    m.named.clear();
    m.declaration.reset();
    m.quad.reset();
    m.device = nullptr;
    m.loaded = false;
}

HRESULT EnbEffectExec::run(IDirect3DDevice9* d, IDirect3DTexture9* scene, IDirect3DSurface9* output, const Options& options) {
    Impl& m = *impl_;
    if (!d || !scene || !output || !m.loaded || d != m.device || options.technique >= m.techniques.size() ||
        m.techniques[options.technique].empty() || !options.scaleDiv)
        return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC target{};
    HRESULT r = output->GetDesc(&target);
    if (FAILED(r)) return r;
    if (!(target.Usage & D3DUSAGE_RENDERTARGET) || target.MultiSampleType != D3DMULTISAMPLE_NONE ||
        (target.Format != D3DFMT_A8R8G8B8 && target.Format != D3DFMT_X8R8G8B8))
        return D3DERR_NOTAVAILABLE;
    const auto& passes = m.techniques[options.technique];
    const unsigned small_w = std::max(1u, target.Width / options.scaleDiv), small_h = std::max(1u, target.Height / options.scaleDiv);
    if (passes.size() > 1) {
        if (FAILED(r = m.ensureScratch(0, small_w, small_h, target.Format))) return r;
        if (passes.size() > 2 && FAILED(r = m.ensureScratch(1, small_w, small_h, target.Format))) return r;
    }
    SavedState state;
    if (FAILED(r = state.capture(d)) || FAILED(r = d->BeginScene())) return r;
    IDirect3DBaseTexture9* color = scene;
    HRESULT result = D3D_OK;
    for (size_t i = 0; i < passes.size() && SUCCEEDED(result); ++i) {
        const bool last = i + 1 == passes.size();
        Ref<IDirect3DSurface9> surface;
        unsigned w = target.Width, h = target.Height;
        IDirect3DSurface9* destination = output;
        const char* named = m.lowered.techniques[options.technique].passes[i].render_target;
        if (named[0]) {   // a named intermediate image: written here, sampled by later passes, the chain colour is unchanged
            IDirect3DTexture9* texture = nullptr;
            unsigned size = target.Width;
            result = m.namedTarget(named, target.Format, &texture, &size);
            if (FAILED(result)) break;
            result = texture->GetSurfaceLevel(0, surface.put());
            if (FAILED(result)) break;
            destination = surface.get();
            D3DSURFACE_DESC desc{};
            texture->GetLevelDesc(0, &desc);
            w = desc.Width;
            h = desc.Height;
            result = m.drawPass(d, passes[i], color, destination, w, h, state.targetCount);
            continue;
        }
        if (!last) {
            const unsigned slot = unsigned(i % 2);
            result = m.scratch[slot]->GetSurfaceLevel(0, surface.put());
            if (FAILED(result)) break;
            destination = surface.get();
            w = small_w;
            h = small_h;
        }
        result = m.drawPass(d, passes[i], color, destination, w, h, state.targetCount);
        if (!last) color = m.scratch[i % 2].get();
    }
    const HRESULT ended = d->EndScene();
    const HRESULT restored = state.restore(d);
    if (FAILED(restored)) return restored;
    return FAILED(result) ? result : ended;
}
