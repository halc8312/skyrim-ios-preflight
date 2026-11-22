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

SkyrimFuzDecision skyrim_fuz_probe_original(const SkyrimFuzPolicy *policy, const char *wav_path) {
    if (!policy || !policy->lookup || !wav_path) return SKYRIM_FUZ_UNSUPPORTED_INPUT;

    char candidate[SKYRIM_FUZ_VOICE_PATH_CAPACITY];
    size_t length = 0;
    while (length < sizeof candidate && wav_path[length]) ++length;
    // The LE hook requires a 17-byte path including the five-byte Data prefix.
    if (length < 12 || length == sizeof candidate || memcmp(wav_path + length - 4, ".wav", 4))
        return SKYRIM_FUZ_UNSUPPORTED_INPUT;
    memcpy(candidate, wav_path, length + 1);

    const char extensions[][4] = {"wav", "fuz", "xwm"};
    bool indeterminate = false;
    for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; ++i) {
        memcpy(candidate + length - 3, extensions[i], 3);
        SkyrimFuzAvailability found = policy->lookup(policy->context, candidate);
        if (found == SKYRIM_FUZ_PRESENT) return SKYRIM_FUZ_KEEP_VOICE;
        if (found != SKYRIM_FUZ_ABSENT) indeterminate = true;
    }
    return indeterminate ? SKYRIM_FUZ_RESOURCE_FAILURE : SKYRIM_FUZ_NEEDS_STOCK;
}

SkyrimFuzVoice skyrim_fuz_choose_stock(const SkyrimFuzPolicy *policy, const char *response,
                                       bool topic_force_subtitle) {
    SkyrimFuzVoice result = {
        .decision = SKYRIM_FUZ_UNSUPPORTED_INPUT, .force_subtitle = topic_force_subtitle
    };
    if (!policy || !policy->lookup || !response) return result;
    const size_t length = strlen(response);
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
    if (found == SKYRIM_FUZ_ABSENT && stock && policy->allow_packed_stock) {
        char candidate[SKYRIM_FUZ_VOICE_PATH_CAPACITY];
        size_t stock_length = 0;
        while (stock_length < sizeof candidate && stock[stock_length]) ++stock_length;
        if (stock_length >= 4 && stock_length < sizeof candidate && !memcmp(stock + stock_length - 4, ".xwm", 4)) {
            // LE 6.1 ships FUZE containers, but assigns the .xwm name to the game.
            memcpy(candidate, stock, stock_length - 3);
            memcpy(candidate + stock_length - 3, "fuz", sizeof "fuz");
            found = policy->lookup(policy->context, candidate);
        }
    }
    if (found != SKYRIM_FUZ_PRESENT) {
        result.decision = found == SKYRIM_FUZ_ABSENT ? SKYRIM_FUZ_STOCK_UNAVAILABLE : SKYRIM_FUZ_RESOURCE_FAILURE;
        return result;
    }
    return (SkyrimFuzVoice) {
        .decision = SKYRIM_FUZ_USE_STOCK, .stock_path = stock, .seconds = seconds,
        .force_subtitle = topic_force_subtitle || automatic_subtitle
    };
}

SkyrimFuzVoice skyrim_fuz_choose_voice(const SkyrimFuzPolicy *policy, const char *wav_path,
                                       const char *response, bool topic_force_subtitle) {
    const SkyrimFuzDecision decision = response ? skyrim_fuz_probe_original(policy, wav_path)
                                                : SKYRIM_FUZ_UNSUPPORTED_INPUT;
    if (decision == SKYRIM_FUZ_NEEDS_STOCK) return skyrim_fuz_choose_stock(policy, response, topic_force_subtitle);
    return (SkyrimFuzVoice){.decision = decision, .force_subtitle = topic_force_subtitle};
}
