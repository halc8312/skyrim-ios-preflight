/* SPDX-License-Identifier: GPL-3.0-only */
#include "device_policy.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static SkyrimDeviceCaps capabilities;
static SkyrimDeviceState current;
static int initialized;
static _Atomic uint64_t warning_until, reclaimed_bytes, reduced_pools;

static uint64_t smaller(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t subtract(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

void skyrim_device_initialize(const SkyrimDeviceCaps *caps, const SkyrimDeviceState *state) {
    if (!caps || !state) return;
    pthread_mutex_lock(&state_lock);
    if (!initialized) {
        capabilities = *caps;
        current = *state;
        initialized = 1;
    }
    pthread_mutex_unlock(&state_lock);
}
void skyrim_device_update(const SkyrimDeviceState *state) {
    if (!state) return;
    pthread_mutex_lock(&state_lock);
    current = *state;
    pthread_mutex_unlock(&state_lock);
}
void skyrim_device_snapshot(SkyrimDeviceCaps *caps, SkyrimDeviceState *state) {
    pthread_mutex_lock(&state_lock);
    if (caps) *caps = capabilities;
    if (state) *state = current;
    pthread_mutex_unlock(&state_lock);
}
void skyrim_device_memory_warning(uint64_t now) {
    const uint64_t duration = UINT64_C(15000000000);
    uint64_t until = now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
    uint64_t old = atomic_load_explicit(&warning_until, memory_order_relaxed);
    while (old < until && !atomic_compare_exchange_weak_explicit(&warning_until,
        &old, until, memory_order_release, memory_order_relaxed)) { }
}
int skyrim_device_memory_pressure(uint64_t now) {
    uint64_t until = atomic_load_explicit(&warning_until, memory_order_acquire);
    return until && (!now || now < until);
}
unsigned skyrim_device_compiler_cpus(unsigned fallback) {
    SkyrimDeviceState state;
    skyrim_device_snapshot(0, &state);
    /* This affects optional host compilation only. Never change guest topology. */
    if (state.low_power || state.thermal >= 2) return 1;
    if (state.active_cpus && (!fallback || state.active_cpus < fallback))
        return state.active_cpus;
    return fallback ? fallback : 1;
}
unsigned skyrim_device_work_qos(unsigned role) {
    SkyrimDeviceState state;
    skyrim_device_snapshot(0, &state);
    if (role == SKYRIM_WORK_CACHE_WRITER || state.low_power || state.thermal >= 2 || !state.foreground)
        return SKYRIM_QOS_BACKGROUND;
    return SKYRIM_QOS_UTILITY;
}
uint64_t skyrim_device_pool_size(uint64_t preferred, uint64_t minimum,
    uint64_t budget, uint64_t usage, int driver_known,
    uint64_t available, int app_known, int pressure) {
    if (!preferred || !minimum || minimum >= preferred) return minimum;
    const uint64_t mib = UINT64_C(1048576);
    uint64_t allowance = UINT64_MAX;
    if (driver_known) {
        uint64_t room = subtract(budget, usage);
        /* Leave slack for driver allocations without imposing a new hard cap. */
        allowance = subtract(room, smaller(budget / 16, 64 * mib));
    }
    if (app_known) {
        uint64_t room = subtract(available, smaller(available / 8, 64 * mib));
        allowance = smaller(allowance, room);
    }
    uint64_t floor = smaller(preferred, minimum > 4 * mib ? minimum : 4 * mib);
    uint64_t chosen = preferred;
    while (chosen > floor && (pressure || chosen > allowance)) {
        chosen /= 2;
        if (chosen < floor) chosen = floor;
    }
    return chosen;
}
void skyrim_device_record_pool(uint64_t preferred, uint64_t chosen) {
    if (chosen < preferred) atomic_fetch_add_explicit(&reduced_pools, 1, memory_order_relaxed);
}
void skyrim_device_record_reclaimed(uint64_t bytes) {
    atomic_fetch_add_explicit(&reclaimed_bytes, bytes, memory_order_relaxed);
}
void skyrim_device_take_counters(uint64_t *reclaimed, uint64_t *reduced) {
    *reclaimed = atomic_exchange_explicit(&reclaimed_bytes, 0, memory_order_relaxed);
    *reduced = atomic_exchange_explicit(&reduced_pools, 0, memory_order_relaxed);
}
