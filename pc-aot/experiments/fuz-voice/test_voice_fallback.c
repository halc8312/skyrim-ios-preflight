/* SPDX-License-Identifier: GPL-3.0-only */
#include "voice_fallback.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const original_paths[] = {
    "Sound/Voice/Probe.esp/Narrator/Reply.wav",
    "Sound/Voice/Probe.esp/Narrator/Reply.fuz",
    "Sound/Voice/Probe.esp/Narrator/Reply.xwm",
};
static const char *const user_stock[] = {
    "Sound/Voice/OwnerPack/Quiet1.xwm",
    "Sound/Voice/OwnerPack/Quiet2.xwm",
    "Sound/Voice/OwnerPack/Quiet3.xwm",
    "Sound/Voice/OwnerPack/Quiet4.xwm",
    "Sound/Voice/OwnerPack/Quiet5.xwm",
    "Sound/Voice/OwnerPack/Quiet6.xwm",
    "Sound/Voice/OwnerPack/Quiet7.xwm",
    "Sound/Voice/OwnerPack/Quiet8.xwm",
    "Sound/Voice/OwnerPack/Quiet9.xwm",
    "Sound/Voice/OwnerPack/Quiet10.xwm",
};
static const char response[] = "The gate remains closed until sunrise";

typedef struct {
    SkyrimFuzAvailability original[3], stock;
    size_t probed;
    unsigned requested_seconds;
} TestResources;

static SkyrimFuzAvailability find_resource(void *context, const char *path) {
    TestResources *resources = context;
    if (resources->probed < sizeof original_paths / sizeof original_paths[0]) {
        unsigned index = (unsigned)resources->probed++;
        assert(!strcmp(path, original_paths[index]));
        return resources->original[index];
    }
    assert(resources->probed++ == 3);
    for (unsigned i = 0; i < SKYRIM_FUZ_STOCK_COUNT; ++i) {
        if (strcmp(path, user_stock[i])) continue;
        resources->requested_seconds = i + 1;
        return resources->stock;
    }
    assert(false);
    return SKYRIM_FUZ_LOOKUP_ERROR;
}

static SkyrimFuzPolicy settings(TestResources *resources) {
    SkyrimFuzPolicy policy = {
        .lookup = find_resource, .context = resources, .words_per_second = 2,
        .skip_empty_responses = true
    };
    memcpy(policy.stock_paths, user_stock, sizeof user_stock);
    return policy;
}

static SkyrimFuzVoice choose(SkyrimFuzPolicy *policy, const char *text, bool topic_force) {
    TestResources *resources = policy->context;
    resources->probed = 0;
    resources->requested_seconds = 0;
    return skyrim_fuz_choose_voice(policy, original_paths[0], text, topic_force);
}

static void check_stock(SkyrimFuzPolicy *policy, const char *text, unsigned seconds, bool forced) {
    SkyrimFuzVoice result = choose(policy, text, false);
    const TestResources *resources = policy->context;
    assert(result.decision == SKYRIM_FUZ_USE_STOCK && result.seconds == seconds);
    assert(result.stock_path == user_stock[seconds - 1] && result.force_subtitle == forced);
    assert(resources->probed == 4 && resources->requested_seconds == seconds);
}

static void check_unchanged(SkyrimFuzVoice result, SkyrimFuzDecision decision, bool forced) {
    assert(result.decision == decision && !result.stock_path && !result.seconds &&
           result.force_subtitle == forced);
}

static void test_voiced_and_per_response(void) {
    TestResources resources = {.stock = SKYRIM_FUZ_PRESENT};
    SkyrimFuzPolicy policy = settings(&resources);
    check_stock(&policy, response, 4, true);
    for (unsigned format = 0; format < 3; ++format) {
        resources.original[format] = SKYRIM_FUZ_PRESENT;
        check_unchanged(choose(&policy, response, false), SKYRIM_FUZ_KEEP_VOICE, false);
        assert(resources.probed == format + 1);
        check_unchanged(choose(&policy, response, true), SKYRIM_FUZ_KEEP_VOICE, true);
        assert(!resources.requested_seconds);
        resources.original[format] = SKYRIM_FUZ_ABSENT;
    }
    check_stock(&policy, response, 4, true);
}

static void test_duration_and_empty_response(void) {
    TestResources resources = {.stock = SKYRIM_FUZ_PRESENT};
    SkyrimFuzPolicy policy = settings(&resources);
    check_stock(&policy, response, 4, true);
    policy.words_per_second = 3;
    check_stock(&policy, response, 3, true);
    policy.words_per_second = 0;
    check_stock(&policy, response, 4, true);
    policy.words_per_second = -1;
    check_stock(&policy, response, 4, true);
    policy.words_per_second = 1;
    check_stock(&policy, "Alpha Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliet Kilo Lima", 10, true);
    policy.words_per_second = 2;
    check_stock(&policy, "Many   spaces remain here   ", 3, true);
    check_stock(&policy, "Tabs count\ttogether here", 2, true);
    check_stock(&policy, "Hey", 2, false);
    check_stock(&policy, "<ID=0001>", 2, false);

    const char *const skipped[] = {"", " ", "Q", "The last token is x   "};
    for (unsigned i = 0; i < sizeof skipped / sizeof skipped[0]; ++i) {
        check_unchanged(choose(&policy, skipped[i], false), SKYRIM_FUZ_SKIP_RESPONSE, false);
        assert(resources.probed == 3);
    }
    check_unchanged(choose(&policy, "", true), SKYRIM_FUZ_SKIP_RESPONSE, true);
    policy.skip_empty_responses = false;
    check_stock(&policy, " ", 2, false);
    check_unchanged(choose(&policy, "", false), SKYRIM_FUZ_SKIP_RESPONSE, false);
    check_unchanged(choose(&policy, "Q", false), SKYRIM_FUZ_SKIP_RESPONSE, false);
    // LE retains the original string if its tokenizer finds no tokens.
    check_stock(&policy, "     ", 1, true);
}

static void test_unavailable_resources(void) {
    TestResources resources = {0};
    SkyrimFuzPolicy policy = settings(&resources);
    check_unchanged(choose(&policy, response, false), SKYRIM_FUZ_STOCK_UNAVAILABLE, false);
    assert(resources.probed == 4 && resources.requested_seconds == 4);
    check_unchanged(choose(&policy, response, true), SKYRIM_FUZ_STOCK_UNAVAILABLE, true);
    policy.stock_paths[3] = NULL;
    check_unchanged(choose(&policy, response, false), SKYRIM_FUZ_STOCK_UNAVAILABLE, false);
    assert(resources.probed == 3);
    policy.stock_paths[3] = user_stock[3];

    resources.stock = SKYRIM_FUZ_LOOKUP_ERROR;
    check_unchanged(choose(&policy, response, false), SKYRIM_FUZ_RESOURCE_FAILURE, false);
    assert(resources.probed == 4 && resources.requested_seconds == 4);
    resources.stock = SKYRIM_FUZ_PRESENT;
    resources.original[0] = SKYRIM_FUZ_LOOKUP_ERROR;
    check_unchanged(choose(&policy, response, false), SKYRIM_FUZ_RESOURCE_FAILURE, false);
    assert(resources.probed == 3);
    resources.original[2] = SKYRIM_FUZ_PRESENT;
    check_unchanged(choose(&policy, response, false), SKYRIM_FUZ_KEEP_VOICE, false);
    assert(resources.probed == 3);

    resources.probed = 0;
    check_unchanged(skyrim_fuz_choose_voice(&policy, original_paths[1], response, false),
                    SKYRIM_FUZ_UNSUPPORTED_INPUT, false);
    char long_path[300];
    memset(long_path, 'x', sizeof long_path);
    memcpy(long_path + sizeof long_path - 5, ".wav", 5);
    check_unchanged(skyrim_fuz_choose_voice(&policy, long_path, response, false),
                    SKYRIM_FUZ_UNSUPPORTED_INPUT, false);
    assert(!resources.probed);
}

int main(void) {
    test_voiced_and_per_response();
    test_duration_and_empty_response();
    test_unavailable_resources();
    puts("LE missing-voice decision fixture passed.");
    return 0;
}
