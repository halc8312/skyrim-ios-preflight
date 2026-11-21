/* SPDX-License-Identifier: GPL-3.0-only */
/* Authored hosted stand-in for the private lift prelude. It declares only
 * the CPU fields and guest accessors that lift_resource_lookup.c uses. The
 * private tests compile the same provider against the reviewed prelude. */
#ifndef SKYRIM_FUZ_HOSTED_LIFT_PRELUDE_H
#define SKYRIM_FUZ_HOSTED_LIFT_PRELUDE_H
#include <stdint.h>

typedef struct CPU {
    uint32_t r[8];
    uint32_t fs_base;
} CPU;

extern uint8_t *MEM;

/* Little-endian hosts with a 4-byte aligned guest base, as for the runtime. */
#define M32(address) (*(uint32_t *)(void *)(MEM + (address)))
#define M8(address) (*(uint8_t *)(MEM + (address)))
#endif
