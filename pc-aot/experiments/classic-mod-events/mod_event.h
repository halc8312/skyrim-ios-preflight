/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CLASSIC_MOD_EVENT_SLOTS = 32, CLASSIC_MOD_EVENT_NAME_MAX = 255,
       CLASSIC_MOD_EVENT_BOOL_MAX = 16 };

typedef struct {
    char name[CLASSIC_MOD_EVENT_NAME_MAX + 1];
    uint8_t argc;
    bool args[CLASSIC_MOD_EVENT_BOOL_MAX];
} ClassicModEventPacket;

typedef struct {
    ClassicModEventPacket packet;
    uint32_t owning_stack;
    bool occupied;
} ClassicModEventSlot;

typedef struct {
    uint64_t epoch;
    ClassicModEventSlot slots[CLASSIC_MOD_EVENT_SLOTS];
    uint8_t size, free_count;
    uint8_t free_slots[CLASSIC_MOD_EVENT_SLOTS];
} ClassicModEvents;

/* The accepted VM owner supplies a unique nonzero epoch and serializes use.
 * Queue the borrowed packet values before returning. The stack liveness query
 * must not change the table during a sweep. */
typedef void (*ClassicModEventEnqueue)(void *context, const ClassicModEventPacket *packet);
typedef bool (*ClassicModEventStackLive)(void *context, uint32_t stack);

void classic_mod_events_reset(ClassicModEvents *, uint64_t epoch);
int32_t classic_mod_event_create(ClassicModEvents *, uint64_t epoch, uint32_t stack,
                                 const char *name, size_t length);
bool classic_mod_event_push_bool(ClassicModEvents *, uint64_t epoch, int32_t handle, bool value);
bool classic_mod_event_send(ClassicModEvents *, uint64_t epoch, int32_t handle,
                            ClassicModEventEnqueue enqueue, void *context);
bool classic_mod_event_release(ClassicModEvents *, uint64_t epoch, int32_t handle);
void classic_mod_events_clean_dropped(ClassicModEvents *, uint64_t epoch,
                                       ClassicModEventStackLive is_live, void *context);
