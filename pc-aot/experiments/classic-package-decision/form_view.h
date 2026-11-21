/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_FORM_VIEW_H
#define CLASSIC_FORM_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CPU;

/* The checked engine adapter fills exactly the requested bytes on success.
 * The lookup returns a guest address or zero. Neither changes CPU state. */
bool classic_engine_read_data(const struct CPU *, uint32_t address, uint8_t *, size_t);
uint32_t classic_engine_lookup_form(const struct CPU *, uint32_t form_id);

/* SKSE 1.7.3 GameForms.h. */
enum {
    CLASSIC_REFERENCE_FORM = 0x3d,
    CLASSIC_CHARACTER_FORM = 0x3e,
    CLASSIC_IDLE_FORM = 0x4e,
    CLASSIC_PACKAGE_FORM = 0x4f,
};

static inline uint32_t classic_read_le32(const uint8_t bytes[4]) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

bool classic_read_form_id(const struct CPU *, uint32_t address, uint8_t type, uint32_t *form_id);

#endif
