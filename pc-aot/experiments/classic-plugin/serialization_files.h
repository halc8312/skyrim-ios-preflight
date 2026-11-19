/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    FIXTURE_SERIAL_UID = 0x46585452u, FIXTURE_SERIAL_TYPE = 0x56414c55u, FIXTURE_SERIAL_VERSION = 1
};
/* Host-side files for this one authored DLL. Previous may be NULL for a fresh
 * .skse. Destination must be new. Both return 1 on success. */
int fixture_store_record(const char *previous, const char *destination, uint32_t value);
int fixture_load_record(const char *source, uint32_t *value);
#ifdef __cplusplus
}
#endif
