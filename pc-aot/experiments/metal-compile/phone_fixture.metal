// Bounded, authored compiler workloads. No Skyrim shaders or game data.
#include <metal_stdlib>
using namespace metal;

#if !defined(PHONE_CASE) || !defined(PHONE_VARIANT) || !defined(PHONE_ROUNDS)
#error Recorded PHONE_CASE, PHONE_VARIANT and PHONE_ROUNDS options are required
#endif
#if (PHONE_CASE == 0 && PHONE_ROUNDS != 12) || (PHONE_CASE == 1 && PHONE_ROUNDS != 20) || (PHONE_CASE == 2 && PHONE_ROUNDS != 28) || PHONE_CASE < 0 || PHONE_CASE > 2
#error Unknown authored workload configuration
#endif
#if PHONE_VARIANT < 0 || PHONE_VARIANT > 3
#error Unknown authored semantic variant
#endif

constant uint phone_scale [[function_constant(0)]];

static inline uint phone_rotate(uint x, uint amount) {
    amount &= 31u;
    return (x << amount) | (x >> ((32u - amount) & 31u));
}

#if PHONE_CASE == 2
static inline uint phone_mix(uint x, uint salt) {
    x ^= phone_rotate(x + salt, 7);
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
#endif

static inline uint4 phone_workload(device const uint* input, uint id, uint stage) {
    // Each predeclared variant changes real integer expressions, not labels or
    // comments. Rounds and the operation structure remain fixed per family.
    const uint salt = 0x6a09e667u ^ (uint(PHONE_VARIANT) * 0xbb67ae85u);
    uint x = input[(id * 3u + uint(PHONE_CASE) * 5u) & 127u] ^ (0x9e3779b9u + stage) ^ salt;
    uint y = input[(id * 5u + 17u) & 127u] + id * 0x85ebca6bu;
    uint z = input[(id * 7u + 31u) & 127u] ^ (phone_scale * 0xc2b2ae35u);
    uint w = input[(id * 11u + 47u) & 127u] + 0x27d4eb2fu + (uint(PHONE_CASE) << 16) + uint(PHONE_VARIANT) * 0x10001u;
    // The bounded compile-time loop is unrolled, while all branches and table
    // accesses depend on runtime buffer values. This is not a large GPU load:
    // only 16 fragment invocations and 16 compute lanes are submitted per sample.
    #pragma unroll
    for (uint r = 0; r < uint(PHONE_ROUNDS); ++r) {
        const uint t = input[(id * 13u + r * 7u + uint(PHONE_CASE) * 19u) & 127u];
        switch ((x ^ t ^ r) & 3u) {
            case 0: x = phone_rotate(x + t + (z ^ 0xa511e9b3u), 5); y ^= x + w; break;
            case 1: x = (x ^ t) * 0x9e3779b1u; z += phone_rotate(y, 11); break;
            case 2: y = phone_rotate(y + t, 13); x ^= z + w; break;
            default: z = phone_rotate(z ^ t, 17); w += x ^ y; break;
        }
#if PHONE_CASE == 0
        x += phone_rotate(y ^ t, (r % 13u) + 1u);
        y = (y ^ z) * 0x7feb352du;
        z ^= phone_rotate(w + x, 9);
        w += x ^ (t * phone_scale);
#elif PHONE_CASE == 1
        const uint a = input[(x ^ r) & 127u];
        const uint b = input[(y + id) & 127u];
        x = phone_rotate((x & a) | (z & ~a), 7);
        y += (b ^ w) * 0x846ca68bu;
        z = phone_rotate(z + b, 3) ^ x;
        w = (w ^ a) + phone_rotate(y, 17);
#else
        const uint u = phone_mix(x + t, r + 1u);
        const uint v = phone_mix(y ^ u, stage + r);
        x = phone_mix(z + v, phone_scale);
        y = phone_mix(w ^ x, t);
        z += phone_rotate(u ^ y, 11);
        w ^= phone_rotate(v + z, 19);
#endif
        x ^= r * (0x9e3779b9u + uint(PHONE_VARIANT) * 2u);
        y += phone_scale ^ salt;
        z ^= phone_rotate(x + w, 5u + uint(PHONE_VARIANT));
        w += t ^ (r + stage + salt);
    }
    return uint4(x ^ stage, y + id, z ^ phone_scale, w + uint(PHONE_CASE));
}

struct PhoneVertex { float4 position [[position]]; };

vertex PhoneVertex phone_vertex(uint id [[vertex_id]]) {
    const float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)
    };
    PhoneVertex result;
    result.position = float4(positions[id], 0.0f, 1.0f);
    return result;
}

fragment uint4 phone_fragment(PhoneVertex interpolated [[stage_in]],
                              device const uint* input [[buffer(0)]]) {
    const uint id = uint(interpolated.position.y) * 4u + uint(interpolated.position.x);
    return phone_workload(input, id, 0x13579bdfu);
}

kernel void phone_kernel(device const uint* input [[buffer(0)]],
                          device uint4* output [[buffer(1)]],
                          uint id [[thread_position_in_grid]]) {
    if (id < 16u) output[id] = phone_workload(input, id, 0x2468ace0u);
}
