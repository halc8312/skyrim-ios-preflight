/* SPDX-License-Identifier: GPL-3.0-only */
#include "boundary_fixture.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef struct { void *image; FixtureGuestAPI guest; } FixtureHost;

FixtureHost *fixture_host_open(const char *path, uint32_t version, const uint8_t launch[16]) {
    FixtureHost *host = calloc(1, sizeof *host);
    if (!host) return NULL;
    host->image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (host->image) {
        void *symbol = dlsym(host->image, "fixture_guest_bind");
        FixtureGuestBind bind;
        _Static_assert(sizeof bind == sizeof symbol, "POSIX function pointer size");
        memcpy(&bind, &symbol, sizeof bind);
        host->guest = (FixtureGuestAPI){sizeof(FixtureGuestAPI), version, NULL};
        if (bind && bind(&host->guest, launch) && host->guest.act) return host;
        dlclose(host->image);
    }
    free(host);
    return NULL;
}

void fixture_host_close(FixtureHost *host) {
    if (host) { dlclose(host->image); free(host); }
}

int fixture_host_act(FixtureHost *host, const FixtureAction *action, FixtureObservation *observation) {
    return host && host->guest.act(action, observation, NULL, NULL);
}

int fixture_host_observe_with_notice(FixtureHost *host, const FixtureAction *action, FixtureObservation *out,
                                    FixtureNotice notice, void *observer) {
    return host && host->guest.act(action, out, notice, observer);
}

typedef struct {
    SkseEventBridge *bridge;
    const SksePairGeneration *base;
    uint32_t uid;
    const SksePairRecord *rows;
    size_t count;
    SksePendingSave *pending;
    SksePairResult detail;
    SkseBridgeResult result;
} SaveCallback;

static void stage_private_records(void *context, const SkseGameEvent *event, uint32_t in_vm) {
    SaveCallback *save = context;
    if (!in_vm) {
        save->result = skse_bridge_begin_save(save->bridge, event, save->base, &save->pending, &save->detail);
    } else if (save->result == SKSE_BRIDGE_OK && save->uid) {
        save->detail = skse_bridge_write_owner(save->pending, save->uid, save->rows, save->count);
        if (save->detail != SKSE_PAIR_OK) save->result = SKSE_BRIDGE_PAIR;
    }
}

/* Host -> separately loaded guest -> guest's VM window -> host's staged
 * records. The guest still completes normally if the separate store fails. */
SkseBridgeResult fixture_host_stage(FixtureHost *host, SkseEventBridge *bridge, const FixtureAction *action,
                                   const SksePairGeneration *base, uint32_t uid, const SksePairRecord *rows,
                                   size_t count, SksePendingSave **pending, FixtureObservation *out,
                                   SksePairResult *detail) {
    if (!pending || !out || !host || !action || action->site != SkyrimSaveSlot) return SKSE_BRIDGE_ARGUMENT;
    *pending = NULL;
    SaveCallback context = {bridge, base, uid, rows, count, NULL, SKSE_PAIR_OK, SKSE_BRIDGE_OK};
    if (!host->guest.act(action, out, stage_private_records, &context)) return SKSE_BRIDGE_ARGUMENT;
    if (detail) *detail = context.detail;
    if (context.result == SKSE_BRIDGE_OK) *pending = context.pending;
    else skse_bridge_drop(context.pending);
    return context.result;
}
