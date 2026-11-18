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
inline uint32_t salt(unsigned profile, unsigned bank, unsigned role) {
    if (profile >= 2 || bank >= 4 || role >= 3)
        throw std::out_of_range("authored scheduling index out of range");
    return 0x31415927u ^ ((profile * 4u + bank + 1u) * 0x9e3779b9u) ^ (role * 0x85ebca6bu);
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
struct Plan { unsigned profile; bool metal4, contended; };
inline Plan plan(unsigned bank, unsigned profileSlot, unsigned apiSlot, unsigned modeSlot, bool metal4First) {
    if (bank >= 4 || profileSlot >= 2 || apiSlot >= 2 || modeSlot >= 2)
        throw std::out_of_range("authored scheduling plan out of range");
    return { (bank + profileSlot) % 2,
             bool((bank & 1u) ^ apiSlot ^ unsigned(metal4First)),
             bool(((bank >> 1u) & 1u) ^ modeSlot) };
}
} // namespace SkyrimContentionWorkload
#endif
