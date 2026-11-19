/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "boundary_fixture.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static uint8_t observer_launch[16];
static int bound;

static void guest_revert(uint32_t flags) {
    if (!(flags & FIXTURE_NO_REVERT)) {
        uint32_t site = flags & FIXTURE_SCRIPT_RELOAD ? SkyrimRevertForReload : SkyrimRevertForLoad;
        SKYRIM_LIFECYCLE_CALL(site, (void)0, SkyrimNoResult);
    }
}

static int guest_vm(const FixtureAction *action, const SkseGameEvent *event, FixtureNotice notice, void *host) {
    if (notice) notice(host, event, 1);
    if (action->site == SkyrimLoadSlot && (action->options & FIXTURE_NESTED_REVERT))
        guest_revert(action->options);
    unsigned char byte;
    return !(action->options & FIXTURE_VM_FAIL) && pread(action->ess_fd, &byte, 1, 0) == 1;
}

static void guest_save(const FixtureAction *action, const SkseGameEvent *event, FixtureNotice notice, void *host) {
    int ok;
    if (!(action->options & FIXTURE_NO_VM))
        SKYRIM_LIFECYCLE_CALL(SkyrimSaveVM, ok = guest_vm(action, event, notice, host), ok ? SkyrimReturnedTrue : SkyrimReturnedFalse);
    /* The observed outer save has a void return. */
}

static int guest_load(const FixtureAction *action, const SkseGameEvent *event, FixtureNotice notice, void *host) {
    if (!(action->options & (FIXTURE_NESTED_REVERT | FIXTURE_LATE_REVERT))) guest_revert(action->options);
    int ok = 0;
    if (!(action->options & FIXTURE_NO_VM))
        SKYRIM_LIFECYCLE_CALL(SkyrimLoadVM, ok = guest_vm(action, event, notice, host), ok ? SkyrimReturnedTrue : SkyrimReturnedFalse);
    if (action->options & FIXTURE_LATE_REVERT) guest_revert(action->options);
    return !(action->options & FIXTURE_SLOT_FAIL);
}

static int guest_act(const FixtureAction *action, FixtureObservation *out, FixtureNotice notice, void *host) {
    if (!action || !out || (action->site != SkyrimSaveSlot && action->site != SkyrimLoadSlot) ||
        memcmp(action->scope.launch, observer_launch, sizeof observer_launch)) return 0;
    memset(out, 0, sizeof *out);
    out->base.ess_fd = -1;
    SkyrimLifecycleCall call = skyrim_lifecycle_begin(action->site);
    out->requested = (SkseGameEvent){action->scope, action->saved, call.ticket};
    if (notice) notice(host, &out->requested, 0);
    uint32_t outcome = SkyrimNoResult;
    if (action->site == SkyrimSaveSlot) guest_save(action, &out->requested, notice, host);
    else outcome = guest_load(action, &out->requested, notice, host) ? SkyrimReturnedTrue : SkyrimReturnedFalse;
    if (!(action->options & FIXTURE_NO_RETURN)) skyrim_lifecycle_end(call, outcome);
    out->count = skyrim_lifecycle_snapshot(out->rows, SkyrimLifecycleCapacity, &out->dropped);
    memcpy(out->trace_launch, observer_launch, sizeof out->trace_launch);
    out->base.event = out->requested;
    /* This independent fixture coordinator supplies a fact absent from the
     * real trace. It can also explicitly withhold or reject it. */
    out->base.outcome = action->coordinator_result;
    if (out->base.outcome == SKSE_BASE_SAVED || out->base.outcome == SKSE_BASE_LOADED)
        out->base.ess_fd = fcntl(action->ess_fd, F_DUPFD_CLOEXEC, 0);
    return 1;
}

int fixture_guest_bind(FixtureGuestAPI *api, const uint8_t launch[16]) {
    if (!api || !launch || bound || api->size != sizeof *api || api->version != 1) return 0;
    memcpy(observer_launch, launch, sizeof observer_launch);
    bound = 1;
    api->act = guest_act;
    return 1;
}
