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
#ifdef __cplusplus
}
#endif
