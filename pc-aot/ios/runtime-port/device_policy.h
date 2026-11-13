/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host execution information only. No graphics settings or guest ABI fields. */
typedef struct SkyrimDeviceCaps {
    uint32_t logical_cpus, active_cpus, display_max_fps, apple_gpu_family;
    uint32_t gpu_present, unified_memory, bc_textures, reserved;
    uint64_t physical_memory, max_buffer_length;
} SkyrimDeviceCaps;
typedef struct SkyrimDeviceState {
    uint32_t active_cpus, thermal, low_power, foreground;
} SkyrimDeviceState;
enum { SKYRIM_WORK_COMPILER = 1, SKYRIM_WORK_CACHE_WRITER = 2 };
enum { SKYRIM_QOS_UTILITY = 1, SKYRIM_QOS_BACKGROUND = 2 };

void skyrim_device_initialize(const SkyrimDeviceCaps *caps, const SkyrimDeviceState *state);
void skyrim_device_update(const SkyrimDeviceState *state);
void skyrim_device_snapshot(SkyrimDeviceCaps *caps, SkyrimDeviceState *state);
void skyrim_device_memory_warning(uint64_t now_ns);
int skyrim_device_memory_pressure(uint64_t now_ns);
unsigned skyrim_device_compiler_cpus(unsigned fallback);
unsigned skyrim_device_work_qos(unsigned role);

/* Advisory pool sizing, never permission to drop a required resource. A zero
 * available value is meaningful when *_known is true. Missing != unlimited. */
uint64_t skyrim_device_pool_size(uint64_t preferred, uint64_t minimum,
    uint64_t driver_budget, uint64_t driver_usage, int driver_known,
    uint64_t app_available, int app_known, int pressure);
void skyrim_device_record_pool(uint64_t preferred, uint64_t chosen);
void skyrim_device_record_reclaimed(uint64_t bytes);
void skyrim_device_take_counters(uint64_t *reclaimed, uint64_t *reduced_pools);

/* Apple implementation, replaced by explicit authored providers in C fixtures. */
void skyrim_device_platform_initialize(void);
void skyrim_device_platform_refresh(void);
void skyrim_device_platform_log(void);
int skyrim_device_platform_headroom(uint64_t *available);
int skyrim_device_platform_work_role(unsigned role);
/* Returns actual nanosleep calls, excluding event-driven run-loop waits. */
unsigned skyrim_device_platform_wait(uint64_t remaining_ns);

#ifdef __cplusplus
}
#endif
