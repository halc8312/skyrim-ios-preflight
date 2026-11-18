#pragma once
// Portable CPU oracle for authored inputs only. No Metal, guest or game data.
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace SkyrimPhoneWorkloads {
inline constexpr unsigned FamilyCount = 3;
inline constexpr unsigned VariantCount = 4;
inline constexpr unsigned CaseCount = FamilyCount * VariantCount;
inline constexpr unsigned RouteCount = 4;
inline constexpr unsigned RepeatCount = 4;
inline constexpr unsigned InputCount = 128;
inline constexpr unsigned LaneCount = 16;
inline constexpr unsigned OutputCount = LaneCount * 4;
inline constexpr uint32_t RenderTag = 0x13579bdfu;
inline constexpr uint32_t ComputeTag = 0x2468ace0u;
using Input = std::array<uint32_t, InputCount>;
using Output = std::array<uint32_t, OutputCount>;

inline const char* caseID(unsigned index) {
    constexpr const char* names[] = {
        "branch_mix_v1", "branch_mix_v2", "branch_mix_v3", "branch_mix_v4",
        "table_permute_v1", "table_permute_v2", "table_permute_v3", "table_permute_v4",
        "coupled_rounds_v1", "coupled_rounds_v2", "coupled_rounds_v3", "coupled_rounds_v4"
    };
    if (index >= CaseCount) throw std::out_of_range("authored case index");
    return names[index];
}

inline unsigned familyIndex(unsigned index) {
    (void)caseID(index);
    return index / VariantCount;
}

inline unsigned variantIndex(unsigned index) {
    (void)caseID(index);
    return index % VariantCount;
}

inline const char* familyID(unsigned index) {
    constexpr const char* names[] = {"branch_mix", "table_permute", "coupled_rounds"};
    return names[familyIndex(index)];
}

inline unsigned rounds(unsigned index) {
    constexpr unsigned values[] = {12, 20, 28};
    return values[familyIndex(index)];
}

inline uint32_t scale(unsigned index) {
    return 7u + 2u * familyIndex(index);
}

inline uint32_t variantSalt(unsigned index) {
    return 0x6a09e667u ^ (variantIndex(index) * 0xbb67ae85u);
}

inline unsigned routeIndex(unsigned iteration, unsigned slot, unsigned index) {
    if (iteration >= RepeatCount || slot >= RouteCount)
        throw std::out_of_range("balanced authored route index");
    return (iteration + slot + familyIndex(index) + variantIndex(index)) % RouteCount;
}

inline uint32_t rotate(uint32_t x, unsigned amount) {
    amount &= 31u;
    return (x << amount) | (x >> ((32u - amount) & 31u));
}

inline uint32_t mix(uint32_t x, uint32_t salt) {
    x ^= rotate(x + salt, 7);
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

inline Input makeInput(unsigned index) {
    Input result{};
    uint32_t state = 0x243f6a88u ^ (familyIndex(index) * 0x9e3779b9u)
        ^ (variantIndex(index) * 0xd1b54a35u);
    for (unsigned i = 0; i < InputCount; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        result[i] = state + i * 0x85ebca6bu;
    }
    return result;
}

// All arithmetic wraps at 32 bits; shifts never exceed 31. The shader uses
// independent scalar Metal code, and both stages must match every oracle word.
inline std::array<uint32_t, 4> lane(unsigned index, const Input& input,
                                    unsigned id, uint32_t stage) {
    const unsigned count = rounds(index);
    if (id >= LaneCount) throw std::out_of_range("authored lane index");
    const uint32_t factor = scale(index);
    const unsigned family = familyIndex(index);
    const unsigned variant = variantIndex(index);
    const uint32_t salt = variantSalt(index);
    uint32_t x = input[(id * 3u + family * 5u) & 127u] ^ (0x9e3779b9u + stage) ^ salt;
    uint32_t y = input[(id * 5u + 17u) & 127u] + id * 0x85ebca6bu;
    uint32_t z = input[(id * 7u + 31u) & 127u] ^ (factor * 0xc2b2ae35u);
    uint32_t w = input[(id * 11u + 47u) & 127u] + 0x27d4eb2fu + (family << 16) + variant * 0x10001u;
    for (unsigned r = 0; r < count; ++r) {
        const uint32_t t = input[(id * 13u + r * 7u + family * 19u) & 127u];
        switch ((x ^ t ^ r) & 3u) {
            case 0: x = rotate(x + t + (z ^ 0xa511e9b3u), 5); y ^= x + w; break;
            case 1: x = (x ^ t) * 0x9e3779b1u; z += rotate(y, 11); break;
            case 2: y = rotate(y + t, 13); x ^= z + w; break;
            default: z = rotate(z ^ t, 17); w += x ^ y; break;
        }
        if (family == 0) {
            x += rotate(y ^ t, (r % 13u) + 1u);
            y = (y ^ z) * 0x7feb352du;
            z ^= rotate(w + x, 9);
            w += x ^ (t * factor);
        } else if (family == 1) {
            const uint32_t a = input[(x ^ r) & 127u];
            const uint32_t b = input[(y + id) & 127u];
            x = rotate((x & a) | (z & ~a), 7);
            y += (b ^ w) * 0x846ca68bu;
            z = rotate(z + b, 3) ^ x;
            w = (w ^ a) + rotate(y, 17);
        } else {
            const uint32_t u = mix(x + t, r + 1u);
            const uint32_t v = mix(y ^ u, stage + r);
            x = mix(z + v, factor);
            y = mix(w ^ x, t);
            z += rotate(u ^ y, 11);
            w ^= rotate(v + z, 19);
        }
        x ^= r * (0x9e3779b9u + variant * 2u);
        y += factor ^ salt;
        z ^= rotate(x + w, 5u + variant);
        w += t ^ (r + stage + salt);
    }
    return {x ^ stage, y + id, z ^ factor, w + family};
}

inline Output oracle(unsigned index, const Input& input, uint32_t stage) {
    Output result{};
    for (unsigned id = 0; id < LaneCount; ++id) {
        const auto value = lane(index, input, id, stage);
        for (unsigned j = 0; j < 4; ++j) result[id * 4 + j] = value[j];
    }
    return result;
}

template<size_t N>
inline std::array<uint8_t, N * 4> littleEndianBytes(const std::array<uint32_t, N>& words) {
    std::array<uint8_t, N * 4> bytes{};
    for (size_t i = 0; i < N; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = uint8_t(words[i] >> (j * 8));
    return bytes;
}
} // namespace SkyrimPhoneWorkloads
