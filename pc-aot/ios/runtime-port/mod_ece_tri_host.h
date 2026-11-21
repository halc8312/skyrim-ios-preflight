/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "mod_ece_profile.h"
#include <sys/types.h>

struct CPU;

void skyrim_ece_tri_allow_input(const char *sha256, bool detailed);
bool skyrim_ece_tri_configure(int data, SkyrimEceProfileDigest digest);
void skyrim_ece_tri_retire(void);

uint64_t skyrim_ece_tri_app_begin(const struct CPU *cpu, uint32_t path, uint32_t stream);
void skyrim_ece_tri_app_finish(const struct CPU *cpu, uint64_t ticket, uint8_t parsed);

/* Prepared Kernel32 ReadFile, after the original syscall. */
extern _Thread_local bool skyrim_ece_tri_read_pending;
void skyrim_ece_tri_app_read(const struct CPU *cpu, uint32_t caller, uint32_t handle, int fd, ssize_t bytes);
