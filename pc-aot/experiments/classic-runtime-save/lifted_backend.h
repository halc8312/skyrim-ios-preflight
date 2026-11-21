/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_RUNTIME_LIFTED_BACKEND_H
#define CLASSIC_RUNTIME_LIFTED_BACKEND_H
#include "runtime_save.h"

void fixture_runtime_begin(struct CPU *, int dir, const char *parent, const char *child, const char *final_leaf);
int fixture_runtime_open(struct CPU *, const char *temp);
ssize_t fixture_runtime_write(struct CPU *, const char *data, size_t count, bool short_write);
ssize_t fixture_runtime_write_buffer(struct CPU *, uint32_t buffer, size_t count, bool short_write);
int fixture_runtime_close(struct CPU *);
int fixture_runtime_rename(struct CPU *, const char *from, const char *to);
uint32_t fixture_runtime_last_error(struct CPU *);
int fixture_runtime_reader(void);
void fixture_runtime_end(int expected_reader);

#endif
