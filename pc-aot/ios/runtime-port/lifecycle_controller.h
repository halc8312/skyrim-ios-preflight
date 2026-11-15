/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Admission is above D3D/D3DX, not at native command-buffer submission.
 * Already admitted work and every downstream producer must remain runnable.
 * All fields are accessed atomically by the implementation. */
typedef struct SkyrimLifecycleController { uint64_t word; } SkyrimLifecycleController;
enum {
    SKYRIM_LIFECYCLE_ACTIVE = 0, SKYRIM_LIFECYCLE_CLOSING = 1,
    SKYRIM_LIFECYCLE_DRAINING = 2, SKYRIM_LIFECYCLE_PAUSED = 3,
    SKYRIM_LIFECYCLE_FAILED = 4
};
typedef struct SkyrimLifecycleSnapshot {
    uint32_t generation, phase, active_calls, requested;
} SkyrimLifecycleSnapshot;

void skyrim_lifecycle_controller_init(SkyrimLifecycleController *state);
/* UI calls never take locks, wait, flush, dispatch guest work, or call Metal. */
int skyrim_lifecycle_controller_request(SkyrimLifecycleController *state, int active);
/* 1 = admitted, 0 = wait outside all host locks, -1 = explicit failed drain.
 * A caller already admitted may nest without another enter/leave pair. */
int skyrim_lifecycle_controller_enter(SkyrimLifecycleController *state);
int skyrim_lifecycle_controller_leave(SkyrimLifecycleController *state);
/* Exactly one worker claims a generation after every admitted call has left.
 * Flush ALL producers before waiting for ANY GPU completion. */
uint32_t skyrim_lifecycle_controller_claim(SkyrimLifecycleController *state);
/* Stale/duplicate completions return 0 and cannot reopen a newer pause.
 * Resume during a drain opens admission only after that drain really ends. */
int skyrim_lifecycle_controller_complete(SkyrimLifecycleController *state,
    uint32_t generation, int32_t result);
SkyrimLifecycleSnapshot skyrim_lifecycle_controller_snapshot(const SkyrimLifecycleController *state);

#ifdef __cplusplus
}
#endif
