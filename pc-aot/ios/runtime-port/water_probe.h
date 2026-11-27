/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
enum { SKYRIM_WATER_PROBE_SURFACE = 1, SKYRIM_WATER_PROBE_SUBMERGED = 2 };
/* A repeat request starts a fresh four-second lead-in. */
void skyrim_water_probe_request(unsigned scene);
#ifdef __cplusplus
}

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace skyrim_water {
enum class Family : uint8_t { Unparsed, Unknown, Other, WaterFog, ImageBlur, ImageDepth };
struct Layout {
    Family family = Family::Unparsed;
    int16_t depth = -1, color = -1, params = -1, camera = -1;
};
Layout classify(const uint8_t* code, size_t length);

enum class Binding : uint8_t { Undeclared, Unbound, ColorUsage, DepthStencilUsage };
enum class Params : uint8_t { Undeclared, Zero, FiniteNonzero, Nonfinite };
Params classifyParams(const float* values);
// Denominator of the stock water shader's reciprocal over normalized depth [0, 1].
enum class Camera : uint8_t {
    Unavailable, Nonfinite, FlatZero, FlatNonzero, ZeroAtDepth0, ZeroAtDepth1,
    CrossesZero, Increasing, Decreasing
};
Camera classifyCamera(const float* values);
struct Inputs {
    Binding depth = Binding::Undeclared, color = Binding::Undeclared;
    bool depthMultisampled = false;
    Params params = Params::Undeclared;
    Camera camera = Camera::Unavailable;
};
using ReadInputs = Inputs (*)(void* device, const Layout& layout);
struct Counts {
    unsigned matches = 0;
    // In Binding / Params enum order. color collapses both bound types.
    std::array<unsigned, 4> depth{}, params{};
    std::array<unsigned, 3> color{};
    unsigned multisampled = 0;
};
enum class Phase : unsigned { Idle, Waiting, Capturing };
enum class End : unsigned { None, Complete, Expired };
struct Window {
    static constexpr unsigned Frames = 3, DrawsPerFrame = 8192;
    static constexpr size_t MaxShaderBytes = 64 * 1024, BytesPerFrame = 2 * 1024 * 1024;
    Phase phase = Phase::Idle;
    unsigned scene = 0, frames = 0, checked = 0, fixed = 0, unknown = 0, other = 0;
    bool limited = false;
    std::array<Counts, 3> groups{};
    std::array<unsigned, 9> cameraCounts{};
    void request(unsigned label, uint64_t nowMs);
    End expire(uint64_t nowMs);
    End present(void* device, const void* chain, uint64_t nowMs);
    void draw(void* device, Layout* cached, const uint8_t* code, size_t length, ReadInputs read);
private:
    uint64_t requestedMs = 0;
    const void* activeDevice = nullptr;
    const void* activeChain = nullptr;
    unsigned frameDraws = 0;
    size_t frameBytes = 0;
};
// DXVK checks only this atomic when the probe is idle.
extern std::atomic<Phase> phase;
void present(void* device, const void* chain);
void draw(void* device, Layout* cached, const uint8_t* code, size_t length, ReadInputs read);
}
#endif
