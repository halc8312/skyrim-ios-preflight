/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

struct CPU;

enum SkyrimSaveNotice {
    SkyrimSaveOpenFinished = 1,
    SkyrimSaveSlotEntered,
    SkyrimSaveSlotLeft,
    SkyrimSaveCloseEntered,
    SkyrimSaveCloseLeft,
    SkyrimSaveVmReturned,
    SkyrimSaveFlushReturned,
    SkyrimSaveCrtCloseReturned,
    SkyrimSaveMoveEntered,
    SkyrimSaveCrtMoveReturned,
    SkyrimSaveAttemptLeft,
};

enum SkyrimSaveNestedNotice {
    SkyrimSaveInSlot = 1,
    SkyrimSaveInClose,
};

/* Future optional guest-to-host binding point. The host must authenticate
 * the CPU, spilled stack frame and original .ess.tmp path against the
 * active profile before returning a nonzero attempt. The callback owns no
 * guest memory and must not modify the original call's registers or frame. */
uint64_t skyrim_save_notify_begin(const struct CPU *cpu, uint32_t stack, uint32_t temporary_path);

/* Returns the active attempt for an authenticated nested save-slot or close
 * call, or zero. Its result is captured before the actual engine call. */
uint64_t skyrim_save_notify_current(const struct CPU *cpu, unsigned phase);

/* An attempt remains local to its launch/owner. Each callback must reject a
 * stale ticket. A zero ticket has no effect. On a nonlocal guest exit the
 * host must also release the attempt. The slot/attempt leave events carry no
 * success claim. A valid publication also requires actual file-wrapper
 * write, close and rename results and a retained matching .ess reader. */
void skyrim_save_notify_event(const struct CPU *cpu, uint64_t attempt, unsigned site, int succeeded);
