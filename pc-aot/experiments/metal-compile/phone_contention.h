#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Worker-thread-only, isolated authored experiment. The +0 result is autoreleased
// into the caller's pool. A stop prevents new requests; accepted requests are
// drained without cancellation or an artificial completion deadline. The result
// distinguishes observed host request overlap from compiler-service concurrency.
FOUNDATION_EXPORT NSDictionary* SkyrimMetalRunPhoneContention(
    NSString* sourcePath, BOOL metal4First,
    BOOL (^ _Nullable shouldContinue)(void)) NS_RETURNS_NOT_RETAINED;

NS_ASSUME_NONNULL_END
#endif

#ifdef __cplusplus
// Portable authored integer oracle and the actual scheduling plan. Keeping them
// here lets a native C++ test exercise the same operations without an Apple SDK.
#include <array>
#include <cstdint>
#include <stdexcept>
namespace SkyrimContentionWorkload {
using Input = std::array<uint32_t, 128>;
using Output = std::array<uint32_t, 64>;
inline uint32_t rotate(uint32_t value, unsigned count) {
    return (value << count) | (value >> (32u - count));
}
inline Input makeInput() {
    Input input{};
    uint32_t state = 0x53a9b4d7u;
    for (unsigned i = 0; i < input.size(); ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        input[i] = state + i * 0x9e3779b9u;
    }
    return input;
}
inline uint32_t groupSalt(unsigned group, unsigned role) {
    // Groups 1..8 retain build4's exact identities. Build5 uses disjoint
    // panels 9..40 and 41..72; each group contributes three semantic sources.
    if (group < 1 || group > 72 || role >= 3)
        throw std::out_of_range("authored scheduling index out of range");
    return 0x31415927u ^ (group * 0x9e3779b9u) ^ (role * 0x85ebca6bu);
}
inline Output oracle(const Input& input, uint32_t value) {
    Output result{};
    for (uint32_t id = 0; id < 16; ++id) {
        uint32_t x = input[(id * 3u) & 127u] ^ value;
        uint32_t y = input[(id * 5u + 17u) & 127u] + value;
        uint32_t z = input[(id * 7u + 31u) & 127u] ^ (value * 0x85ebca6bu);
        uint32_t w = input[(id * 11u + 47u) & 127u] + (value ^ 0xc2b2ae35u);
        for (uint32_t r = 0; r < 16; ++r) {
            const uint32_t a = input[(x + r * 7u) & 127u];
            const uint32_t b = input[(y ^ r ^ id) & 127u];
            switch ((a ^ z ^ r) & 3u) {
                case 0: x = rotate(x + a + w, 5); y ^= x; break;
                case 1: y = rotate(y ^ b ^ x, 11); z += y; break;
                case 2: z = rotate(z + a + y, 17); w ^= z; break;
                default: w = rotate(w ^ b ^ z, 23); x += w; break;
            }
            x = (x ^ (z + value + r)) * 0x7feb352du;
            y += rotate(w ^ a, 7);
            z = (z + (y ^ b)) * 0x846ca68bu;
            w ^= rotate(x + value, 13);
        }
        result[id * 4] = x; result[id * 4 + 1] = y;
        result[id * 4 + 2] = z; result[id * 4 + 3] = w;
    }
    return result;
}
inline constexpr unsigned BlockCount = 8;
inline constexpr unsigned Conditions = 4;
inline constexpr unsigned TrialCount = BlockCount * Conditions;
inline unsigned panelFirstGroup(bool metal4First) { return metal4First ? 41u : 9u; }
#ifdef __OBJC__
// These literal-return helpers never return null. Keep the annotation scoped
// away from portable C++ and from the standard-library includes above.
NS_ASSUME_NONNULL_BEGIN
#endif
inline const char* panelNamespace(bool metal4First) {
    return metal4First ? "contention_v2_groups_41_72" : "contention_v2_groups_9_40";
}
inline const char* conditionID(unsigned condition) {
    constexpr const char* names[] = {"legacy_idle", "legacy_contended", "metal4_idle", "metal4_contended"};
    if (condition >= Conditions) throw std::out_of_range("authored condition out of range");
    return names[condition];
}
#ifdef __OBJC__
NS_ASSUME_NONNULL_END
#endif
struct Plan {
    unsigned block, position, condition, lane, sourceGroup;
    bool metal4, contended;
};
inline Plan plan(unsigned block, unsigned position, bool metal4First) {
    if (block >= BlockCount || position >= Conditions)
        throw std::out_of_range("authored scheduling plan out of range");
    constexpr unsigned williams[] = {0, 1, 3, 2};
    const unsigned base = (williams[position] + block) % Conditions;
    // The alternate panel changes only the backend assignment of each planned
    // condition. Idle/contended status and the Williams position stay intact.
    const unsigned condition = base ^ (metal4First ? 2u : 0u);
    const unsigned lane = (condition + block / 2) % Conditions;
    return {block, position, condition, lane,
            panelFirstGroup(metal4First) + block * Conditions + lane,
            condition >= 2, (condition & 1u) != 0};
}
} // namespace SkyrimContentionWorkload
#endif
