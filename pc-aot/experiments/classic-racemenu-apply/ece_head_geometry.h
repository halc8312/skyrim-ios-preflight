/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "ece_nose_witness.h"

/* The caller holds the current FaceGen root and its child list stable for
 * this synchronous read. The result is borrowed within the same scope.
 * A complete search reports zero when the stock lookup or cast misses.
 * Unknown dispatch, an incomplete read or a changed edge leaves out alone. */
bool ece_head_geometry(EceNoseRead read, void *context, uint32_t face, uint32_t name, uint32_t *out);

enum { ECE_HEAD_PART_LIMIT = 32 };

typedef struct {
    uint32_t part, geometry, model_data, tri_path;
    uint16_t vertices;
} EceHeadPart;

typedef struct {
    unsigned count;
    EceHeadPart parts[ECE_HEAD_PART_LIMIT];
} EceHeadLayout;

/* Read the list/count already selected by the stock FaceGen caller. Empty
 * model or TRI paths are proven morph skips. Any unresolved participating
 * part fails the whole collection and leaves out alone. All output types
 * are borrowed from the caller's current retained FaceGen update. */
bool ece_head_layout(EceNoseRead read, void *context, uint32_t face, uint32_t list,
                     uint32_t count, EceHeadLayout *out);
