/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_effect.h"

enum {
    START_PUBLICATION = 0x656ec5, FINISH_PUBLICATION = 0x65710b,
    SCRIPT_EFFECT_TABLE = 0x10c9de4, MAGIC_EFFECT_TYPE = 0x12, ACTOR_TYPE = 0x3e,
    TARGET_ARGUMENT = 4, NUMBER_ARGUMENT = 8, PHASE_ARGUMENT = 12,
    EFFECT_ITEM = 0x34, EFFECT_MAGNITUDE = 0x50, EFFECT_NUMBER = 0x5c,
    ITEM_SETTING = 0xc, FORM_ID = 0xc, FORM_TYPE = 0x12
};

static bool field(HighHeelEffectRead read, void *context, uint32_t object, unsigned offset,
                  unsigned width, uint32_t *out) {
    const uint64_t address = (uint64_t)object + offset;
    if (!object || (object & 3) || address + width > UINT64_C(0x100000000)) return false;
    uint8_t bytes[4] = {0};
    if (!read(context, (uint32_t)address, bytes, width)) return false;
    *out = bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

bool highheel_effect_sample(HighHeelEffectRead read, void *context, uint32_t effect, uint32_t arguments,
                            uint32_t site, uint32_t expected_setting, HighHeelEffectSample *out) {
    const bool starting = site == START_PUBLICATION;
    if (!read || !out || !expected_setting || (!starting && site != FINISH_PUBLICATION)) return false;
    uint32_t type, item, setting, setting_id;
    if (!field(read, context, effect, 0, 4, &type) || type != SCRIPT_EFFECT_TABLE ||
        !field(read, context, effect, EFFECT_ITEM, 4, &item) ||
        !field(read, context, item, ITEM_SETTING, 4, &setting) ||
        !field(read, context, setting, FORM_TYPE, 1, &type) || type != MAGIC_EFFECT_TYPE ||
        !field(read, context, setting, FORM_ID, 4, &setting_id) || setting_id != expected_setting) return false;

    uint32_t phase, number, current_number, target_slot, actor, actor_id, magnitude;
    if (!field(read, context, arguments, PHASE_ARGUMENT, 4, &phase) || phase != (uint32_t)starting ||
        !field(read, context, arguments, NUMBER_ARGUMENT, 4, &number) ||
        !field(read, context, effect, EFFECT_NUMBER, 2, &current_number) || number != current_number ||
        !field(read, context, arguments, TARGET_ARGUMENT, 4, &target_slot) ||
        !field(read, context, target_slot, 0, 4, &actor) ||
        !field(read, context, actor, FORM_TYPE, 1, &type) || type != ACTOR_TYPE ||
        !field(read, context, actor, FORM_ID, 4, &actor_id) || !actor_id ||
        !field(read, context, effect, EFFECT_MAGNITUDE, 4, &magnitude) ||
        (magnitude & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) return false;
    *out = (HighHeelEffectSample){actor_id, setting_id, number, magnitude, starting};
    return true;
}
