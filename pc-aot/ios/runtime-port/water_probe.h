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
enum ViewFlags : unsigned {
    ViewBound = 1, ViewRenderTarget = 2, ViewDepthStencil = 4,
    ViewSrgbRequested = 8, ViewShadow = 16, ViewAliasesColor = 32, ViewAliasesDepth = 64
};
struct SampleView {
    unsigned format = 0, aspect = 0, width = 0, height = 0, flags = 0;
    bool operator==(const SampleView& other) const {
        return format == other.format && aspect == other.aspect && width == other.width &&
               height == other.height && flags == other.flags;
    }
};
struct ViewCount { SampleView view{}; unsigned draws = 0; };
enum class PixelEncoding : uint8_t { R32Float, R16Float, R8Unorm, Rgba8Unorm, Bgra8Unorm };
enum class PixelRole : unsigned { Depth = 1, Refraction = 2 };
struct PixelPoint { uint32_t x, y; };
using PixelPoints = std::array<PixelPoint, 5>;
struct CaptureTag {
    uint64_t sequence = 0;
    unsigned scene = 0, draw = 0;
};
// Five single texels at the center and in the four quadrants. Tiny views are omitted.
bool pixelPoints(uint32_t width, uint32_t height, PixelPoints& result);
struct PixelSummary {
    unsigned nonfinite = 0, outside = 0, zero = 0, one = 0, interior = 0;
    bool varies = false;
};
// Each texel starts four bytes after the previous one, regardless of its format.
// Only counts leave the probe. For refraction compare RGB, ignoring alpha.
bool summarizePixels(const uint8_t* bytes, size_t length, PixelEncoding encoding,
                     PixelRole role, PixelSummary& result);
struct Inputs {
    Binding depth = Binding::Undeclared, color = Binding::Undeclared;
    bool depthMultisampled = false;
    Params params = Params::Undeclared;
    Camera camera = Camera::Unavailable;
    std::array<SampleView, 2> views{}; // WaterFog depth and refraction, respectively
};
using ReadInputs = Inputs (*)(void* device, const Layout& layout, CaptureTag tag);
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
    std::array<std::array<ViewCount, 8>, 2> sampleViews{};
    void request(unsigned label, uint64_t nowMs);
    End expire(uint64_t nowMs);
    End present(void* device, const void* chain, uint64_t nowMs);
    void draw(void* device, Layout* cached, const uint8_t* code, size_t length, ReadInputs read);
    bool acceptsPixels(CaptureTag tag, uint64_t nowMs) const;
private:
    uint64_t sequence = 0;
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
bool pixelsCurrent(CaptureTag tag);
void reportPixels(CaptureTag tag, PixelRole role, unsigned format, const PixelSummary& summary);
}
#endif
