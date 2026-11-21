/* SPDX-License-Identifier: GPL-3.0-only
 * Runs authored ENB-style effect files through EnbEffectExec on a real D3D9 device (native DXVK on a
 * software Vulkan driver here) and reads every output pixel back. No ENB binary, preset or game input. */
#include "enb_exec.h"
#include "enb_runtime.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vkd3d_shader.h>

namespace {
void expect(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void tryCall(HRESULT r, const char* what) {
    if (FAILED(r)) { char b[96]; std::snprintf(b, sizeof b, "%s (0x%08x)", what, unsigned(r)); throw std::runtime_error(b); }
}
#define TRY(x) tryCall((x), #x)

template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
};

bool compileHlsl(const std::string& hlsl, const std::string& entry, const std::string& profile, std::vector<DWORD>& code,
                 std::string& log) {
    vkd3d_shader_hlsl_source_info source = {VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO, nullptr, entry.c_str(), {}, profile.c_str()};
    vkd3d_shader_compile_info info = {VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, &source, {hlsl.data(), hlsl.size()},
                                      VKD3D_SHADER_SOURCE_HLSL, VKD3D_SHADER_TARGET_D3D_BYTECODE, nullptr, 0,
                                      VKD3D_SHADER_LOG_WARNING, "enb"};
    vkd3d_shader_code out = {};
    char* messages = nullptr;
    const int status = vkd3d_shader_compile(&info, &out, &messages);
    if (messages) { log += messages; vkd3d_shader_free_messages(messages); }
    if (status < 0) { vkd3d_shader_free_shader_code(&out); return false; }
    code.assign(static_cast<const DWORD*>(out.code), static_cast<const DWORD*>(out.code) + out.size / 4);
    vkd3d_shader_free_shader_code(&out);
    return true;
}

constexpr DWORD quadrantColors[] = {0x23281030, 0xb4f0c060, 0x7f20ea10, 0xddcc4c20};

struct Frame {
    unsigned w, h;
    Ptr<IDirect3DTexture9> scene;
    Ptr<IDirect3DSurface9> output;
};

unsigned quadrant(unsigned x, unsigned y, unsigned w, unsigned h) { return (y >= h / 2 ? 2u : 0u) + (x >= w / 2 ? 1u : 0u); }

void makeFrame(IDirect3DDevice9* d, Frame& f, unsigned w, unsigned h) {
    f.w = w; f.h = h;
    TRY(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, f.scene.put(), nullptr));
    Ptr<IDirect3DSurface9> surface;
    TRY(f.scene->GetSurfaceLevel(0, surface.put()));
    for (unsigned i = 0; i < 4; ++i) {
        RECT area = {LONG(i & 1 ? w / 2 : 0), LONG(i & 2 ? h / 2 : 0), LONG(i & 1 ? w : w / 2), LONG(i & 2 ? h : h / 2)};
        TRY(d->ColorFill(surface.get(), &area, quadrantColors[i]));
    }
    TRY(d->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, f.output.put(), nullptr));
    TRY(d->ColorFill(f.output.get(), nullptr, 0xff00ff00)); // a stale green the effect must replace
}

// Compare each pixel with expected(channels in 0..255 order a,r,g,b), tolerance in 8-bit levels.
template <class Fn> void check(IDirect3DDevice9* d, Frame& f, Fn expectedOf, int tolerance, const char* what) {
    Ptr<IDirect3DSurface9> host;
    TRY(d->CreateOffscreenPlainSurface(f.w, f.h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, host.put(), nullptr));
    TRY(d->GetRenderTargetData(f.output.get(), host.get()));
    D3DLOCKED_RECT lock{};
    TRY(host->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    for (unsigned y = 0; y < f.h; ++y)
        for (unsigned x = 0; x < f.w; ++x) {
            DWORD got;
            std::memcpy(&got, static_cast<const char*>(lock.pBits) + y * lock.Pitch + x * 4, 4);
            const DWORD src = quadrantColors[quadrant(x, y, f.w, f.h)];
            int want[4];
            expectedOf(src, want);
            for (int c = 0; c < 4; ++c) {
                const int have = int((got >> (24 - 8 * c)) & 255);
                if (std::abs(have - want[c]) > tolerance) {
                    std::fprintf(stderr, "%s: pixel %u,%u channel %d got %d expected %d\n", what, x, y, c, have, want[c]);
                    host->UnlockRect();
                    throw std::runtime_error(what);
                }
            }
        }
    TRY(host->UnlockRect());
}

int channel(DWORD v, int c) { return int((v >> (24 - 8 * c)) & 255); }
int clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : int(v + 0.5f); }

const char* effectA =
    "sampler2D S = sampler_state { Texture = <TextureColor>; MinFilter = Point; MagFilter = Point; AddressU = Clamp; AddressV = Clamp; };\n"
    "float4 Tint <string UIName=\"Tint\"; float UIMin=0.0; float UIMax=2.0;> = {1,1,1,1};\n"
    "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(S, uv) * Tint; }\n"
    "technique Main <string UIName=\"A\";> { pass P0 { PixelShader = compile ps_2_0 PS(); AlphaBlendEnable = false; } }\n";

const char* effectB =
    "texture2D TextureColor;\n"
    "sampler2D S = sampler_state { Texture = <TextureColor>; MinFilter = Point; MagFilter = Point; AddressU = Clamp; AddressV = Clamp; };\n"
    "float4 Add = {0, 0, 0, 0};\n"
    "struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT VS(float3 p : POSITION, float2 uv : TEXCOORD0) { VS_OUT o; o.pos = float4(p, 1); o.uv = uv; return o; }\n"
    "float4 PSInvert(VS_OUT i) : COLOR { float4 c = tex2D(S, i.uv); return float4(1 - c.rgb, c.a); }\n"
    "float4 PSHalf(VS_OUT i) : COLOR { float4 c = tex2D(S, i.uv); return float4(c.rgb * 0.5 + Add.rgb, c.a); }\n"
    "technique Two { pass P0 { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PSInvert(); }\n"
    "                pass P1 { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PSHalf(); } }\n"
    "technique One { pass P0 { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PSInvert(); } }\n";

const char* effectNamedTarget =
    "sampler2D S = sampler_state { Texture = <TextureColor>; MinFilter = Point; MagFilter = Point; };\n"
    "sampler2D R = sampler_state { Texture = <RenderTarget64>; MinFilter = Point; MagFilter = Point; };\n"
    "struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT VS(float3 p : POSITION, float2 uv : TEXCOORD0) { VS_OUT o; o.pos = float4(p, 1); o.uv = uv; return o; }\n"
    "float4 PSDown(VS_OUT i) : COLOR { return tex2D(S, i.uv) * 0.5; }\n"
    "float4 PSFinal(VS_OUT i) : COLOR { float4 c = tex2D(R, i.uv); return float4(c.rgb + 0.1, c.a); }\n"
    "technique T { pass Down { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PSDown(); RenderTarget = RenderTarget64; }\n"
    "              pass Final { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PSFinal(); } }\n";

const char* effectBadTarget =
    "float4 PS(float2 uv : TEXCOORD0) : COLOR { return 0.5; }\n"
    "technique T { pass A { PixelShader = compile ps_2_0 PS(); RenderTarget = SomethingElse; } pass B { PixelShader = compile ps_2_0 PS(); } }\n";

const char* effectNeedsBloom =
    "sampler2D S = sampler_state { Texture = <TextureColor>; };\nsampler2D B = sampler_state { Texture = <TextureBloom>; };\n"
    "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(S, uv) + tex2D(B, uv); }\n"
    "technique T { pass P0 { PixelShader = compile ps_2_0 PS(); } }\n";

bool lowerAndLoad(IDirect3DDevice9* d, EnbEffectExec& exec, const char* text, std::string& log) {
    EnbLowered lowered;
    expect(enb_fx_lower(text, std::strlen(text), &lowered) == 0, "lowering");
    const bool ok = exec.load(d, lowered, compileHlsl, log);
    enb_lowered_free(&lowered);
    return ok;
}

struct GameState {
    Ptr<IDirect3DSurface9> target;
    Ptr<IDirect3DTexture9> texture;
    D3DVIEWPORT9 viewport = {3, 5, 20, 9, 0.25f, 0.75f};
    explicit GameState(IDirect3DDevice9* d) {
        TRY(d->CreateRenderTarget(40, 24, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, target.put(), nullptr));
        TRY(d->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, texture.put(), nullptr));
        TRY(d->SetRenderTarget(0, target.get()));
        TRY(d->SetViewport(&viewport));
        TRY(d->SetTexture(2, texture.get()));
        TRY(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        TRY(d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
        TRY(d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN));
        TRY(d->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT));
        TRY(d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
    }
    void verify(IDirect3DDevice9* d) const {
        Ptr<IDirect3DSurface9> rt;
        TRY(d->GetRenderTarget(0, rt.put()));
        expect(rt.get() == target.get(), "render target restored");
        D3DVIEWPORT9 v{};
        TRY(d->GetViewport(&v));
        expect(!std::memcmp(&v, &viewport, sizeof v), "viewport restored");
        Ptr<IDirect3DBaseTexture9> t;
        TRY(d->GetTexture(2, t.put()));
        expect(t.get() == texture.get(), "texture stage restored");
        DWORD value = 0;
        TRY(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &value));
        expect(value == TRUE, "blend state restored");
        TRY(d->GetRenderState(D3DRS_COLORWRITEENABLE, &value));
        expect(value == D3DCOLORWRITEENABLE_GREEN, "write mask restored");
        TRY(d->GetSamplerState(2, D3DSAMP_MINFILTER, &value));
        expect(value == D3DTEXF_POINT, "sampler restored");
        TRY(d->GetFVF(&value));
        expect(value == (D3DFVF_XYZ | D3DFVF_DIFFUSE), "FVF restored");
    }
};

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    expect(bool(out), "write preset file");
}

// A whole preset on disk: an ini with UI values, an include, a bloom stage that makes TextureBloom,
// a main effect that adds it, and a depth-of-field effect that only runs with a depth image.
void presetChecks(IDirect3DDevice9* d, unsigned& checks) {
    char templ[] = "/tmp/enbpresetXXXXXX";
    expect(mkdtemp(templ) != nullptr, "temporary preset folder");
    const std::string root = templ;
    expect(!mkdir((root + "/EnbSeries").c_str(), 0700), "preset subfolder");
    writeFile(root + "/enbseries.ini", "[EFFECT]\nEnableBloom=true\nEnableDepthOfField=true\nBloom Amount=2.0\nTint=1.0, 0.5, 1.0, 1.0\n");
    writeFile(root + "/EnbSeries/COMMON.FXH",
              "sampler2D S = sampler_state { Texture = <TextureColor>; MinFilter = Point; MagFilter = Point; };\n"
              "struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
              "VS_OUT VS(float3 p : POSITION, float2 uv : TEXCOORD0) { VS_OUT o; o.pos = float4(p, 1); o.uv = uv; return o; }\n");
    writeFile(root + "/EnbSeries/enbbloom.fx",
              "#include \"common.fxh\"\nfloat4 PS(VS_OUT i) : COLOR { return tex2D(S, i.uv) * 0.25; }\n"
              "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n");
    writeFile(root + "/EnbSeries/enbeffect.fx",
              "#include \"common.fxh\"\nsampler2D B = sampler_state { Texture = <TextureBloom>; MinFilter = Point; MagFilter = Point; };\n"
              "float BloomAmount <string UIName=\"Bloom Amount\"; float UIMin=0.0; float UIMax=4.0;> = 1.0;\n"
              "float4 Tint <string UIName=\"Tint\";> = {1,1,1,1};\n"
              "float4 PS(VS_OUT i) : COLOR { float4 c = tex2D(S, i.uv); float4 b = tex2D(B, i.uv); return float4(c.rgb * Tint.rgb + b.rgb * BloomAmount, c.a); }\n"
              "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n");
    writeFile(root + "/EnbSeries/enbdepthoffield.fx",
              "#include \"common.fxh\"\nsampler2D D = sampler_state { Texture = <TextureDepth>; MinFilter = Point; MagFilter = Point; };\n"
              "float4 PS(VS_OUT i) : COLOR { return tex2D(S, i.uv) * 0.5 + tex2D(D, i.uv) * 0; }\n"
              "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n");
    EnbRuntime runtime;
    std::string log;
    expect(runtime.load(d, root.c_str(), compileHlsl, log), "preset loads");
    expect(runtime.effectsLoaded() == 3, "three effects loaded");
    Frame f;
    makeFrame(d, f, 32, 16);
    runtime.setGameState({13.0f, 0, 1.0f, 32, 16, 85.0f});
    // Quality 3, no depth image: bloom at full resolution, then the main effect; depth of field is skipped.
    TRY(runtime.run(d, f.scene.get(), f.output.get(), 3));
    expect(runtime.lastPlan().count == 2 && runtime.lastPlan().steps[0].effect == ENB_EFFECT_BLOOM, "plan without depth");
    auto full = [&](DWORD src, int* want) {
        want[0] = channel(src, 0);
        const float tint[3] = {1.0f, 0.5f, 1.0f};
        for (int c = 1; c < 4; ++c) want[c] = clamp255((channel(src, c) * tint[c - 1] + channel(src, c) * 0.25f * 2.0f));
    };
    check(d, f, full, 3, "whole preset: bloom image and ini values");
    ++checks;
    // Quality 1 runs the bloom stage at a quarter of the resolution; the uniform quadrants keep the same colours.
    TRY(d->ColorFill(f.output.get(), nullptr, 0xff00ff00));
    TRY(runtime.run(d, f.scene.get(), f.output.get(), 1));
    expect(runtime.lastPlan().steps[0].scale_div == 4, "quarter resolution at quality 1");
    check(d, f, full, 3, "whole preset at quality 1");
    ++checks;
    // Quality 0 drops the bloom stage; the main effect still runs with a black bloom image.
    TRY(d->ColorFill(f.output.get(), nullptr, 0xff00ff00));
    TRY(runtime.run(d, f.scene.get(), f.output.get(), 0));
    expect(runtime.lastPlan().count == 1, "only the main effect at quality 0");
    check(d, f, [&](DWORD src, int* want) {
        want[0] = channel(src, 0);
        const float tint[3] = {1.0f, 0.5f, 1.0f};
        for (int c = 1; c < 4; ++c) want[c] = clamp255(channel(src, c) * tint[c - 1]);
    }, 3, "whole preset at quality 0");
    ++checks;
    // With a depth image, depth of field joins the chain before the main effect.
    Ptr<IDirect3DTexture9> depth;
    TRY(d->CreateTexture(32, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, depth.put(), nullptr));
    runtime.setDepth(depth.get());
    TRY(d->ColorFill(f.output.get(), nullptr, 0xff00ff00));
    TRY(runtime.run(d, f.scene.get(), f.output.get(), 3));
    expect(runtime.lastPlan().count == 3 && runtime.lastPlan().needs_depth, "plan with depth");
    check(d, f, [&](DWORD src, int* want) {
        want[0] = clamp255(channel(src, 0) * 0.5f);
        const float tint[3] = {1.0f, 0.5f, 1.0f};
        for (int c = 1; c < 4; ++c) {
            const float half = channel(src, c) * 0.5f;     // depth of field first, then bloom of the original and tint
            want[c] = clamp255(half * tint[c - 1] + channel(src, c) * 0.25f * 2.0f);
        }
    }, 3, "chain with depth of field");
    ++checks;
    // A broken effect is switched off on its own and named; the rest of the preset still runs.
    writeFile(root + "/EnbSeries/enbbloom.fx", "float4 PS() : COLOR { return nothing; }\ntechnique T { pass P { PixelShader = compile ps_2_0 PS(); } }\n");
    EnbRuntime partial;
    log.clear();
    expect(partial.load(d, root.c_str(), compileHlsl, log) && partial.effectsLoaded() == 2, "partial preset loads");
    expect(log.find("enbbloom.fx: switched off") != std::string::npos, "the broken effect is named");
    ++checks;
    // A missing include is reported and that effect is off.
    writeFile(root + "/EnbSeries/enbbloom.fx", "#include \"absent.fxh\"\ntechnique T { pass P { } }\n");
    log.clear();
    EnbRuntime missing;
    expect(missing.load(d, root.c_str(), compileHlsl, log) && log.find("include not found: absent.fxh") != std::string::npos, "missing include reported");
    ++checks;
    unlink((root + "/EnbSeries/enbbloom.fx").c_str());
    unlink((root + "/EnbSeries/enbeffect.fx").c_str());
    unlink((root + "/EnbSeries/enbdepthoffield.fx").c_str());
    unlink((root + "/EnbSeries/COMMON.FXH").c_str());
    unlink((root + "/enbseries.ini").c_str());
    rmdir((root + "/EnbSeries").c_str());
    rmdir(root.c_str());
}

int run() {
    SDL_SetMainReady();
    expect(!SDL_Init(SDL_INIT_VIDEO) && !SDL_Vulkan_LoadLibrary(nullptr), "SDL");
    SDL_Window* window = SDL_CreateWindow("enb", 0, 0, 64, 48, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    expect(window != nullptr, "window");
    unsigned checks = 0;
    {   // the device and API are released before SDL unloads Vulkan
    Ptr<IDirect3D9> api;
    *api.put() = Direct3DCreate9(D3D_SDK_VERSION);
    expect(api.get() != nullptr, "Direct3DCreate9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = reinterpret_cast<HWND>(window);
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    Ptr<IDirect3DDevice9> device;
    TRY(api->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, device.put()));
    IDirect3DDevice9* d = device.get();

    // 1. One pixel-shader pass with a constant, no vertex shader.
    {
        EnbEffectExec exec;
        std::string log;
        expect(lowerAndLoad(d, exec, effectA, log), "effect A loads");
        expect(exec.techniqueCount() == 1 && exec.passCount(0) == 1, "effect A structure");
        Frame f;
        makeFrame(d, f, 32, 16);
        const float tint[4] = {0.5f, 1.0f, 0.25f, 1.0f};   // r,g,b,a
        exec.setConstant("Tint", tint, 4);
        TRY(exec.run(d, f.scene.get(), f.output.get(), {}));
        check(d, f, [&](DWORD src, int* want) {
            want[0] = clamp255(channel(src, 0) * 1.0f);
            want[1] = clamp255(channel(src, 1) * 0.5f);
            want[2] = clamp255(channel(src, 2) * 1.0f);
            want[3] = clamp255(channel(src, 3) * 0.25f);
        }, 2, "single pass with constant");
        ++checks;
        // The same effect with another value: constants are applied per run.
        const float tint2[4] = {1.0f, 0.5f, 1.0f, 0.5f};
        exec.setConstant("Tint", tint2, 4);
        TRY(d->ColorFill(f.output.get(), nullptr, 0xff00ff00));
        TRY(exec.run(d, f.scene.get(), f.output.get(), {}));
        check(d, f, [&](DWORD src, int* want) {
            want[0] = clamp255(channel(src, 0) * 0.5f);
            want[1] = clamp255(channel(src, 1) * 1.0f);
            want[2] = clamp255(channel(src, 2) * 0.5f);
            want[3] = clamp255(channel(src, 3) * 1.0f);
        }, 2, "constant changed between runs");
        ++checks;
        expect(exec.drawCount() == 2, "one draw per run");
        // A multisampled or non-render-target output is refused.
        Ptr<IDirect3DSurface9> plain;
        TRY(d->CreateOffscreenPlainSurface(32, 16, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, plain.put(), nullptr));
        expect(FAILED(exec.run(d, f.scene.get(), plain.get(), {})), "non render target refused");
    }

    // 2. Two passes with a vertex shader, at full and at half intermediate resolution, and the one-pass technique.
    {
        EnbEffectExec exec;
        std::string log;
        expect(lowerAndLoad(d, exec, effectB, log), "effect B loads");
        expect(exec.techniqueCount() == 2 && exec.passCount(0) == 2 && exec.passCount(1) == 1, "effect B structure");
        const float add[4] = {0.1f, 0.2f, 0.3f, 0};
        exec.setConstant("Add", add, 4);
        auto both = [&](DWORD src, int* want) {
            want[0] = channel(src, 0);
            for (int c = 1; c < 4; ++c) want[c] = clamp255(((255 - channel(src, c)) / 255.0f * 0.5f + add[c - 1]) * 255.0f);
        };
        for (unsigned scale : {1u, 2u}) {
            Frame f;
            makeFrame(d, f, 32, 16);
            EnbEffectExec::Options options;
            options.scaleDiv = scale;
            TRY(exec.run(d, f.scene.get(), f.output.get(), options));
            check(d, f, both, 2, scale == 1 ? "two passes at full resolution" : "two passes, half-resolution intermediate");
            ++checks;
        }
        Frame f;
        makeFrame(d, f, 32, 16);
        EnbEffectExec::Options one;
        one.technique = 1;
        TRY(exec.run(d, f.scene.get(), f.output.get(), one));
        check(d, f, [&](DWORD src, int* want) {
            want[0] = channel(src, 0);
            for (int c = 1; c < 4; ++c) want[c] = 255 - channel(src, c);
        }, 2, "second technique");
        ++checks;
        one.technique = 5;
        expect(FAILED(exec.run(d, f.scene.get(), f.output.get(), one)), "unknown technique refused");
    }

    // 2b. A pass that writes a named intermediate image which a later pass samples; unknown names are refused.
    {
        EnbEffectExec exec;
        std::string log;
        expect(lowerAndLoad(d, exec, effectNamedTarget, log), "named target effect loads");
        Frame f;
        makeFrame(d, f, 32, 16);
        TRY(exec.run(d, f.scene.get(), f.output.get(), {}));
        check(d, f, [&](DWORD src, int* want) {
            want[0] = clamp255(channel(src, 0) * 0.5f);   // the first pass halves alpha too
            for (int c = 1; c < 4; ++c) want[c] = clamp255(channel(src, c) * 0.5f + 25.5f);
        }, 3, "named render target");
        ++checks;
        EnbEffectExec bad;
        expect(lowerAndLoad(d, bad, effectBadTarget, log), "bad target effect loads");
        expect(FAILED(bad.run(d, f.scene.get(), f.output.get(), {})), "unknown render target name refused");
        ++checks;
    }

    // 3. The game's own state survives every run, including a refused one; a scene in progress refuses the pass.
    {
        EnbEffectExec exec;
        std::string log;
        expect(lowerAndLoad(d, exec, effectA, log), "effect A loads again");
        Frame f;
        makeFrame(d, f, 32, 16);
        GameState game(d);
        game.verify(d);
        TRY(exec.run(d, f.scene.get(), f.output.get(), {}));
        game.verify(d);
        TRY(d->BeginScene());
        expect(FAILED(exec.run(d, f.scene.get(), f.output.get(), {})), "scene in progress refused");
        TRY(d->EndScene());
        game.verify(d);
        ++checks;
        // A missing image is an error, not a black frame, and the state is still restored.
        EnbEffectExec missing;
        expect(lowerAndLoad(d, missing, effectNeedsBloom, log), "bloom effect loads");
        expect(missing.run(d, f.scene.get(), f.output.get(), {}) == D3DERR_NOTFOUND, "missing image reported");
        game.verify(d);
        Ptr<IDirect3DTexture9> bloom;
        TRY(d->CreateTexture(32, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, bloom.put(), nullptr));
        Ptr<IDirect3DSurface9> bloomSurface;
        TRY(bloom->GetSurfaceLevel(0, bloomSurface.put()));
        TRY(d->ColorFill(bloomSurface.get(), nullptr, 0x00101010));
        missing.setImage("TextureBloom", bloom.get());
        TRY(missing.run(d, f.scene.get(), f.output.get(), {}));
        check(d, f, [&](DWORD src, int* want) {
            for (int c = 0; c < 4; ++c) want[c] = clamp255(channel(src, c) + (c == 0 ? 0 : 16));
        }, 3, "second image bound by name");
        game.verify(d);
        ++checks;
    }

    // 4. A shader that does not compile is reported with its message and nothing is kept.
    {
        EnbEffectExec exec;
        std::string log;
        expect(!lowerAndLoad(d, exec, "float4 PS(float2 uv : TEXCOORD0) : COLOR { return notDeclared; }\n"
                                     "technique T { pass P { PixelShader = compile ps_2_0 PS(); } }\n", log), "broken effect refused");
        expect(!log.empty() && exec.techniqueCount() == 0, "log text and no kept state");
        Frame f;
        makeFrame(d, f, 8, 8);
        expect(FAILED(exec.run(d, f.scene.get(), f.output.get(), {})), "unloaded effect refused");
        EnbEffectExec none;
        expect(!lowerAndLoad(d, none, "float4 PS() : COLOR { return 1; }\n", log) || none.techniqueCount() == 0, "no technique");
        ++checks;
    }

    presetChecks(d, checks);

    // 5. Releasing for a device reset drops the objects; a new load works.
    {
        EnbEffectExec exec;
        std::string log;
        expect(lowerAndLoad(d, exec, effectA, log), "load before release");
        exec.release();
        expect(exec.techniqueCount() == 0, "release drops techniques");
        expect(lowerAndLoad(d, exec, effectA, log), "load after release");
        ++checks;
    }
    }
    std::printf("Verified %u ENB effect checks\n", checks);
    std::fflush(stdout);
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 0;
}
} // namespace

int main() {
    try { return run(); }
    catch (const std::exception& e) { std::fprintf(stderr, "ENB effect test failed: %s\n", e.what()); return 1; }
}
