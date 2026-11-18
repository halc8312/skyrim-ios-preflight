// Authored binary-archive fixture. No game data or function constants.
#include <metal_stdlib>
using namespace metal;

kernel void archive_recorded(device uint4* output [[buffer(0)]],
                             uint id [[thread_position_in_grid]]) {
    if (id < 4)
        output[id] = uint4(id + 3u, (id + 1u) * 7u, 10u, 0x12345678u);
}

kernel void archive_unrecorded(device uint4* output [[buffer(0)]],
                               uint id [[thread_position_in_grid]]) {
    if (id < 4)
        output[id] = uint4(id * 5u + 11u, (id ^ 3u) + 17u, 29u - id, 0x87654321u);
}
