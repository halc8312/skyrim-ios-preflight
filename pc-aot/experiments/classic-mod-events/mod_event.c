/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_event.h"

#include <string.h>

void classic_mod_events_reset(ClassicModEvents *events, uint64_t epoch) {
    *events = (ClassicModEvents){.epoch = epoch};
}

static ClassicModEventSlot *event_slot(ClassicModEvents *events, uint64_t epoch, int32_t handle) {
    if (!epoch || events->epoch != epoch || handle <= 0 || handle > events->size) return NULL;
    ClassicModEventSlot *slot = &events->slots[handle - 1];
    return slot->occupied ? slot : NULL;
}

int32_t classic_mod_event_create(ClassicModEvents *events, uint64_t epoch, uint32_t stack,
                                 const char *name, size_t length) {
    if (!epoch || events->epoch != epoch || !name || length > CLASSIC_MOD_EVENT_NAME_MAX ||
        memchr(name, '\0', length)) return 0;
    uint8_t index;
    if (events->free_count) {
        index = events->free_slots[--events->free_count];
    } else {
        if (events->size == CLASSIC_MOD_EVENT_SLOTS) return 0;
        index = events->size++;
    }
    ClassicModEventSlot *slot = &events->slots[index];
    *slot = (ClassicModEventSlot){.owning_stack = stack, .occupied = true};
    memcpy(slot->packet.name, name, length);
    return (int32_t)index + 1;
}

bool classic_mod_event_push_bool(ClassicModEvents *events, uint64_t epoch, int32_t handle, bool value) {
    ClassicModEventSlot *slot = event_slot(events, epoch, handle);
    if (!slot || slot->packet.argc == CLASSIC_MOD_EVENT_BOOL_MAX) return false;
    slot->packet.args[slot->packet.argc++] = value;
    return true;
}

static void drop_slot(ClassicModEvents *events, uint8_t index) {
    events->slots[index] = (ClassicModEventSlot){0};
    events->free_slots[events->free_count++] = index;
}

bool classic_mod_event_send(ClassicModEvents *events, uint64_t epoch, int32_t handle,
                            ClassicModEventEnqueue enqueue, void *context) {
    ClassicModEventSlot *slot = event_slot(events, epoch, handle);
    if (!slot || !enqueue) return false;
    const ClassicModEventPacket packet = slot->packet;
    drop_slot(events, (uint8_t)(handle - 1));
    enqueue(context, &packet);
    return true;
}

bool classic_mod_event_release(ClassicModEvents *events, uint64_t epoch, int32_t handle) {
    if (!event_slot(events, epoch, handle)) return false;
    drop_slot(events, (uint8_t)(handle - 1));
    return true;
}

void classic_mod_events_clean_dropped(ClassicModEvents *events, uint64_t epoch,
                                       ClassicModEventStackLive is_live, void *context) {
    if (!epoch || events->epoch != epoch || !is_live) return;
    for (uint8_t i = 0; i < events->size; ++i) {
        const ClassicModEventSlot *slot = &events->slots[i];
        if (slot->occupied && !is_live(context, slot->owning_stack)) drop_slot(events, i);
    }
}
