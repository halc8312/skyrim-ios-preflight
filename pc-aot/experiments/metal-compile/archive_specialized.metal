// Authored specialization boundary: one function, two distinct constant values.
#include <metal_stdlib>
using namespace metal;

constant uint archive_value [[function_constant(0)]];

kernel void archive_specialized(device uint4* output [[buffer(0)]],
                                uint id [[thread_position_in_grid]]) {
    if (id < 4)
        output[id] = uint4(id + archive_value, (id + 1u) * archive_value,
                           archive_value ^ 0x12345678u, 100u - archive_value);
}
