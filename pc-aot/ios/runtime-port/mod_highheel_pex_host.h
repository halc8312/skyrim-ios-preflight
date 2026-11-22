/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "PcGuestModHooksABI.h"
#include <sys/types.h>

typedef bool (*SkyrimHighHeelPexDigest)(const void *, size_t, uint8_t result[32]);
void skyrim_highheel_pex_configure(SkyrimHighHeelPexDigest);
void skyrim_highheel_pex_fill(PcGuestModHooks *);
bool skyrim_highheel_pex_linked_in(uint64_t epoch);
extern _Thread_local bool skyrim_highheel_pex_read_pending;
void skyrim_highheel_pex_read(const struct CPU *, uint32_t caller, uint32_t handle, int fd, ssize_t count);
