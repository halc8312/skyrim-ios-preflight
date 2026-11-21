/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_voice_seam.h"
#include "x86_winfile.h"

#include <string.h>

enum { BSSTRING_BYTES = 8 };
static const char game_prefix[] = "Data\\";

static uint16_t get_u16(const uint8_t bytes[2]) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t get_u32(const uint8_t bytes[4]) {
    return get_u16(bytes) | ((uint32_t)get_u16(bytes + 2) << 16);
}

bool skyrim_fuz_copy_response_text(const SkyrimFuzLiftMemory *memory, uint32_t object,
                                    char response[SKYRIM_FUZ_RESPONSE_TEXT_BYTES]) {
    uint8_t text[BSSTRING_BYTES];
    if (!memory || !memory->read || !response || !object || object > UINT32_MAX - (BSSTRING_BYTES - 1) ||
        !memory->read(memory->context, object, text, sizeof text))
        return false;

    uint32_t address = get_u32(text);
    size_t length = get_u16(text + 4);
    size_t capacity = get_u16(text + 6);
    if (!address) {
        response[0] = '\0';
        return length == 0;
    }
    if (length >= SKYRIM_FUZ_RESPONSE_TEXT_BYTES || length >= capacity || address > UINT32_MAX - length ||
        !memory->read(memory->context, address, response, length + 1))
        return false;
    return !response[length] && !memchr(response, '\0', length);
}

static bool relative_name(const char *path, size_t length, char output[SKSE_WIN_PATH_BYTES]) {
    uint16_t units[SKSE_WIN_PATH_UNITS];
    if (!length || length >= SKSE_WIN_PATH_UNITS || (length >= 2 && path[1] == ':')) return false;
    for (size_t i = 0; i < length; ++i) units[i] = (uint8_t)path[i];
    return skse_win_asset_path(units, length, false, output);
}

static bool original_path(const char frame[SKYRIM_FUZ_LIFT_FRAME_BYTES], char path[SKSE_WIN_PATH_BYTES]) {
    const size_t prefix = sizeof game_prefix - 1;
    const char *end = memchr(frame, '\0', SKYRIM_FUZ_LIFT_FRAME_BYTES);
    if (!end || (size_t)(end - frame) <= prefix || (frame[4] != '\\' && frame[4] != '/')) return false;
    for (size_t i = 0; i < prefix - 1; ++i)
        if (skse_win_ascii_lower((uint8_t)frame[i]) != skse_win_ascii_lower((uint8_t)game_prefix[i])) return false;
    return relative_name(frame + prefix, (size_t)(end - frame) - prefix, path);
}

bool skyrim_fuz_apply_lifted_path_choice(const SkyrimFuzPolicy *policy, const SkyrimFuzLiftMemory *memory,
                                         uint32_t argument_slot, uint32_t response_object, SkyrimFuzLiftChoice *choice) {
    if (choice) *choice = (SkyrimFuzLiftChoice){0};
    if (!policy || !policy->lookup || !memory || !memory->read || !memory->replace ||
        !argument_slot || argument_slot > UINT32_MAX - SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT -
                                         (SKYRIM_FUZ_LIFT_FRAME_BYTES - 1))
        return false;

    uint8_t argument[4];
    if (!memory->read(memory->context, argument_slot, argument, sizeof argument)) return false;
    uint32_t frame = get_u32(argument);
    if (frame != argument_slot + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT) return false;

    char before[SKYRIM_FUZ_LIFT_FRAME_BYTES], path[SKSE_WIN_PATH_BYTES], response[SKYRIM_FUZ_RESPONSE_TEXT_BYTES];
    if (!memory->read(memory->context, frame, before, sizeof before) ||
        !original_path(before, path) || !skyrim_fuz_copy_response_text(memory, response_object, response))
        return false;
    SkyrimFuzVoice voice = skyrim_fuz_choose_voice(policy, path, response, false);
    if (voice.decision != SKYRIM_FUZ_USE_STOCK) return false;

    const size_t prefix = sizeof game_prefix - 1;
    size_t length = 0;
    while (length < sizeof before - prefix && voice.stock_path[length]) ++length;
    if (length == sizeof before - prefix ||
        !relative_name(voice.stock_path, length, path) || strcmp(path, voice.stock_path))
        return false;

    char replacement[sizeof before];
    memcpy(replacement, before, sizeof before);
    memcpy(replacement, game_prefix, prefix);
    for (size_t i = 0; i < length; ++i) replacement[prefix + i] = path[i] == '/' ? '\\' : path[i];
    replacement[prefix + length] = '\0';
    if (!memory->replace(memory->context, frame, before, replacement, sizeof replacement)) return false;
    if (choice) {
        memcpy(choice->assigned_path, replacement, prefix + length + 1);
        choice->force_subtitle = voice.force_subtitle;
    }
    return true;
}

bool skyrim_fuz_apply_lifted_path(const SkyrimFuzPolicy *policy, const SkyrimFuzLiftMemory *memory,
                                  uint32_t argument_slot, uint32_t response_object) {
    return skyrim_fuz_apply_lifted_path_choice(policy, memory, argument_slot, response_object, NULL);
}
