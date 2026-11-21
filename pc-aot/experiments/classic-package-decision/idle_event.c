/* SPDX-License-Identifier: GPL-3.0-only */
#include "idle_event.h"
#include "form_view.h"

enum {
    TES_ACTION_DATA_VTABLE = 0x107f494,
    OBJECT_OFFSET = 4,
    EVENT_OFFSET = 0x14,
    REQUEST_BYTES = 0x30,
};

static bool copy_event(const struct CPU *cpu, uint32_t address,
                       char name[CLASSIC_IDLE_EVENT_BYTES]) {
    if (!address || address > UINT32_MAX - (CLASSIC_IDLE_EVENT_BYTES - 1)) return false;

    for (uint32_t i = 0; i < CLASSIC_IDLE_EVENT_BYTES; ++i) {
        uint8_t byte;
        if (!classic_engine_read_data(cpu, address + i, &byte, 1)) return false;
        name[i] = (char)byte;
        if (!byte) return i != 0;
    }
    return false;
}

static void choose_idle(const struct CPU *cpu, uint32_t request) {
    uint8_t fields[EVENT_OFFSET + 4];
    if (!classic_engine_read_data(cpu, request, fields, sizeof(fields)) ||
        classic_read_le32(fields) != TES_ACTION_DATA_VTABLE) {
        return;
    }

    ClassicIdleEvent event = {0};
    const uint32_t object = classic_read_le32(fields + OBJECT_OFFSET);
    if ((!classic_read_form_id(cpu, object, CLASSIC_CHARACTER_FORM, &event.object_id) &&
         !classic_read_form_id(cpu, object, CLASSIC_REFERENCE_FORM, &event.object_id)) ||
        classic_engine_lookup_form(cpu, event.object_id) != object ||
        !copy_event(cpu, classic_read_le32(fields + EVENT_OFFSET), event.event_name)) {
        return;
    }

    const ClassicIdleReplacement replacement = classic_idle_event_choose(event);
    if (!replacement.present || !replacement.idle_id) return;

    const uint32_t idle = classic_engine_lookup_form(cpu, replacement.idle_id);
    uint32_t checked_id;
    if (classic_read_form_id(cpu, idle, CLASSIC_IDLE_FORM, &checked_id) &&
        checked_id == replacement.idle_id) {
        classic_engine_idle_apply(cpu, request, idle);
    }
}

void classic_idle_event_before_test(const struct CPU *cpu, uint32_t request) {
    if (!classic_idle_event_subscribed() || !request || (request & 3u) ||
        request > UINT32_MAX - (REQUEST_BYTES - 1) || !classic_engine_idle_begin(cpu, request)) {
        return;
    }
    choose_idle(cpu, request);
    classic_engine_idle_end(cpu, request);
}
