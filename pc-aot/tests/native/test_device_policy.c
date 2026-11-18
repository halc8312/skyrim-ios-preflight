/* Authored device states; no Apple hardware or game data. */
#include "device_policy.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define MIB UINT64_C(1048576)
static void *writer(void *unused) {
    (void)unused;
    for (unsigned i = 1; i <= 20000; ++i) {
        SkyrimDeviceState state = {i, i, i, i};
        skyrim_device_update(&state);
        skyrim_device_record_pool(128*MIB, 4*MIB);
        skyrim_device_record_reclaimed(4*MIB);
    }
    return NULL;
}
int main(void) {
    assert(skyrim_device_compiler_cpus(0) == 1);
    assert(skyrim_device_compiler_cpus(8) == 8);
    SkyrimDeviceCaps caps = {.logical_cpus=6, .active_cpus=6, .display_max_fps=120,
        .apple_gpu_family=10, .gpu_present=1, .physical_memory=12*1024*MIB};
    SkyrimDeviceState state = {6, 0, 0, 1};
    skyrim_device_initialize(&caps, &state);
    SkyrimDeviceCaps changed = {.logical_cpus=2};
    skyrim_device_initialize(&changed, &state);
    skyrim_device_snapshot(&changed, NULL);
    assert(!memcmp(&caps, &changed, sizeof caps));
    assert(skyrim_device_compiler_cpus(32) == 6);
    assert(skyrim_device_compiler_cpus(2) == 2);
    assert(skyrim_device_work_qos(SKYRIM_WORK_COMPILER) == SKYRIM_QOS_UTILITY);
    assert(skyrim_device_work_qos(SKYRIM_WORK_CACHE_WRITER) == SKYRIM_QOS_BACKGROUND);
    state.low_power=1; skyrim_device_update(&state);
    assert(skyrim_device_compiler_cpus(32) == 1);
    assert(skyrim_device_work_qos(SKYRIM_WORK_COMPILER) == SKYRIM_QOS_BACKGROUND);
    state.low_power=0; state.thermal=2; skyrim_device_update(&state);
    assert(skyrim_device_compiler_cpus(6) == 1);
    state.thermal=0; state.foreground=0; skyrim_device_update(&state);
    assert(skyrim_device_work_qos(SKYRIM_WORK_COMPILER) == SKYRIM_QOS_BACKGROUND);
    state.foreground=1; skyrim_device_update(&state);
    assert(skyrim_device_work_qos(SKYRIM_WORK_COMPILER) == SKYRIM_QOS_UTILITY);
    assert(skyrim_device_compiler_cpus(32) == 6);

    assert(!skyrim_device_memory_pressure(100));
    skyrim_device_memory_warning(100);
    assert(skyrim_device_memory_pressure(15000000099ull));
    assert(!skyrim_device_memory_pressure(15000000100ull));
    skyrim_device_memory_warning(200);
    skyrim_device_memory_warning(10); /* an older callback cannot shorten it */
    assert(skyrim_device_memory_pressure(15000000199ull));
    assert(!skyrim_device_memory_pressure(15000000200ull));
    assert(skyrim_device_pool_size(128*MIB, 4096, 0, 0, 0, 0, 0, 0) == 128*MIB);
    assert(skyrim_device_pool_size(128*MIB, 4096, 1024*MIB, 0, 1, 1024*MIB, 1, 0) == 128*MIB);
    assert(skyrim_device_pool_size(128*MIB, 4096, 0, 0, 0, 20*MIB, 1, 0) == 16*MIB);
    assert(skyrim_device_pool_size(128*MIB, 4096, 1024*MIB, 1000*MIB, 1, 1024*MIB, 1, 0) == 4*MIB);
    assert(skyrim_device_pool_size(128*MIB, 4096, 0, 0, 1, 0, 1, 0) == 4*MIB);
    assert(skyrim_device_pool_size(128*MIB, 6*MIB, 0, UINT64_MAX, 1, 0, 1, 0) == 6*MIB);
    assert(skyrim_device_pool_size(128*MIB, 256*MIB, 0, 0, 1, 0, 1, 1) == 256*MIB);
    assert(skyrim_device_pool_size(MIB, 4096, 0, 0, 0, 0, 0, 1) == MIB);
    assert(skyrim_device_pool_size(128*MIB, 4096, 0, 0, 0, UINT64_MAX, 1, 1) == 4*MIB);
    /* Exhaust bounds including non-power-of-two minimum sizes and zero budgets. */
    for (uint64_t minimum=4096; minimum<=160*MIB; minimum+=255*4096) {
        for (uint64_t room=0; room<160*MIB; room+=7*MIB) {
            uint64_t size = skyrim_device_pool_size(128*MIB, minimum,
                256*MIB, 256*MIB-room, 1, room, 1, 0);
            assert(size >= minimum);
            assert(size <= (minimum > 128*MIB ? minimum : 128*MIB));
        }
    }
    state=(SkyrimDeviceState){1,1,1,1}; skyrim_device_update(&state);
    pthread_t threads[4];
    for (unsigned i=0; i<4; ++i) assert(!pthread_create(&threads[i], NULL, writer, NULL));
    for (unsigned i=0; i<20000; ++i) {
        skyrim_device_snapshot(&changed, &state);
        assert(state.active_cpus == state.thermal && state.thermal == state.low_power && state.low_power == state.foreground);
        assert(!memcmp(&caps, &changed, sizeof caps));
    }
    for (unsigned i=0; i<4; ++i) assert(!pthread_join(threads[i], NULL));
    uint64_t reclaimed, reduced;
    skyrim_device_take_counters(&reclaimed, &reduced);
    assert(reclaimed == 80000*4*MIB && reduced == 80000);
    skyrim_device_take_counters(&reclaimed, &reduced);
    assert(!reclaimed && !reduced);
    skyrim_device_memory_warning(UINT64_MAX-1);
    assert(skyrim_device_memory_pressure(UINT64_MAX-1));
    assert(!skyrim_device_memory_pressure(UINT64_MAX));
    puts("policy_boundaries=PASS concurrent_snapshot=PASS quality_settings_absent=1");
}
