/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "digest.h"

typedef struct SkseImmutableEss SkseImmutableEss;

/* Internal to the event bridge. The supplied directory is a private,
 * caller-owned scratch directory dedicated to this one capture. The closed
 * handle never retains a reference to the source file's pathname. */
SksePairResult skse_ess_capture_immutable(int source, int scratch_dir, SkseImmutableEss **out);
/* Consume the snapshot wrapper. The returned anonymous read-only input
 * starts at byte zero and has a seek position independent of the source. */
int skse_ess_take_immutable_fd(SkseImmutableEss *snapshot);
void skse_ess_release_immutable(SkseImmutableEss *snapshot);
SksePairResult skse_ess_read_immutable(SkseImmutableEss *snapshot, SkseEssDigest *out);
SksePairResult skse_pair_commit_immutable(SksePairStore *store, SksePairStage *stage,
                                         const SksePairKey *expected, SkseImmutableEss *snapshot);
