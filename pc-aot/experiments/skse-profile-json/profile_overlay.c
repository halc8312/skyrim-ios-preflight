/* SPDX-License-Identifier: GPL-3.0-only */
#include "profile_overlay.h"

#include <stdlib.h>

typedef SkseProfileJsonResult Result;

struct SkseProfileJsonOverlay {
    const SkseProfileJsonStore *store;
    SkseProfileJsonIdentity identity;
};

SkseProfileJsonResult skse_profile_json_overlay_open(const SkseProfileJsonStore *store,
                                                    const SkseProfileJsonIdentity *identity, SkseProfileJsonOverlay **out) {
    if (!out) return SKSE_PROFILE_JSON_ARGUMENT;
    *out = NULL;
    if (!store || !identity) return SKSE_PROFILE_JSON_ARGUMENT;
    if (!skse_profile_json_matches(store, identity)) return SKSE_PROFILE_JSON_SCOPE;
    SkseProfileJsonOverlay *overlay = malloc(sizeof(*overlay));
    if (!overlay) return SKSE_PROFILE_JSON_NO_MEMORY;
    *overlay = (SkseProfileJsonOverlay){.store = store, .identity = *identity};
    *out = overlay;
    return SKSE_PROFILE_JSON_OK;
}

void skse_profile_json_overlay_close(SkseProfileJsonOverlay *overlay) {
    free(overlay);
}

static uint32_t windows_error(Result status) {
    switch (status) {
    case SKSE_PROFILE_JSON_OK: return 0;
    case SKSE_PROFILE_JSON_MISSING: case SKSE_PROFILE_JSON_SCOPE: case SKSE_PROFILE_JSON_ARGUMENT:
        return 2; // ERROR_FILE_NOT_FOUND for names outside the profile provider.
    case SKSE_PROFILE_JSON_CONFLICT: return 5;
    case SKSE_PROFILE_JSON_NO_MEMORY: return 8;
    case SKSE_PROFILE_JSON_LIMIT: return 223;
    default: return 13; // ERROR_INVALID_DATA, including unstable snapshots.
    }
}

static uint32_t profile_read(void *context, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    const SkseProfileJsonOverlay *overlay = context;
    return windows_error(skse_profile_json_read(overlay->store, &overlay->identity, path, limit, bytes, length));
}

static uint32_t profile_attributes(void *context, const char *path, uint32_t *attributes) {
    const SkseProfileJsonOverlay *overlay = context;
    int directory = 0;
    Result status = skse_profile_json_attributes(overlay->store, &overlay->identity, path, &directory);
    if (!status) *attributes = directory ? SKSE_WIN_ATTRIBUTE_DIRECTORY : SKSE_WIN_ATTRIBUTE_NORMAL;
    return windows_error(status);
}

static bool writable_path(void *context, const char *path) {
    const SkseProfileJsonOverlay *overlay = context;
    return skse_profile_json_allows(overlay->store, &overlay->identity, path);
}

SkseWinProfile skse_profile_json_overlay_provider(SkseProfileJsonOverlay *overlay) {
    if (!overlay) return (SkseWinProfile){0};
    return (SkseWinProfile){.input = {profile_read, profile_attributes, overlay}, .allow_write = writable_path};
}

void skse_profile_json_batch_discard(SkseProfileJsonBatch *batch) {
    if (!batch) return;
    for (size_t i = 0; i < batch->count; ++i) skse_profile_json_discard(batch->entries[i]);
    *batch = (SkseProfileJsonBatch){0};
}

SkseProfileJsonCaptureResult skse_profile_json_overlay_capture(SkseProfileJsonOverlay *overlay, SkseWinFiles *files,
                                                               bool callback_succeeded, SkseProfileJsonBatch *batch) {
    if (!batch) return (SkseProfileJsonCaptureResult){.storage_error = SKSE_PROFILE_JSON_ARGUMENT};
    *batch = (SkseProfileJsonBatch){0};
    if (!overlay || !files || files->profile.input.context != overlay || files->profile.input.read != profile_read ||
        files->profile.input.attributes != profile_attributes || files->profile.allow_write != writable_path)
        return (SkseProfileJsonCaptureResult){.storage_error = SKSE_PROFILE_JSON_SCOPE};
    SkseWinProfileOutputs outputs;
    uint32_t guest_error = skse_win_profile_closed_outputs(files, callback_succeeded, &outputs);
    if (guest_error) return (SkseProfileJsonCaptureResult){.guest_error = guest_error};

    SkseProfileJsonBatch ready = {0};
    for (size_t i = 0; i < outputs.count; ++i) {
        const SkseWinFileData *src = outputs.entries[i];
        Result result = skse_profile_json_capture(overlay->store, &overlay->identity, src->path, src->bytes, src->length,
                                                 &ready.entries[ready.count]);
        if (result) {
            skse_profile_json_batch_discard(&ready);
            return (SkseProfileJsonCaptureResult){.storage_error = result};
        }
        ++ready.count;
    }
    *batch = ready;
    return (SkseProfileJsonCaptureResult){0};
}
