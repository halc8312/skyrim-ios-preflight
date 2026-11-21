/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_FUZ_VOICE_FALLBACK_H
#define SKYRIM_EXPERIMENT_FUZ_VOICE_FALLBACK_H

#include <stdbool.h>

enum { SKYRIM_FUZ_STOCK_COUNT = 10 };

typedef enum {
    SKYRIM_FUZ_ABSENT, SKYRIM_FUZ_PRESENT, SKYRIM_FUZ_LOOKUP_ERROR
} SkyrimFuzAvailability;

typedef SkyrimFuzAvailability (*SkyrimFuzLookup)(void *context, const char *path);

typedef struct {
    SkyrimFuzLookup lookup;
    void *context;
    const char *stock_paths[SKYRIM_FUZ_STOCK_COUNT]; // One through ten seconds.
    int words_per_second;
    bool skip_empty_responses;
} SkyrimFuzPolicy;

typedef enum {
    SKYRIM_FUZ_KEEP_VOICE, SKYRIM_FUZ_USE_STOCK, SKYRIM_FUZ_SKIP_RESPONSE,
    SKYRIM_FUZ_STOCK_UNAVAILABLE, SKYRIM_FUZ_RESOURCE_FAILURE, SKYRIM_FUZ_UNSUPPORTED_INPUT
} SkyrimFuzDecision;

typedef struct {
    SkyrimFuzDecision decision;
    const char *stock_path; // Borrowed from policy, valid for USE_STOCK.
    unsigned seconds;     // Nonzero only for USE_STOCK.
    bool force_subtitle;  // Effective for this response only.
} SkyrimFuzVoice;

/* The provider resolves authorized Data-relative paths, including any packed
 * stock clips. wav_path is a canonical Data-relative resource name ending in
 * ".wav". The response is NUL-terminated text. Inputs are borrowed for the
 * call, and the callback may not retain a candidate path. The provider
 * performs any resource I/O. The helper has no game engine dependency. */
SkyrimFuzVoice skyrim_fuz_choose_voice(const SkyrimFuzPolicy *policy, const char *wav_path,
                                       const char *response, bool topic_force_subtitle);
#endif
