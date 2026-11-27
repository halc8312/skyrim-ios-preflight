/* SPDX-License-Identifier: GPL-3.0-only */
#include "water_probe.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <thread>
#include <vector>

using namespace skyrim_water;
using Bytes = std::vector<uint8_t>;
static void set(Bytes& out, size_t at, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) out.at(at + i) = uint8_t(value >> (8 * i));
}
struct Constant { const char* name; unsigned set, reg, span = 1, columns = 4; };
static Bytes pixel(std::initializer_list<Constant> constants) {
    constexpr uint32_t version = 0xffff0300;
    Bytes header(28 + 20 * constants.size(), 0);
    set(header, 0, 28, 4);
    set(header, 8, version, 4);
    set(header, 12, uint32_t(constants.size()), 4);
    set(header, 16, 28, 4);
    size_t i = 28;
    for (auto c : constants) {
        set(header, i, uint32_t(header.size()), 4);
        set(header, i + 4, c.set, 2);
        set(header, i + 6, c.reg, 2);
        set(header, i + 8, c.span, 2);
        header.insert(header.end(), c.name, c.name + std::strlen(c.name) + 1);
        if (std::strcmp(c.name, "CameraData") == 0) {
            const size_t start = header.size();
            set(header, i + 12, uint32_t(start), 4);
            header.resize(start + 16);
            set(header, start, 1, 2); // D3DXPC_VECTOR
            set(header, start + 2, 3, 2); // D3DXPT_FLOAT
            set(header, start + 4, 1, 2);
            set(header, start + 6, c.columns, 2);
            set(header, start + 8, 1, 2);
        }
        i += 20;
    }
    header.resize((header.size() + 3) & ~size_t(3));
    Bytes shader(28, 0);
    set(shader, 0, version, 4);
    set(shader, 4, 0x01000001, 4);  // one operand whose bits look like a comment
    set(shader, 8, 0x0002fffe, 4);
    set(shader, 12, 0x0001fffe, 4);
    set(shader, 16, 0x4f4d4544, 4); // unrelated comment
    set(shader, 20, ((1 + uint32_t(header.size() / 4)) << 16) | 0xfffe, 4);
    set(shader, 24, 0x42415443, 4);
    shader.insert(shader.end(), header.begin(), header.end());
    shader.resize(shader.size() + 4);
    set(shader, shader.size() - 4, 0xffff, 4);
    return shader;
}
static Layout layout(const Bytes& bytes) { return classify(bytes.data(), bytes.size()); }
static unsigned reads = 0;
static Inputs readInputs(void*, const Layout& kind) {
    ++reads;
    assert(kind.family == Family::WaterFog && kind.depth == 11 && kind.color == 8 && kind.params == 62 && kind.camera == 93);
    const float camera[4] = {4.0f, 0.0f, 3.0f, 0.0f};
    return {Binding::ColorUsage, Binding::Unbound, false, Params::FiniteNonzero, classifyCamera(camera)};
}
int main(int argc, char**) {
    const auto water = pixel({{"DepthSampler", 3, 11}, {"FogParam", 2, 62},
                              {"RefractionSampler", 3, 8}, {"WaterParams", 2, 23}, {"CameraData", 2, 93}});
    const auto w = layout(water);
    assert(w.family == Family::WaterFog && w.depth == 11 && w.color == 8 && w.params == 62 && w.camera == 93);
    const auto without = layout(pixel({{"FogParam", 2, 1}, {"WaterParams", 2, 2}, {"RefractionSampler", 3, 7},
                                       {"CameraData", 2, 4}}));
    assert(without.family == Family::WaterFog && without.depth == -1 && without.camera == -1);
    const auto wrongCamera = layout(pixel({{"FogParam", 2, 2}, {"WaterParams", 2, 4}, {"RefractionSampler", 3, 7},
                                           {"DepthSampler", 3, 11}, {"CameraData", 2, 93, 1, 3}}));
    assert(wrongCamera.family == Family::WaterFog && wrongCamera.depth == 11 && wrongCamera.camera == -1);
    auto badCamera = water;
    constexpr size_t cameraTypeOffsetField = 28 + 28 + 4 * 20 + 12;
    set(badCamera, cameraTypeOffsetField, 0xffffffffu, 4);
    assert(layout(badCamera).family == Family::WaterFog && layout(badCamera).camera == -1);
    const auto blur = layout(pixel({{"Depth", 3, 3}, {"Blurred", 3, 0}}));
    assert(blur.family == Family::ImageBlur && blur.depth == 3 && blur.color == 0 && blur.params == -1);
    const auto depth = layout(pixel({{"Depth", 3, 9}, {"DepthParams", 2, 212}}));
    assert(depth.family == Family::ImageDepth && depth.depth == 9 && depth.params == 212);
    assert(layout(pixel({{"Depth", 2, 8}, {"Blurred", 3, 7}})).family == Family::Other);
    assert(layout(pixel({{"Depth", 3, 15, 2}, {"Blurred", 3, 3}})).family == Family::Unknown);
    assert(layout(pixel({{"Depth", 3, 1}, {"Depth", 3, 1}})).family == Family::Unknown);
    assert(layout(pixel({{"DepthParams", 2, 224}})).family == Family::Unknown);
    assert(layout(pixel({{"DepthParams", 2, 9, 0}})).family == Family::Unknown);
    assert(layout(pixel({})).family == Family::Other);
    assert(classify(nullptr, 0).family == Family::Unknown);
    Bytes bad = water;
    set(bad, 0, 0xfffe0300, 4);
    assert(layout(bad).family == Family::Unknown); // vertex shader
    bad = water;
    set(bad, 28 + 16, 0xfffffff1, 4);
    assert(layout(bad).family == Family::Unknown);
    bad = water;
    set(bad, 28 + 28, uint32_t(bad.size()), 4);
    assert(layout(bad).family == Family::Unknown);
    bad = water;
    bad.resize(bad.size() - 8);
    assert(layout(bad).family == Family::Unknown);
    bad = water;
    set(bad, 24, 0, 4);
    assert(layout(bad).family == Family::Unknown); // unrecognized comment
    float c[4] = {0.0f, -0.0f, 0.0f, 0.0f};
    assert(classifyParams(nullptr) == Params::Undeclared && classifyParams(c) == Params::Zero);
    c[1] = 2.0f;
    assert(classifyParams(c) == Params::FiniteNonzero);
    c[3] = std::numeric_limits<float>::infinity();
    assert(classifyParams(c) == Params::Nonfinite);
    c[3] = std::numeric_limits<float>::quiet_NaN();
    assert(classifyParams(c) == Params::Nonfinite);
    const auto camera = [](float first, float scale) {
        const float values[4] = {first, std::numeric_limits<float>::quiet_NaN(), scale,
                                 std::numeric_limits<float>::infinity()};
        return classifyCamera(values);
    };
    const float largest = std::numeric_limits<float>::max();
    assert(classifyCamera(nullptr) == Camera::Unavailable);
    assert(camera(std::numeric_limits<float>::infinity(), 1) == Camera::Nonfinite);
    assert(camera(2, std::numeric_limits<float>::quiet_NaN()) == Camera::Nonfinite);
    assert(camera(largest, -largest) == Camera::Nonfinite);
    assert(camera(-0.0f, 0.0f) == Camera::FlatZero);
    assert(camera(8, 0) == Camera::FlatNonzero);
    assert(camera(0, -2) == Camera::ZeroAtDepth0);
    assert(camera(8, 8) == Camera::ZeroAtDepth1);
    assert(camera(2, 3) == Camera::CrossesZero);
    assert(camera(-2, -3) == Camera::CrossesZero);
    assert(camera(2, -2) == Camera::Increasing);
    assert(camera(2, 1) == Camera::Decreasing);

    Window window;
    int device = 0, swap = 0, second = 0;
    Layout cached;
    auto sample = [&] { window.draw(&device, &cached, water.data(), water.size(), readInputs); };
    window.request(99, 0);
    sample();
    assert(window.phase == Phase::Idle && window.checked == 0 && reads == 0);
    window.request(SKYRIM_WATER_PROBE_SURFACE, 10);
    assert(window.present(&device, &swap, 4009) == End::None);
    sample();
    assert(window.checked == 0 && cached.family == Family::Unparsed);
    assert(window.present(&device, &swap, 4010) == End::None);
    window.draw(&second, &cached, water.data(), water.size(), readInputs);
    assert(cached.family == Family::Unparsed);
    sample();
    // A cached layout never needs the bytecode again.
    window.draw(&device, &cached, nullptr, 0, readInputs);
    window.draw(&device, nullptr, nullptr, 0, readInputs);
    const auto another = pixel({{"Unrelated", 2, 1}});
    Layout other, stripped, over;
    window.draw(&device, &other, another.data(), another.size(), readInputs);
    window.draw(&device, &stripped, bad.data(), bad.size(), readInputs);
    window.draw(&device, &over, nullptr, Window::MaxShaderBytes + 1, readInputs);
    assert(over.family == Family::Unparsed && window.limited && window.unknown == 2 && window.other == 1);
    assert(window.groups[0].depth[2] == 2 && window.groups[0].color[1] == 2 && window.groups[0].params[2] == 2);
    assert(window.cameraCounts[unsigned(Camera::Decreasing)] == 2);
    assert(window.present(&device, &second, 4020) == End::None && window.frames == 0);
    assert(window.present(&device, &swap, 4021) == End::None && window.frames == 1);
    assert(window.present(&device, &swap, 4022) == End::None);
    assert(window.present(&device, &swap, 4023) == End::Complete);
    sample();
    assert(window.frames == 3 && window.checked == 6 && reads == 2);
    window.request(SKYRIM_WATER_PROBE_SUBMERGED, 5000);
    assert(window.checked == 0 && !window.limited && window.groups[0].matches == 0);
    assert(window.present(&device, &swap, 25000) == End::Expired && window.frames == 0);
    window.request(SKYRIM_WATER_PROBE_SURFACE, 30000);
    window.present(&device, &swap, 34000);
    assert(window.expire(46000) == End::Expired); // first draw after background
    sample();
    assert(window.checked == 0);
    window.request(SKYRIM_WATER_PROBE_SURFACE, 50000);
    window.present(&device, &swap, 54000);
    for (unsigned i = 0; i < Window::DrawsPerFrame + 3; ++i)
        window.draw(&device, nullptr, nullptr, 0, readInputs);
    assert(window.checked == Window::DrawsPerFrame && window.limited);
    window.present(&device, &swap, 54025);
    sample();
    assert(window.groups[0].matches == 1);
    // Restarting an active capture must not merge the two scenes.
    window.request(SKYRIM_WATER_PROBE_SUBMERGED, 54030);
    sample();
    assert(window.checked == 0 && window.phase == Phase::Waiting);

    if (argc > 1) {
        skyrim_water_probe_request(SKYRIM_WATER_PROBE_SUBMERGED);
        std::this_thread::sleep_for(std::chrono::milliseconds(4050));
        present(&device, &swap);
        draw(&device, &cached, water.data(), water.size(), readInputs);
        for (unsigned i = 0; i < Window::Frames; ++i) present(&device, &swap);
        assert(phase.load() == Phase::Idle);
    }
}
