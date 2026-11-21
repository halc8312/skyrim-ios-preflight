/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_PACKAGE_DECISION_H
#define CLASSIC_PACKAGE_DECISION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t actor_id;
    uint32_t original_package_id; /* Zero when the engine chose no package. */
} ClassicPackageDecision;

typedef struct {
    bool present;
    uint32_t package_id;
} ClassicPackageReplacement;

/* These symbols are linked into the hosted fixture. The subscriber receives
 * copied FormIDs and has no guest addresses, CPU access, or memory writer. */
bool classic_package_decision_subscribed(void);
ClassicPackageReplacement classic_package_decision_choose(ClassicPackageDecision);

struct CPU;

uint32_t classic_package_decision_after_call(const struct CPU *, uint32_t actor,
                                             uint32_t original_package);

#endif
