/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKYRIM_CLASSIC_FIXTURE_OK,
    SKYRIM_CLASSIC_FIXTURE_ARGUMENT,
    SKYRIM_CLASSIC_FIXTURE_INPUT,
    SKYRIM_CLASSIC_FIXTURE_EXISTS,
    SKYRIM_CLASSIC_FIXTURE_BUSY,
    SKYRIM_CLASSIC_FIXTURE_MEMORY,
    SKYRIM_CLASSIC_FIXTURE_CALLBACK,
    SKYRIM_CLASSIC_FIXTURE_IO,
    SKYRIM_CLASSIC_FIXTURE_OUTPUT
} SkyrimClassicFixtureStatus;

typedef struct {
    uint32_t input_value;
    uint32_t reverted_value; /* First save after Revert. The plugin adds five on each save. */
    uint32_t output_value;
    uint32_t records_read;
    uint32_t callback_mask;  /* Bits 0, 1, 2 represent Revert, Save and Load. */
} SkyrimClassicFixtureResult;

/* Input must exactly match the small file written by the authored Windows test.
 * The output must be a new .skse inside the caller's experiment directory.
 * Result fields are populated only after the verified output is published. */
SkyrimClassicFixtureStatus skyrim_classic_fixture_run(const char *input, const char *output,
                                                     SkyrimClassicFixtureResult *result);
const char *skyrim_classic_fixture_message(SkyrimClassicFixtureStatus status);

#ifdef __cplusplus
}
#endif
