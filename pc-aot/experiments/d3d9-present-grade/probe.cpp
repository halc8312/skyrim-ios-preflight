/* SPDX-License-Identifier: GPL-3.0-only */
// Exercise the same DXVK present draw against a readable, private test destination.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <SDL.h>
#include <SDL_vulkan.h>
#include "d3d9_device.h"
#include "d3d9_surface.h"
#include "grade_trial_blitter.h"
#include "../d3d9-postprocess/postprocess.h"

namespace {
using namespace dxvk;
using Surface = PostFXRef<IDirect3DSurface9>;
using Pixels = std::vector<uint32_t>;

void expect(bool ok, const char *action) {
    if (!ok)
        throw std::runtime_error(action);
}
void checked(HRESULT hr, const char *action) {
    if (FAILED(hr)) {
        char detail[24];
        std::snprintf(detail, sizeof detail, " (0x%08x)", unsigned(hr));
        throw std::runtime_error(std::string(action) + detail);
    }
}
#define CHECK(call) checked((call), #call)

struct Window {
    SDL_Window *handle = nullptr;
    Window() {
        SDL_SetMainReady();
        expect(SDL_Init(SDL_INIT_VIDEO) == 0 && SDL_Vulkan_LoadLibrary(nullptr) == 0, SDL_GetError());
        handle =
            SDL_CreateWindow("Display grade test", 0, 0, 80, 60, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        expect(handle != nullptr, SDL_GetError());
    }
    ~Window() {
        SDL_DestroyWindow(handle);
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
    }
};

struct Rendering {
    Window window;
    PostFXRef<IDirect3D9> d3d;
    PostFXRef<IDirect3DDevice9> device;
    Rc<DxvkDevice> gpu;
    Rc<DxvkContext> context;

    Rendering() {
        *d3d.put() = Direct3DCreate9(D3D_SDK_VERSION);
        expect(d3d.get(), "Direct3DCreate9");
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.BackBufferFormat = D3DFMT_A8R8G8B8;
        p.BackBufferWidth = 80;
        p.BackBufferHeight = 60;
        p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        p.hDeviceWindow = reinterpret_cast<HWND>(window.handle);
        CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, p.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                &p, device.put()));
        gpu = static_cast<D3D9DeviceEx *>(device.get())->GetDXVKDevice();
        context = gpu->createContext();
        D3DADAPTER_IDENTIFIER9 id{};
        CHECK(d3d->GetAdapterIdentifier(0, 0, &id));
        std::printf("adapter=%s\n", id.Description);
    }
    void finish_d3d() {
        auto *native = static_cast<D3D9DeviceEx *>(device.get());
        native->Flush();
        native->SynchronizeCsThread();
        gpu->waitForIdle();
    }
    void target(Surface &out, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples) {
        CHECK(device->CreateRenderTarget(width, height, format, samples, 0, FALSE, out.put(), nullptr));
    }
    Pixels read(IDirect3DSurface9 *rt) {
        D3DSURFACE_DESC desc{};
        CHECK(rt->GetDesc(&desc));
        Surface resolved, host;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            target(resolved, desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE);
            CHECK(device->StretchRect(rt, nullptr, resolved.get(), nullptr, D3DTEXF_NONE));
            rt = resolved.get();
        }
        CHECK(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                  D3DPOOL_SYSTEMMEM, host.put(), nullptr));
        CHECK(device->GetRenderTargetData(rt, host.get()));
        D3DLOCKED_RECT locked{};
        CHECK(host->LockRect(&locked, nullptr, D3DLOCK_READONLY));
        Pixels pixels(size_t(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(pixels.data() + y * desc.Width,
                        static_cast<const char *>(locked.pBits) + y * locked.Pitch, desc.Width * 4);
        CHECK(host->UnlockRect());
        if (desc.Format == D3DFMT_X8R8G8B8)
            for (uint32_t &pixel : pixels)
                pixel |= 0xff000000u;
        return pixels;
    }
};

constexpr uint32_t colors[] = {0x40385b81, 0xbad86822, 0x7d20beb2, 0xdd8c4821};

void paint_source(Rendering &r, IDirect3DSurface9 *surface, unsigned w, unsigned h,
                  bool mixed_samples) {
    for (unsigned i = 0; i < 4; ++i) {
        RECT region{LONG(i & 1 ? w / 2 : 0), LONG(i & 2 ? h / 2 : 0), LONG(i & 1 ? w : w / 2),
                    LONG(i & 2 ? h : h / 2)};
        CHECK(r.device->ColorFill(surface, &region, colors[i]));
    }
    if (!mixed_samples)
        return;
    Surface old;
    CHECK(r.device->GetRenderTarget(0, old.put()));
    CHECK(r.device->SetRenderTarget(0, surface));
    CHECK(r.device->SetDepthStencilSurface(nullptr));
    CHECK(r.device->SetPixelShader(nullptr));
    CHECK(r.device->SetVertexShader(nullptr));
    CHECK(r.device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    CHECK(r.device->SetRenderState(D3DRS_ZENABLE, FALSE));
    CHECK(r.device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    CHECK(r.device->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(r.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(r.device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf));
    CHECK(r.device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    CHECK(r.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    CHECK(r.device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE));
    CHECK(r.device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0x55555555));
    CHECK(r.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(r.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    CHECK(r.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    CHECK(r.device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    struct Vertex {
        float x, y, z, rhw;
        DWORD color;
    };
    const float right = float(w) - .5f, bottom = float(h) - .5f;
    constexpr DWORD ink = 0xeaff0000;
    const Vertex quad[] = {{-.5f, -.5f, 0, 1, ink},
                           {right, -.5f, 0, 1, ink},
                           {-.5f, bottom, 0, 1, ink},
                           {right, bottom, 0, 1, ink}};
    CHECK(r.device->BeginScene());
    CHECK(r.device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)));
    CHECK(r.device->EndScene());
    CHECK(r.device->SetRenderState(D3DRS_MULTISAMPLEMASK, ~DWORD(0)));
    CHECK(r.device->SetRenderTarget(0, old.get()));
}

struct GameState {
    Surface target;
    DWORD fvf;
    D3DVIEWPORT9 viewport{2, 3, 19, 11, 0.25f, 0.75f};
    RECT scissor{3, 5, 13, 7};
    explicit GameState(IDirect3DDevice9 *d) {
        CHECK(d->GetRenderTarget(0, target.put()));
        CHECK(d->SetViewport(&viewport));
        CHECK(d->SetScissorRect(&scissor));
        CHECK(d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
        CHECK(d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED));
        CHECK(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        CHECK(d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
        CHECK(d->GetFVF(&fvf));
    }
    void verify(IDirect3DDevice9 *d) const {
        Surface rt;
        DWORD mask, blend, scissors, vertex;
        D3DVIEWPORT9 vp{};
        RECT rect{};
        CHECK(d->GetRenderTarget(0, rt.put()));
        CHECK(d->GetViewport(&vp));
        CHECK(d->GetScissorRect(&rect));
        CHECK(d->GetRenderState(D3DRS_COLORWRITEENABLE, &mask));
        CHECK(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend));
        CHECK(d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissors));
        CHECK(d->GetFVF(&vertex));
        expect(rt.get() == target.get() && std::memcmp(&vp, &viewport, sizeof vp) == 0 &&
                   std::memcmp(&rect, &scissor, sizeof rect) == 0 && mask == D3DCOLORWRITEENABLE_RED &&
                   blend == TRUE && scissors == TRUE && vertex == fvf,
               "guest draw state changed");
    }
};

// The test can bracket whole batches on the queue when a graphics timestamp is supported.
struct QueueClock {
    Rc<DxvkGpuQuery> begin, end;
    double period_ns = 0;
    uint64_t mask = 0;
    explicit QueueClock(const Rc<DxvkDevice> &device) {
        auto adapter = device->adapter();
        uint32_t count = 0;
        auto query = adapter->vki()->vkGetPhysicalDeviceQueueFamilyProperties;
        query(adapter->handle(), &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        query(adapter->handle(), &count, properties.data());
        const uint32_t family = device->queues().graphics.queueFamily;
        expect(family < count, "graphics queue family");
        const uint32_t bits = properties[family].timestampValidBits;
        expect(bits <= 64, "graphics queue timestamp width");
        period_ns = adapter->deviceProperties().limits.timestampPeriod;
        if (!bits || period_ns <= 0)
            return;
        mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
        begin = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
        end = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    }
    double elapsed_us() const {
        DxvkQueryData first{}, last{};
        expect(begin->getData(first) == DxvkGpuQueryStatus::Available &&
                   end->getData(last) == DxvkGpuQueryStatus::Available,
               "completed queue timestamps");
        return ((last.timestamp.time - first.timestamp.time) & mask) * period_ns / 1000.;
    }
};

struct Measured {
    uint64_t draws = 0, render_passes = 0, submits = 0;
    double microseconds = 0;
    double queue_us = 0;
};
template <class Blitter>
Measured present(Rendering &r, Blitter &blitter, IDirect3DSurface9 *source, VkRect2D srcRect,
                 IDirect3DSurface9 *dest, VkRect2D dstRect, unsigned draws = 1,
                 QueueClock *clock = nullptr) {
    auto src = static_cast<D3D9Surface *>(source)->GetImageView(false);
    auto dst = static_cast<D3D9Surface *>(dest)->GetRenderTargetView(false);
    expect(src != nullptr && dst != nullptr && src->image() != dst->image(),
           "separate source/destination views");
    auto before = r.gpu->getStatCounters();
    auto start = std::chrono::steady_clock::now();
    r.context->beginRecording(r.gpu->createCommandList());
    if (clock)
        r.context->writeTimestamp(clock->begin);
    for (unsigned i = 0; i < draws; ++i)
        blitter.presentImage(r.context.ptr(), dst, dstRect, src, srcRect);
    if (clock)
        r.context->writeTimestamp(clock->end);
    r.gpu->submitCommandList(r.context->endRecording(), VK_NULL_HANDLE, VK_NULL_HANDLE);
    r.gpu->waitForIdle();
    auto elapsed =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    auto delta = r.gpu->getStatCounters().diff(before);
    return {delta.getCtr(DxvkStatCounter::CmdDrawCalls),
            delta.getCtr(DxvkStatCounter::CmdRenderPassCount),
            delta.getCtr(DxvkStatCounter::QueueSubmitCount), elapsed, clock ? clock->elapsed_us() : 0};
}

uint32_t grade_reference(uint32_t input) {
    double red = ((input >> 16) & 255) / 255., green = ((input >> 8) & 255) / 255.,
           blue = (input & 255) / 255.;
    double luma = .2126 * red + .7152 * green + .0722 * blue;
    auto channel = [&](double value, double gain) {
        return uint32_t(std::lround(std::clamp((.1 * luma + .9 * value) * gain, 0., 1.) * 255));
    };
    return (input & 0xff000000) | (channel(red, 1.08) << 16) | (channel(green, 1.) << 8) |
           channel(blue, .90);
}
bool near(uint32_t a, uint32_t b) {
    if ((a >> 24) != (b >> 24))
        return false;
    for (unsigned i = 0; i < 24; i += 8)
        if (std::abs(int((a >> i) & 255) - int((b >> i) & 255)) > 2)
            return false;
    return true;
}

void run_case(Rendering &r, DxvkSwapchainBlitter &original, SkyrimGradeTrialBlitter &off,
              SkyrimGradeTrialBlitter &on, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, bool scale,
              bool gamma) {
    constexpr unsigned w = 23, h = 13, dw = 35, dh = 19;
    unsigned width = scale ? dw : w, height = scale ? dh : h;
    Surface source, out;
    r.target(source, w, h, format, samples);
    r.target(out, width, height, format, D3DMULTISAMPLE_NONE);
    paint_source(r, source.get(), w, h, samples != D3DMULTISAMPLE_NONE);
    auto original_source = r.read(source.get());
    if (samples != D3DMULTISAMPLE_NONE) {
        auto pix = original_source[(h / 4) * w + w / 4];
        expect((pix & 0x00ffffffu) != (colors[0] & 0x00ffffffu) && (pix & 0x00ffffffu) != 0xff0000,
               "missing mixed MSAA subsamples");
    }
    GameState state(r.device.get());
    r.finish_d3d();
    // Same-sized test crops with non-zero offsets. Scaling uses the full target.
    const VkRect2D src = scale ? VkRect2D{{0, 0}, {w, h}} : VkRect2D{{1, 1}, {w - 3, h - 3}};
    const VkRect2D dst = scale ? VkRect2D{{0, 0}, {width, height}} : VkRect2D{{2, 2}, src.extent};
    std::array<DxvkGammaCp, 256> lut{};
    for (auto &point : lut)
        point = {0x5555, 0x9999, 0xbbbb, 0xffff};
    const uint32_t points = gamma ? uint32_t(lut.size()) : 0;
    original.setGammaRamp(points, lut.data());
    off.setGammaRamp(points, lut.data());
    on.setGammaRamp(points, lut.data());
    auto standard = present(r, original, source.get(), src, out.get(), dst);
    auto expected = r.read(out.get());
    r.finish_d3d();
    auto disabled = present(r, off, source.get(), src, out.get(), dst);
    expect(r.read(out.get()) == expected, "disabled pixels differ from the original DXVK binary");
    r.finish_d3d();
    auto enabled = present(r, on, source.get(), src, out.get(), dst);
    auto actual = r.read(out.get());
    unsigned changed = 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            size_t i = size_t(y) * width + x;
            bool inside = int(x) >= dst.offset.x && x < unsigned(dst.offset.x) + dst.extent.width &&
                          int(y) >= dst.offset.y && y < unsigned(dst.offset.y) + dst.extent.height;
            uint32_t wanted = expected[i];
            if (inside && !gamma)
                wanted = grade_reference(wanted);
            if (inside && gamma)
                expect((expected[i] & 0xffffffu) == 0x5599bb, "test gamma not bound");
            if (!near(wanted, actual[i])) {
                std::fprintf(stderr, "pixel %u,%u expected %08x got %08x\n", x, y, wanted, actual[i]);
                throw std::runtime_error("grade mismatch");
            }
            changed += expected[i] != actual[i];
        }
    expect(gamma ? changed == 0 : changed > 0, "effect applied before existing gamma ramp");
    for (const Measured *m : {&disabled, &enabled})
        expect(standard.draws == 1 && m->draws == standard.draws &&
                   standard.render_passes == m->render_passes && standard.submits == m->submits,
               "unexpected extra draw, render pass, or queue submission");
    expect(r.read(source.get()) == original_source, "source contents changed");
    state.verify(r.device.get());
    std::printf("samples=%u scale=%u gamma=%u format=%d pixels=%u changed=%u draws=%llu passes=%llu "
                "submits=%llu\n",
                unsigned(samples), unsigned(scale), unsigned(gamma), int(format), width * height,
                changed, (unsigned long long)enabled.draws, (unsigned long long)enabled.render_passes,
                (unsigned long long)enabled.submits);
}

void benchmark(Rendering &r, DxvkSwapchainBlitter &original, SkyrimGradeTrialBlitter &on) {
    Surface src, dst;
    constexpr unsigned width = 480, height = 270, batch = 16;
    r.target(src, width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
    r.target(dst, width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
    paint_source(r, src.get(), width, height, false);
    r.finish_d3d();
    VkRect2D bounds{{0, 0}, {width, height}};
    original.setGammaRamp(0, nullptr);
    on.setGammaRamp(0, nullptr);
    present(r, original, src.get(), bounds, dst.get(), bounds, batch);
    present(r, on, src.get(), bounds, dst.get(), bounds, batch);
    QueueClock timer(r.gpu);
    QueueClock *clock = timer.mask ? &timer : nullptr;
    struct Samples {
        std::vector<double> completion, queue;
    };
    Samples old, warm;
    for (unsigned i = 0; i < 12; ++i) {
        auto record = [&](auto &blitter, Samples &measurements) {
            auto m = present(r, blitter, src.get(), bounds, dst.get(), bounds, batch, clock);
            expect(m.draws == batch, "benchmark draw count");
            measurements.completion.push_back(m.microseconds / batch);
            if (clock)
                measurements.queue.push_back(m.queue_us / batch);
        };
        if (i % 2) {
            record(on, warm);
            record(original, old);
        } else {
            record(original, old);
            record(on, warm);
        }
    }
    auto median = [](std::vector<double> data) {
        std::sort(data.begin(), data.end());
        return .5 * (data[5] + data[6]);
    };
    auto report = [&](const char *unit, const std::vector<double> &original,
                      const std::vector<double> &graded) {
        std::vector<double> difference;
        for (size_t i = 0; i < original.size(); ++i)
            difference.push_back(graded[i] - original[i]);
        std::printf(
            "%s median original=%.2f warm=%.2f paired_difference=%.2f batches=12x16 viewport=480x270\n",
            unit, median(original), median(graded), median(difference));
    };
    report("completed_batch_us_per_draw", old.completion, warm.completion);
    if (clock)
        report("vulkan_queue_us_per_draw", old.queue, warm.queue);
    else
        std::puts("vulkan_queue_us_per_draw unavailable on this graphics queue");
}

void check_staged_host(Rendering &r) {
    constexpr unsigned width = 17, height = 9;
    constexpr const char *setting = "SKYRIM_TRIAL_PRESENT_GRADE";
    Surface src, dst;
    r.target(src, width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
    r.target(dst, width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
    paint_source(r, src.get(), width, height, false);
    GameState state(r.device.get());
    r.finish_d3d();
    const VkRect2D bounds{{0, 0}, {width, height}};
    auto capture = [&](auto &blitter) {
        auto sample = present(r, blitter, src.get(), bounds, dst.get(), bounds);
        expect(sample.draws == 1 && sample.render_passes == 1, "staged host draw count");
        auto result = r.read(dst.get());
        r.finish_d3d();
        return result;
    };
    SkyrimGradeTrialBlitter plain(r.gpu, false), warm(r.gpu, true);
    const Pixels reference = capture(plain), graded = capture(warm);
    expect(reference != graded, "staged test requires distinguishable images");
    expect(SDL_setenv(setting, "1", 1) == 0, "select warm display");
    DxvkSwapchainBlitter enabled(r.gpu);
    expect(SDL_setenv(setting, "0", 1) == 0, "select original display");
    DxvkSwapchainBlitter disabled(r.gpu);
    expect(SDL_setenv(setting, "10", 1) == 0, "try unsupported setting");
    DxvkSwapchainBlitter unknown(r.gpu);
    expect(capture(enabled) == graded, "actual host missed its warm selection at creation");
    expect(capture(disabled) == reference && capture(unknown) == reference,
           "actual host original selection");
    state.verify(r.device.get());
    // The production API wrappers forward to this same native swapchain. This
    // smoke check preserves return codes; readback above uses an inspectable destination.
    PostFXRef<IDirect3DSwapChain9> chain;
    CHECK(r.device->GetSwapChain(0, chain.put()));
    CHECK(r.device->Present(nullptr, nullptr, nullptr, nullptr));
    CHECK(chain->Present(nullptr, nullptr, nullptr, nullptr, 0));
    state.verify(r.device.get());
    std::puts("staged_host=verified select-on select-off unknown-off latched-at-creation "
              "device-present chain-present");
}
} // namespace

int main(int argc, char **argv) {
    try {
        const bool host_check = argc == 2 && !std::strcmp(argv[1], "--staged-check");
        if (argc > 2 || (argc == 2 && !host_check && std::strcmp(argv[1], "--bench")))
            return 2;
        expect(SDL_setenv("SKYRIM_TRIAL_PRESENT_GRADE", "0", 1) == 0, "set original test baseline");
        Rendering r;
        if (host_check) {
            check_staged_host(r);
            return 0;
        }
        DxvkSwapchainBlitter original(r.gpu);
        SkyrimGradeTrialBlitter off(r.gpu, false), on(r.gpu, true);
        for (auto format : {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8})
            for (auto samples :
                 {D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES}) {
                if (FAILED(r.d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, format, TRUE, samples,
                                                             nullptr)))
                    continue;
                for (bool scale : {false, true})
                    for (bool gamma : {false, true})
                        run_case(r, original, off, on, format, samples, scale, gamma);
            }
        if (argc == 2)
            benchmark(r, original, on);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Present grade: %s\n", e.what());
        return 1;
    }
}
