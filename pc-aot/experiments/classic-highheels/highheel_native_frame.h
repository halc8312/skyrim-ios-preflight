/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "highheel_effect.h"

struct CPU;

typedef struct {
    uint64_t epoch;
    uint32_t registry, effect_class;
    uint32_t keep_update, stop_update;
    uint32_t move_up, on_update, move_down;
} HighHeelNativeBindings;

/* The visitor borrows the address of the caller's typed self VMValue for
 * the duration of the callback. It must keep the current frame alive and
 * must consume the address synchronously. */
typedef bool (*HighHeelVisitSelf)(void *context, uint32_t value);

/* Call inside the stock ordinary Run callback. The host supplies its actual
 * current CPU, the Run's register values, the current VM epoch, and class
 * and function identities already attested for that epoch. The reader must
 * copy only readable guest memory. A rejected call leaves the visitor idle. */
bool highheel_visit_native_self(const HighHeelNativeBindings *bindings, uint64_t current_epoch,
                                const struct CPU *cpu, const struct CPU *current_cpu,
                                uint32_t native_object, uint32_t sp, HighHeelEffectRead read,
                                void *read_context, HighHeelVisitSelf visit, void *visit_context);
