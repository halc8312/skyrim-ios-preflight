/* SPDX-License-Identifier: GPL-3.0-only */
#include "water_probe.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

namespace skyrim_water {
namespace {
uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
Layout unknown() { return {Family::Unknown, -1, -1, -1}; }

// D3D9 CTAB offsets are relative to the header immediately after the FourCC.
// Identify semantics from declared register sets, never from fixed slot numbers.
Layout table(const uint8_t* base, size_t bytes, uint32_t version) {
    if (bytes < 28 || u32(base) != 28 || u32(base + 8) != version) return unknown();
    const auto count = u32(base + 12), offset = u32(base + 16);
    if (count > 128 || offset > bytes || count > (bytes - offset) / 20 || (count && offset < 28))
        return unknown();
    int16_t water = -1, fog = -1, refraction = -1, waterDepth = -1;
    int16_t imageDepth = -1, blur = -1, depthParams = -1, camera = -1;
    bool sawCamera = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = base + offset + i * 20;
        const uint32_t nameOffset = u32(entry);
        if (nameOffset >= bytes) return unknown();
        const char* name = reinterpret_cast<const char*>(base + nameOffset);
        const size_t available = bytes - nameOffset;
        const void* end = std::memchr(name, 0, available < 128 ? available : 128);
        if (!end) return unknown();
        const std::string_view key(name, static_cast<const char*>(end) - name);
        const auto set = u16(entry + 4), reg = u16(entry + 6), span = u16(entry + 8);
        if (key == "CameraData") {
            const uint32_t typeOffset = u32(entry + 12);
            // Only read X/Z for one reflected D3DXPC_VECTOR/D3DXPT_FLOAT float4.
            // A missing, ambiguous or differently typed camera leaves the other observations intact.
            const bool recordFits = typeOffset >= 28 && typeOffset <= bytes && bytes - typeOffset >= 16;
            const uint8_t* type = recordFits ? base + typeOffset : nullptr;
            camera = !sawCamera && set == 2 && span == 1 && reg < 224 && type &&
                     u16(type) == 1 && u16(type + 2) == 3 && u16(type + 4) == 1 &&
                     u16(type + 6) == 4 && u16(type + 8) == 1 && u16(type + 10) == 0
                     ? static_cast<int16_t>(reg) : -1;
            sawCamera = true;
        }
        int16_t* slot = nullptr;
        unsigned limit = 0;
        if (set == 2) {
            limit = 224; // PS 3.0 float4 registers
            if (key == "WaterParams") slot = &water;
            else if (key == "FogParam") slot = &fog;
            else if (key == "DepthParams") slot = &depthParams;
        } else if (set == 3) {
            limit = 16; // pixel samplers
            if (key == "RefractionSampler") slot = &refraction;
            else if (key == "DepthSampler") slot = &waterDepth;
            else if (key == "Blurred") slot = &blur;
            else if (key == "Depth") slot = &imageDepth;
        }
        if (slot) {
            if (*slot >= 0 || !span || reg >= limit || span > limit - reg) return unknown();
            *slot = static_cast<int16_t>(reg);
        }
    }
    if (water >= 0 && fog >= 0 && refraction >= 0)
        return {Family::WaterFog, waterDepth, refraction, fog, waterDepth >= 0 ? camera : int16_t(-1)};
    if (imageDepth >= 0 && blur >= 0) return {Family::ImageBlur, imageDepth, blur, depthParams};
    if (imageDepth >= 0 && depthParams >= 0) return {Family::ImageDepth, imageDepth, -1, depthParams};
    return {Family::Other, -1, -1, -1};
}

Window window;
std::mutex windowLock;
uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
void report(const Window& w, End end) {
    for (size_t i = 0; i < w.groups.size(); ++i) {
        const auto& g = w.groups[i];
        if (!g.matches) continue;
        std::fprintf(stderr, "IOS_WATER_DRAW schema=1 scene=%u kind=%u count=%u depth=%u,%u,%u,%u "
                     "ms_depth=%u color=%u,%u,%u params=%u,%u,%u,%u\n",
                     w.scene, unsigned(i + 1), g.matches, g.depth[0], g.depth[1], g.depth[2], g.depth[3],
                     g.multisampled, g.color[0], g.color[1], g.color[2],
                     g.params[0], g.params[1], g.params[2], g.params[3]);
    }
    if (w.groups[0].matches) {
        const auto& c = w.cameraCounts;
        std::fprintf(stderr, "IOS_WATER_CAMERA schema=1 scene=%u count=%u endpoints=%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                     w.scene, w.groups[0].matches, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
    }
    std::fprintf(stderr, "IOS_WATER_SUMMARY schema=1 scene=%u end=%u frames=%u checked=%u "
                 "fixed=%u unknown=%u other=%u limited=%u\n", w.scene, unsigned(end), w.frames,
                 w.checked, w.fixed, w.unknown, w.other, unsigned(w.limited));
}
}

Layout classify(const uint8_t* code, size_t length) {
    if (!code || length < 8 || (length & 3) || length > Window::MaxShaderBytes) return unknown();
    const uint32_t version = u32(code);
    if (version != 0xffff0300 && version != 0xffff0200) return unknown();
    for (size_t pos = 4; pos + 4 <= length;) {
        const uint32_t token = u32(code + pos), opcode = token & 0xffff;
        pos += 4;
        if (opcode == 0xffff) break;
        const bool comment = opcode == 0xfffe;
        const size_t words = comment ? (token >> 16) & 0x7fff : (token >> 24) & 15;
        if (words > (length - pos) / 4) return unknown();
        if (comment && words && u32(code + pos) == 0x42415443)
            return table(code + pos + 4, (words - 1) * 4, version);
        pos += words * 4;
    }
    return unknown(); // stripped, malformed or unsupported bytecode
}

Params classifyParams(const float* values) {
    if (!values) return Params::Undeclared;
    bool nonzero = false;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(values[i])) return Params::Nonfinite;
        nonzero |= values[i] != 0.0f;
    }
    return nonzero ? Params::FiniteNonzero : Params::Zero;
}

Camera classifyCamera(const float* values) {
    if (!values) return Camera::Unavailable;
    const float first = values[0], scale = values[2];
    if (!std::isfinite(first) || !std::isfinite(scale)) return Camera::Nonfinite;
    const float last = first - scale;
    if (!std::isfinite(last)) return Camera::Nonfinite;
    if (first == last) return first == 0.0f ? Camera::FlatZero : Camera::FlatNonzero;
    if (first == 0.0f) return Camera::ZeroAtDepth0;
    if (last == 0.0f) return Camera::ZeroAtDepth1;
    if (std::signbit(first) != std::signbit(last)) return Camera::CrossesZero;
    return last > first ? Camera::Increasing : Camera::Decreasing;
}

void Window::request(unsigned label, uint64_t now) {
    if (label != SKYRIM_WATER_PROBE_SURFACE && label != SKYRIM_WATER_PROBE_SUBMERGED) return;
    *this = Window{};
    phase = Phase::Waiting;
    requestedMs = now;
    scene = label;
}

End Window::expire(uint64_t now) {
    if (phase != Phase::Idle && now >= requestedMs && now - requestedMs > 15000) {
        phase = Phase::Idle;
        return End::Expired;
    }
    return End::None;
}

End Window::present(void* device, const void* chain, uint64_t now) {
    if (expire(now) == End::Expired) return End::Expired;
    if (phase == Phase::Idle) return End::None;
    if (!device || !chain || now < requestedMs || now - requestedMs < 4000) return End::None;
    if (phase == Phase::Waiting) {
        activeDevice = device;
        activeChain = chain;
        phase = Phase::Capturing;
    } else if (device == activeDevice && chain == activeChain) {
        if (++frames == Frames) {
            phase = Phase::Idle;
            return End::Complete;
        }
    } else {
        return End::None;
    }
    frameDraws = 0;
    frameBytes = 0;
    return End::None;
}

void Window::draw(void* device, Layout* cached, const uint8_t* code, size_t length, ReadInputs read) {
    if (phase != Phase::Capturing || device != activeDevice) return;
    if (frameDraws == DrawsPerFrame) { limited = true; return; }
    ++frameDraws;
    ++checked;
    if (!cached) { ++fixed; return; }
    if (cached->family == Family::Unparsed) {
        if (length > MaxShaderBytes || length > BytesPerFrame - frameBytes) {
            limited = true;
            ++unknown;
            return;
        }
        frameBytes += length;
        *cached = classify(code, length);
    }
    if (cached->family == Family::Unknown) { ++unknown; return; }
    if (cached->family == Family::Other) { ++other; return; }
    const auto inputs = read(device, *cached);
    auto& group = groups[unsigned(cached->family) - unsigned(Family::WaterFog)];
    ++group.matches;
    ++group.depth[unsigned(inputs.depth)];
    const auto color = unsigned(inputs.color);
    ++group.color[color < 2 ? color : 2];
    ++group.params[unsigned(inputs.params)];
    group.multisampled += inputs.depthMultisampled;
    if (cached->family == Family::WaterFog) ++cameraCounts[unsigned(inputs.camera)];
}

std::atomic<Phase> phase{Phase::Idle};
void present(void* device, const void* chain) {
    Window completed;
    End end;
    {
        std::lock_guard<std::mutex> guard(windowLock);
        end = window.present(device, chain, nowMs());
        phase.store(window.phase, std::memory_order_relaxed);
        if (end != End::None) completed = window;
    }
    if (end != End::None) report(completed, end);
}
void draw(void* device, Layout* cached, const uint8_t* code, size_t length, ReadInputs read) {
    Window expired;
    {
        std::lock_guard<std::mutex> guard(windowLock);
        // A resumed app can issue draws before its first successful Present.
        if (window.expire(nowMs()) != End::Expired) {
            window.draw(device, cached, code, length, read);
            return;
        }
        phase.store(Phase::Idle, std::memory_order_relaxed);
        expired = window;
    }
    report(expired, End::Expired);
}
} // namespace skyrim_water

extern "C" void skyrim_water_probe_request(unsigned scene) {
    using namespace skyrim_water;
    std::lock_guard<std::mutex> guard(windowLock);
    window.request(scene, nowMs());
    phase.store(window.phase, std::memory_order_relaxed);
}
