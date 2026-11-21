/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "cosave_fixture.h"
#include "event_bridge.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { OWNER_UID = 0x368790, RECORD_TYPE = 0x45564153, RECORD_VERSION = 2 };
static const char record_data[] = "authored record";
static char store_path[PATH_MAX];
static SkseEventBridge *bridge;
static SksePendingSave *pending;
static SkseGameEvent requested = {
    .scope = {.launch = {1}, .profile = {2}},
    .saved = {.profile = {2}, .slot = {3}, .event = {4}},
};

void save_composition_open(const char *directory) {
    assert(!bridge && !pending);
    const int length = snprintf(store_path, sizeof store_path, "%s/text-en-voice-en", directory);
    assert(length > 0 && (size_t)length < sizeof store_path);
    SksePairResult detail;
    assert(skse_bridge_open(directory, &requested.scope, &bridge, &detail) == SKSE_BRIDGE_OK);
}

void save_composition_begin(void) {
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    uint64_t dropped;
    const size_t count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &dropped);
    assert(count && !dropped && !pending);
    const SkyrimLifecycleRow *slot = &rows[count - 1];
    assert(slot->site == SkyrimSaveSlot && !slot->returning && !slot->parent);
    requested.ticket = slot->ticket;
    SksePairResult detail;
    assert(skse_bridge_begin_save(bridge, &requested, NULL, &pending, &detail) == SKSE_BRIDGE_OK);
}

void save_composition_record(void) {
    const SksePairRecord record = {RECORD_TYPE, RECORD_VERSION, sizeof record_data, record_data};
    assert(skse_bridge_write_owner(pending, OWNER_UID, &record, 1) == SKSE_PAIR_OK);
}

typedef struct { bool reset, restored; } Restored;

static void reset(void *context) {
    Restored *result = context;
    assert(!result->reset && !result->restored);
    result->reset = true;
}

static int restore(void *context, const SksePairRecord *record) {
    Restored *result = context;
    assert(result->reset && !result->restored);
    assert(record->type == RECORD_TYPE && record->version == RECORD_VERSION && record->length == sizeof record_data);
    assert(!memcmp(record->data, record_data, sizeof record_data));
    result->restored = true;
    return 1;
}

static void check_committed(int fd) {
    SksePairStore *store = NULL;
    assert(skse_pair_open(store_path, &store) == SKSE_PAIR_OK);
    const SksePairGeneration generation = {requested.saved, fd};
    Restored received = {0};
    assert(skse_pair_restore(store, &generation, OWNER_UID, reset, restore, &received) == SKSE_PAIR_OK);
    assert(received.reset && received.restored);
    skse_pair_close(store);
}

void save_composition_finish(int published_fd, bool vm_saved) {
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    SkseEventTrace trace = {.rows = rows};
    trace.count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &trace.dropped);
    memcpy(trace.launch, requested.scope.launch, sizeof trace.launch);
    const SkseBaseEvidence base = {requested, published_fd >= 0 ? SKSE_BASE_SAVED : SKSE_BASE_FAILED, published_fd};
    SksePairResult detail;
    const SkseBridgeResult result = skse_bridge_capture_save(pending, &trace, &base, &detail);
    if (published_fd >= 0 && vm_saved) {
        assert(result == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK);
        SkseGameScope changed = requested.scope;
        ++changed.launch[0];
        assert(skse_bridge_publish(pending, &changed, &detail) == SKSE_BRIDGE_SCOPE);
        assert(skse_bridge_publish(pending, &requested.scope, &detail) == SKSE_BRIDGE_OK);
        check_committed(published_fd);
    } else {
        assert(result == SKSE_BRIDGE_REJECTED);
        assert(skse_bridge_publish(pending, &requested.scope, &detail) == SKSE_BRIDGE_STATE);
    }
    skse_bridge_drop(pending);
    pending = NULL;
}

void save_composition_close(void) {
    assert(bridge && !pending);
    skse_bridge_close(bridge);
    bridge = NULL;
}
