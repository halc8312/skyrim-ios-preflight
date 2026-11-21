/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_PACKAGE_CHANGE_H
#define CLASSIC_PACKAGE_CHANGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t actor_id;
    uint32_t outgoing_package_id;
} ClassicPackageChange;

bool classic_package_change_subscribed(void);
void classic_package_change_notify(ClassicPackageChange);

struct CPU;
void classic_package_change_before_assign(const struct CPU *, uint32_t event_address);

#endif
