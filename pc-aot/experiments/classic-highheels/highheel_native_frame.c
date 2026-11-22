/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_native_frame.h"

enum {
    ORDINARY_RUN_RETURN = 0xc46e61, RUN_WORDS = 6, FRAME_WORDS = 3, CALLER_WORDS = 7,
    RUN_RESULT = 0x2c, RUN_BASE = 0x38,
    STACK_FRAME = 0x30, STACK_ID = 0x44, CALLER_SELF = 0x14
};

static bool read_words(HighHeelEffectRead read, void *context, uint32_t object, unsigned offset,
                       unsigned count, uint32_t *out) {
    const uint64_t address = (uint64_t)object + offset;
    uint8_t bytes[CALLER_WORDS * 4];
    if (!object || (object & 3u) || count > CALLER_WORDS ||
        address + count * 4u > UINT64_C(0x100000000) ||
        !read(context, (uint32_t)address, bytes, count * 4u)) return false;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *word = bytes + i * 4u;
        out[i] = word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
    }
    return true;
}

bool highheel_visit_native_self(const HighHeelNativeBindings *bindings, uint64_t current_epoch,
                                const struct CPU *cpu, const struct CPU *current_cpu,
                                uint32_t native_object, uint32_t sp, HighHeelEffectRead read,
                                void *read_context, HighHeelVisitSelf visit, void *visit_context) {
    if (!bindings || !cpu || cpu != current_cpu || !read || !visit || !current_epoch ||
        current_epoch != bindings->epoch || !bindings->registry || !bindings->effect_class ||
        (bindings->effect_class & 3u) || !native_object || (native_object & 3u) ||
        bindings->keep_update == bindings->stop_update ||
        (native_object != bindings->keep_update && native_object != bindings->stop_update)) return false;

    uint32_t run[RUN_WORDS], base_kind, frame[FRAME_WORDS], current_frame, stack_id, caller[CALLER_WORDS];
    if (sp > UINT32_MAX - RUN_BASE - 7u ||
        !read_words(read, read_context, sp, 0, RUN_WORDS, run) || run[0] != ORDINARY_RUN_RETURN ||
        run[1] != sp + RUN_BASE || run[2] != bindings->registry || run[4] != sp + RUN_RESULT ||
        !read_words(read, read_context, run[1], 0, 1, &base_kind) || base_kind ||
        !read_words(read, read_context, run[5], 0, FRAME_WORDS, frame) || frame[2] != native_object ||
        !read_words(read, read_context, frame[0], STACK_FRAME, 1, &current_frame) || current_frame != run[5] ||
        !read_words(read, read_context, frame[0], STACK_ID, 1, &stack_id) || stack_id != run[3] ||
        frame[1] == run[5] || !read_words(read, read_context, frame[1], 0, CALLER_WORDS, caller) ||
        caller[0] != frame[0] || !caller[2] || caller[5] != bindings->effect_class ||
        !caller[6] || (caller[6] & 3u)) return false;
    const bool caller_allowed = native_object == bindings->keep_update
        ? caller[2] == bindings->move_up || caller[2] == bindings->on_update
        : caller[2] == bindings->move_down;
    return caller_allowed && visit(visit_context, frame[1] + CALLER_SELF);
}
