/* SPDX-License-Identifier: GPL-3.0-only */
#include "voice_fallback.h"

#include <stddef.h>
#include <string.h>

enum {
    SKYRIM_FUZ_VOICE_PATH_CAPACITY = 260, SKYRIM_FUZ_DEFAULT_WORDS_PER_SECOND = 2,
    SKYRIM_FUZ_SHORT_RESPONSE_SECONDS = 2
};

/* The LE tokenizer uses the literal space as its delimiter. Its final
 * output remains in the same string the author uses in the stock gate. */
static size_t count_words(const char *response, size_t length, size_t *final_length) {
    size_t words = 0, token_length = 0;
    for (size_t i = 0; i < length; ++i) {
        if (response[i] == ' ') continue;
        if (!i || response[i - 1] == ' ') {
            ++words;
            token_length = 0;
        }
        ++token_length;
    }
    *final_length = words ? token_length : length;
    return words;
}

SkyrimFuzVoice skyrim_fuz_choose_voice(const SkyrimFuzPolicy *policy, const char *wav_path,
                                       const char *response, bool topic_force_subtitle) {
    SkyrimFuzVoice result = {
        .decision = SKYRIM_FUZ_UNSUPPORTED_INPUT, .force_subtitle = topic_force_subtitle
    };
    if (!policy || !policy->lookup || !wav_path || !response) return result;

    char candidate[SKYRIM_FUZ_VOICE_PATH_CAPACITY];
    size_t length = 0;
    while (length < sizeof candidate && wav_path[length]) ++length;
    // The LE hook requires a 17-byte path including the five-byte Data prefix.
    if (length < 12 || length == sizeof candidate || memcmp(wav_path + length - 4, ".wav", 4))
        return result;
    memcpy(candidate, wav_path, length + 1);

    const char extensions[][4] = {"wav", "fuz", "xwm"};
    bool indeterminate = false;
    for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; ++i) {
        memcpy(candidate + length - 3, extensions[i], 3);
        SkyrimFuzAvailability found = policy->lookup(policy->context, candidate);
        if (found == SKYRIM_FUZ_PRESENT) {
            result.decision = SKYRIM_FUZ_KEEP_VOICE;
            return result;
        }
        if (found != SKYRIM_FUZ_ABSENT) indeterminate = true;
    }
    if (indeterminate) {
        result.decision = SKYRIM_FUZ_RESOURCE_FAILURE;
        return result;
    }

    length = strlen(response);
    size_t final_length = length;
    bool automatic_subtitle = length > 4 && memcmp(response, "<ID=", 4) != 0;
    unsigned seconds = SKYRIM_FUZ_SHORT_RESPONSE_SECONDS;
    if (automatic_subtitle) {
        unsigned rate = policy->words_per_second > 0 ? (unsigned)policy->words_per_second
                                                     : SKYRIM_FUZ_DEFAULT_WORDS_PER_SECOND;
        size_t whole_seconds = count_words(response, length, &final_length) / rate;
        seconds = whole_seconds < SKYRIM_FUZ_STOCK_COUNT - 1 ? 1 + (unsigned)whole_seconds
                                                            : SKYRIM_FUZ_STOCK_COUNT;
    }

    bool allow_single_space = length == 1 && response[0] == ' ' && !policy->skip_empty_responses;
    if (final_length <= 1 && !allow_single_space) {
        result.decision = SKYRIM_FUZ_SKIP_RESPONSE;
        return result;
    }

    const char *stock = policy->stock_paths[seconds - 1];
    SkyrimFuzAvailability found = stock && *stock ? policy->lookup(policy->context, stock) : SKYRIM_FUZ_ABSENT;
    if (found != SKYRIM_FUZ_PRESENT) {
        result.decision = found == SKYRIM_FUZ_ABSENT ? SKYRIM_FUZ_STOCK_UNAVAILABLE : SKYRIM_FUZ_RESOURCE_FAILURE;
        return result;
    }
    return (SkyrimFuzVoice) {
        .decision = SKYRIM_FUZ_USE_STOCK, .stock_path = stock, .seconds = seconds,
        .force_subtitle = topic_force_subtitle || automatic_subtitle
    };
}
