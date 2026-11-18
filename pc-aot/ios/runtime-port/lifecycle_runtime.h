/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Stable, non-private diagnostic reasons; Vulkan drain errors keep their own
 * original signed result instead of being remapped into these values. */
enum {
    SKYRIM_LIFECYCLE_INVALID_GENERATION = -1001,
    SKYRIM_LIFECYCLE_INIT_OFF_MAIN = -1002,
    SKYRIM_LIFECYCLE_NOTIFICATION_FAILED = -1003,
    SKYRIM_LIFECYCLE_WORKER_FAILED = -1004,
    SKYRIM_LIFECYCLE_UNINITIALIZED = -1005,
    SKYRIM_LIFECYCLE_DEVICE_FAILED = -1006,
    SKYRIM_LIFECYCLE_UNBALANCED_CALL = -1007,
    SKYRIM_LIFECYCLE_INVALID_COUNT = -1008,
    SKYRIM_LIFECYCLE_UNADMITTED_DEVICE = -1009,
    SKYRIM_LIFECYCLE_DUPLICATE_DEVICE = -1010,
    SKYRIM_LIFECYCLE_REGISTRY_FULL = -1011,
    SKYRIM_LIFECYCLE_UNKNOWN_DEVICE = -1012
};
/* Initialize on UIKit main before the first guest D3D factory call. */
void skyrim_lifecycle_initialize(void);
void skyrim_lifecycle_request(int active);
void skyrim_lifecycle_did_background(void);
/* Call only outside host D3D locks. Main parks in a live CFRunLoop; workers
 * wait on a condition variable. No return value substitutes for a D3D call. */
void skyrim_lifecycle_host_enter(void);
void skyrim_lifecycle_host_leave(void);
void skyrim_lifecycle_main_checkpoint(void);
/* The audio engine, outside its guest callbacks/mixer lock, uses this to pause
 * the existing device and wait without changing the game's StartEngine state. */
int skyrim_lifecycle_audio_should_pause(void);
void skyrim_lifecycle_audio_wait(void);
void skyrim_lifecycle_audio_event(int paused);

/* Each callback is invoked on the single lifecycle worker, with host admission
 * closed and zero admitted callers. phase 0 flushes every producer, phase 1
 * joins its CS stream, phase 2 drains submitted GPU work. Native submit remains
 * open throughout. Callback lifetime ends only after unregister returns. */
typedef int32_t (*SkyrimLifecycleDrain)(void *device, unsigned phase);
void skyrim_lifecycle_register_device(void *device, SkyrimLifecycleDrain drain);
void skyrim_lifecycle_unregister_device(void *device);
#ifdef __cplusplus
}
/* Outer guest-to-host entries only: nested helpers already own admission and
 * must never park an admitted producer behind the closed boundary. */
class SkyrimLifecycleCall {
public:
    SkyrimLifecycleCall() { skyrim_lifecycle_host_enter(); }
    ~SkyrimLifecycleCall() { skyrim_lifecycle_host_leave(); }
    SkyrimLifecycleCall(const SkyrimLifecycleCall&) = delete;
    SkyrimLifecycleCall& operator=(const SkyrimLifecycleCall&) = delete;
};
#endif
