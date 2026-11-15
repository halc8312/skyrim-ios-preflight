/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Only call save on a background worker. Cancellation is checked between API
 * and I/O operations; an in-progress driver call is not forcibly interrupted. */
uint64_t skyrim_pipeline_checkpoint_begin(void);
void skyrim_pipeline_checkpoint_cancel(uint64_t ticket);
void skyrim_pipeline_checkpoint_save(uint64_t ticket);
/* Opt-in admission only. Caller combines the startup option with visibility
 * of native Settings; false is the default. Showing Settings does not pause
 * the game. This setter never waits for the registry, driver or file I/O. */
void skyrim_pipeline_checkpoint_foreground_permission(int allowed);
/* Worker only: returns 0 for busy/clean/recent compilation/cooldown. The caller
 * must save or cancel every nonzero ticket. New compile work or loss of
 * permission cancels later steps, not an already-running driver/fsync call. */
uint64_t skyrim_pipeline_checkpoint_foreground_begin(void);
#ifdef __cplusplus
}
#endif
