/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_menu_seam.h"

#include <string.h>

enum {
    MENU_LAST_SELECTION = 0x1c, SELECTION_CURRENT_NODE = 0x20, SELECTION_TOPIC = 0x18,
    RESPONSE_TITLE = 0x0, RESPONSE_VOICE = 0x10, TOPIC_FLAGS = 0x24,
    TOPIC_FORCE_SUBTITLE = 0x0200, SELECTED_MENU_RETURN = 0x00675caa,
};

static bool read_field(const SkyrimFuzLiftMemory *memory, uint32_t address, uint32_t offset,
                       void *buffer, size_t count) {
    return memory && memory->read && address && count && address <= UINT32_MAX - offset &&
        (uint64_t)address + offset + count - 1 <= UINT32_MAX &&
        memory->read(memory->context, address + offset, buffer, count);
}

static bool read_word(const SkyrimFuzLiftMemory *memory, uint32_t address, uint32_t offset, uint32_t *value) {
    uint8_t bytes[4];
    if (!read_field(memory, address, offset, bytes, sizeof bytes)) return false;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return true;
}

static const SkyrimFuzResponseMark *response_mark(const SkyrimFuzMenuTracker *tracker, uint32_t response) {
    for (size_t i = 0; i < tracker->count; ++i)
        if (tracker->active[i].object == response) return &tracker->active[i];
    return NULL;
}

void skyrim_fuz_menu_forget(SkyrimFuzMenuTracker *tracker, uint32_t response) {
    for (size_t i = 0; i < tracker->count; ++i) {
        if (tracker->active[i].object != response) continue;
        tracker->active[i] = tracker->active[--tracker->count];
        return;
    }
}

bool skyrim_fuz_menu_assigned(SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                              uint32_t response, const SkyrimFuzLiftChoice *choice) {
    skyrim_fuz_menu_forget(tracker, response);
    if (!choice || !choice->force_subtitle || !choice->assigned_path[0] ||
        tracker->count == SKYRIM_FUZ_MAX_MARKED_RESPONSES || tracker->issued == UINT64_MAX) return false;
    const char *end = memchr(choice->assigned_path, 0, sizeof choice->assigned_path);
    if (!end) return false;
    size_t count = (size_t)(end - choice->assigned_path) + 1;
    uint32_t reference;
    char assigned[SKYRIM_FUZ_LIFT_FRAME_BYTES];
    if (!read_word(memory, response, RESPONSE_VOICE, &reference) ||
        !read_field(memory, reference, 0, assigned, count) || memcmp(assigned, choice->assigned_path, count)) return false;
    tracker->active[tracker->count++] = (SkyrimFuzResponseMark){
        .object = response, .voice_reference = reference, .serial = ++tracker->issued,
    };
    return true;
}

uint64_t skyrim_fuz_substitution_serial(const SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                                       uint32_t response, uint32_t voice, uint32_t title) {
    if (!voice || !title) return 0;
    const SkyrimFuzResponseMark *mark = response_mark(tracker, response);
    uint32_t actual_voice, actual_title;
    if (!mark || mark->voice_reference != voice ||
        !read_word(memory, response, RESPONSE_VOICE, &actual_voice) || actual_voice != voice ||
        !read_word(memory, response, RESPONSE_TITLE, &actual_title) || actual_title != title)
        return 0;
    char text[SKYRIM_FUZ_RESPONSE_TEXT_BYTES];
    return skyrim_fuz_copy_response_text(memory, response, text) ? mark->serial : 0;
}

SkyrimFuzMenuScope skyrim_fuz_menu_capture(const SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                                           uint32_t manager, uint32_t node, uint32_t queued_title) {
    SkyrimFuzMenuScope scope = {.manager = manager, .node = node};
    uint32_t current;
    if (!queued_title || !read_word(memory, manager, MENU_LAST_SELECTION, &scope.selection) ||
        !read_word(memory, scope.selection, SELECTION_CURRENT_NODE, &current) || current != node ||
        !read_word(memory, node, 0, &scope.response) ||
        !read_word(memory, scope.response, RESPONSE_TITLE, &scope.title) || scope.title != queued_title ||
        !read_word(memory, scope.response, RESPONSE_VOICE, &scope.voice_reference))
        return (SkyrimFuzMenuScope){0};
    const SkyrimFuzResponseMark *mark = response_mark(tracker, scope.response);
    if (mark && mark->voice_reference == scope.voice_reference) scope.serial = mark->serial;
    return scope;
}

bool skyrim_fuz_menu_force(const SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                           const SkyrimFuzMenuScope *scope, uint32_t address, uint32_t queued_title) {
    if (address != SELECTED_MENU_RETURN || !scope || !scope->response || scope->title != queued_title) return false;
    SkyrimFuzMenuScope current = skyrim_fuz_menu_capture(tracker, memory, scope->manager, scope->node, queued_title);
    if (current.selection != scope->selection || current.response != scope->response ||
        current.voice_reference != scope->voice_reference || current.serial != scope->serial) return false;
    bool force = scope->serial != 0;
    if (!force) {
        uint32_t topic;
        uint8_t flags[2];
        force = read_word(memory, current.selection, SELECTION_TOPIC, &topic) &&
            read_field(memory, topic, TOPIC_FLAGS, flags, sizeof flags) &&
            (((uint32_t)flags[0] | ((uint32_t)flags[1] << 8)) & TOPIC_FORCE_SUBTITLE);
    }
    char title[SKYRIM_FUZ_RESPONSE_TEXT_BYTES];
    return force && skyrim_fuz_copy_response_text(memory, current.response, title);
}
