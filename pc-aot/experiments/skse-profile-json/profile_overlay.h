/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "profile_json.h"
#include "../skse-plugin-vm/x86_winfile.h"

typedef struct SkseProfileJsonOverlay SkseProfileJsonOverlay;

typedef struct {
    SkseProfileJsonSnapshot *entries[SKSE_WIN_OUTPUT_FILES];
    size_t count;
} SkseProfileJsonBatch;

typedef struct {
    uint32_t guest_error; /* Win32 error from the session's close/permission checks. */
    SkseProfileJsonResult storage_error;
} SkseProfileJsonCaptureResult;

/* Bind immutable profile input and write policy to an existing private store.
 * The store and overlay must remain alive until the VM session is closed. */
SkseProfileJsonResult skse_profile_json_overlay_open(const SkseProfileJsonStore *, const SkseProfileJsonIdentity *,
                                                    SkseProfileJsonOverlay **);
SkseWinProfile skse_profile_json_overlay_provider(SkseProfileJsonOverlay *);
void skse_profile_json_overlay_close(SkseProfileJsonOverlay *);

/* Stage all authorized closed outputs or none. No file on disk changes.
 * A result with both errors clear and count zero is an accepted no-op.
 * Publish individual owned entries explicitly through profile_json.h. */
SkseProfileJsonCaptureResult skse_profile_json_overlay_capture(SkseProfileJsonOverlay *, SkseWinFiles *,
                                                               bool callback_succeeded, SkseProfileJsonBatch *);
void skse_profile_json_batch_discard(SkseProfileJsonBatch *);
