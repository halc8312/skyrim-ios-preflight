/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../event_bridge.h"

enum {
    FIXTURE_VM_FAIL = 1, FIXTURE_SLOT_FAIL = 2, FIXTURE_NO_REVERT = 4,
    FIXTURE_NO_VM = 8, FIXTURE_SCRIPT_RELOAD = 16, FIXTURE_NESTED_REVERT = 32,
    FIXTURE_NO_RETURN = 64, FIXTURE_LATE_REVERT = 128
};
typedef struct {
    SkseGameScope scope;
    SksePairKey saved;
    uint32_t site, options;
    SkseBaseOutcome coordinator_result;
    int ess_fd;
} FixtureAction;
typedef struct {
    SkseGameEvent requested;
    SkseBaseEvidence base;
    uint8_t trace_launch[16];
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    size_t count;
    uint64_t dropped;
} FixtureObservation;
typedef void (*FixtureNotice)(void *, const SkseGameEvent *, uint32_t in_vm);
typedef struct {
    uint32_t size, version;
    int (*act)(const FixtureAction *, FixtureObservation *, FixtureNotice, void *);
} FixtureGuestAPI;
typedef int (*FixtureGuestBind)(FixtureGuestAPI *, const uint8_t launch[16]);
