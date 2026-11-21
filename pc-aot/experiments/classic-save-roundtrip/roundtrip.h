/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_SAVE_ROUNDTRIP_H
#define CLASSIC_SAVE_ROUNDTRIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { ROUNDTRIP_OWNER = 0x368790, ROUNDTRIP_RECORD = 0x45564153, ROUNDTRIP_RECORD_VERSION = 2 };
#define ROUNDTRIP_RECORD_DATA "authored record"
#define ROUNDTRIP_ORIGINAL "authored base input"
#define ROUNDTRIP_REPLACEMENT "modified base input"

void roundtrip_enter_game(void);
void roundtrip_leave_game(void);
void roundtrip_expect_clone(int source, bool unavailable);
void roundtrip_finish_clone(bool attempted);
size_t roundtrip_digest_bytes(void);
int roundtrip_openat(int directory, const char *name, int flags, int mode);
int roundtrip_ioctl(int target, unsigned long request, int source);
ssize_t roundtrip_digest_pread(int fd, void *out, size_t count, off_t offset);
void save_roundtrip_configure(const char *case_name);

#endif
