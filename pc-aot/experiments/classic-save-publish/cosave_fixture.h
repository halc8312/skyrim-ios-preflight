/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_SAVE_COSAVE_FIXTURE_H
#define CLASSIC_SAVE_COSAVE_FIXTURE_H
#include <stdbool.h>

void save_composition_open(const char *directory);
void save_composition_begin(void);
void save_composition_record(void);
void save_composition_finish(int published_fd, bool vm_saved);
void save_composition_close(void);

#endif
