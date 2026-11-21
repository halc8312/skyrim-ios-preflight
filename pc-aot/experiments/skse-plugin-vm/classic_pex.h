/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_CLASSIC_PEX_H
#define SKYRIM_EXPERIMENT_CLASSIC_PEX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "x86_classic_catalog.h"

enum { SKSE_PEX_MAX_BYTES = 8 << 20, SKSE_PEX_MAX_ARGS = 8, SKSE_PEX_MAX_ROWS = 512, SKSE_PEX_MAX_CALLS = 4096 };

typedef struct {
    const uint8_t *bytes;
    uint16_t length;
} SksePexText;

typedef struct {
    SksePexText script, state, name, result, arguments[SKSE_PEX_MAX_ARGS];
    uint8_t count;
    bool global;
} SksePexNative;

typedef struct {
    SksePexText script, name;
    uint16_t count;
} SksePexStaticCall;

typedef struct {
    SksePexNative *natives;
    size_t native_capacity, native_count;
    SksePexStaticCall *calls;
    size_t call_capacity, call_count;
} SksePexInventory;

typedef enum {
    SKSE_PEX_OK, SKSE_PEX_MALFORMED, SKSE_PEX_UNSUPPORTED, SKSE_PEX_LIMIT, SKSE_PEX_NOMEM
} SksePexStatus;

/* Examine one TESV-format PEX. Null output tables skip the corresponding
 * inventory. The static call table describes encoded call sites, without
 * reachability or instance-method inference. Text borrows the immutable input.
 * On failure both counts are zero. */
SksePexStatus skse_pex_inspect(const uint8_t *bytes, size_t length, SksePexInventory *inventory);
bool skse_pex_text_equal(SksePexText text, const char *ascii);

typedef enum {
    SKSE_PEX_MATCH, SKSE_PEX_MISSING, SKSE_PEX_AMBIGUOUS,
    SKSE_PEX_DIFFERENT_SIGNATURE, SKSE_PEX_UNMODELED_TYPE, SKSE_PEX_NAMED_STATE
} SksePexMatch;

/* Match the validated native declaration against one captured registry.
 * A match verifies the registration ABI only. It never admits a VM call. */
SksePexMatch skse_pex_match(const SksePexNative *native, const SkseClassicCatalog *catalog,
                            const SkseClassicDeclaration **matched);
/* Require the call's static class, name and arity to agree with a single
 * declaration from the selected scripts before verifying the DLL row. */
SksePexMatch skse_pex_match_call(const SksePexStaticCall *call, const SksePexNative *declarations,
                                 size_t count, const SkseClassicCatalog *catalog,
                                 const SkseClassicDeclaration **matched);
#endif
