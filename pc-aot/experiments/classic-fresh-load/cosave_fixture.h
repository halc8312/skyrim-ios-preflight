/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_FRESH_COSAVE_FIXTURE_H
#define CLASSIC_FRESH_COSAVE_FIXTURE_H

#include "fresh_load.h"

enum ClassicCosaveCase {
    COSAVE_ACCEPT, COSAVE_VM_REJECT, COSAVE_NO_REVERT, COSAVE_NO_VM,
    COSAVE_TICKET_SHIFT, COSAVE_WRONG_FD, COSAVE_BASE_REJECT, COSAVE_NO_SLOT_READ,
};

void classic_cosave_fixture_init(const char *directory, const char *selected);
void classic_cosave_fixture_begin(enum ClassicCosaveCase);
int classic_cosave_fixture_load_fd(int original);
size_t classic_cosave_fixture_read(int fd, bool in_slot);
void classic_cosave_fixture_after_metadata(void);
void classic_cosave_fixture_vm(const struct CPU *, uint32_t stream);
void classic_cosave_fixture_finish(ClassicFreshReport, const char *other);
void classic_cosave_fixture_close(void);

#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
void classic_cosave_roundtrip_configure(const char *expected);
bool classic_cosave_roundtrip_cached(void);
bool classic_cosave_roundtrip_same_input(int fd);
#endif

#endif
