#include "ConstantReplayKernel.h"
kernel void owned_constant_replay_v1(const device ohcp::Word *records [[buffer(0)]],
                                    const device ohcp::Vec4 *values [[buffer(1)]],
                                    device ohcp::Vec4 *blocks [[buffer(2)]],
                                    uint3 group [[threadgroup_position_in_grid]],
                                    uint lane [[thread_index_in_threadgroup]]) {
    ohcp::replay_lane(records, values, blocks, group.x, lane);
}
