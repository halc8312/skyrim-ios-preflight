/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_CLASSIC_SCALAR_H
#define SKYRIM_EXPERIMENT_CLASSIC_SCALAR_H
#include "x86_call.h"

typedef struct {
    uint32_t type, bits;
    const char *text;
    size_t text_size;   /* Includes the terminating NUL. */
} SkseClassicScalar;

typedef struct {
    uint32_t type, bits;
    char text[SKSE_CLASSIC_FRAME_STRING_BYTES];
} SkseClassicScalarResult;

typedef enum {
    SKSE_SCALAR_OK, SKSE_SCALAR_ARGUMENT, SKSE_SCALAR_MEMORY, SKSE_SCALAR_EXECUTION, SKSE_SCALAR_RESULT
} SkseClassicScalarStatus;

typedef struct {
    const SkseClassicCatalog *catalog;
    const SkseClassicDeclaration *prepare_method; /* Optional read-only preparation capability. */
    SksePeImage *const *images;
    size_t image_count;
    SkseX86Scratch scratch;
    const SkseX86Services *services;
    uint32_t epoch, stack_id;
    uint32_t arguments_base, text_base;
} SkseClassicScalarScope;

/* Single-owner hosted scope. The catalog and registry must remain alive for
 * the call. The host assigns a new epoch before reusing a registry address.
 * Output and trace use distinct storage. A failed call leaves output unchanged.
 * Execution or result failure seals the trial and invalidates private file output. */
SkseClassicScalarStatus skse_classic_scalar_call(const SkseClassicScalarScope *,
    const SkseClassicDeclaration *, const SkseClassicScalar *, size_t count, uint32_t work_limit,
    bool preparing, SkseClassicScalarResult *output, SkseX86Result *trace);
#endif
